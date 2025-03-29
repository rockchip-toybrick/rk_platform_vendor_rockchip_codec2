/*
 * Copyright 2025 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKYolov5Session"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <cutils/properties.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBufferAllocator.h>

#include "C2RKYolov5Session.h"
#include "C2RKPostProcess.h"
#include "C2RKGrallocOps.h"
#include "C2RKRknnWrapper.h"
#include "C2RKEasyTimer.h"
#include "C2RKLog.h"

namespace android {

#define DEFAULT_MODEL_PATH          "/data/video/yolov5n_seg_for3576.rknn"
#define MAX_SUPPORT_SIZE            3840*2160
#define MAX_RKNN_OUTPUT_SIZE        7

#define PROPERTY_NAME_MODEL_PATH    "codec2_yolov5_model_path"
#define PROPERTY_NAME_ENABLE_RECT   "codec2_yolov5_enable_draw_rect"

const char *toStr_TensorFormat(rknn_tensor_format fmt) {
    switch(fmt) {
        case RKNN_TENSOR_NCHW:      return "NCHW";
        case RKNN_TENSOR_NHWC:      return "NHWC";
        case RKNN_TENSOR_NC1HWC2:   return "NC1HWC2";
        case RKNN_TENSOR_UNDEFINED: return "UNDEFINED";
        default:                    return "UNKNOW";
    }
}

const char* toStr_TensorType(rknn_tensor_type type) {
    switch(type) {
        case RKNN_TENSOR_FLOAT32:   return "TENSOR_FLOAT32";
        case RKNN_TENSOR_FLOAT16:   return "TENSOR_FLOAT16";
        case RKNN_TENSOR_INT8:      return "TENSOR_INT8";
        case RKNN_TENSOR_UINT8:     return "TENSOR_UINT8";
        case RKNN_TENSOR_INT16:     return "TENSOR_INT16";
        default:                    return "UNKNOW";
    }
}

void dumpTensorAttr(rknn_tensor_attr *attr) {
    if (nullptr == attr) {
        c2_err("invalid rknn_tensor_attr");
        return;
    }

    c2_trace("\t index    : %d", attr->index);
    c2_trace("\t name     : %s",  attr->name);
    c2_trace("\t n_dims   : %d dims = [%d %d %d %d]",
              attr->n_dims, attr->dims[3], attr->dims[2],
              attr->dims[1], attr->dims[0]);
    c2_trace("\t n_elems  : %d", attr->n_elems);
    c2_trace("\t size     : %d", attr->size);
    c2_trace("\t fmt      : %d fmd name = %s",
             attr->fmt, toStr_TensorFormat(attr->fmt));
    c2_trace("\t type     : %d type name = %s",
              attr->type, toStr_TensorType(attr->type));
    c2_trace("\t qnt_type : %d", attr->qnt_type);
    c2_trace("\t fl       : %d", attr->fl);
    c2_trace("\t zp       : %d", attr->zp);
    c2_trace("\t scale    : %f", attr->scale);
    c2_trace("\n");
}

void* loadModelFile(int32_t *size) {
    FILE *fp = nullptr;
    char path[PROPERTY_VALUE_MAX];

    if (property_get(PROPERTY_NAME_MODEL_PATH, path, "") <= 0) {
        // use default yolov5 model path
        memcpy(path, DEFAULT_MODEL_PATH, sizeof(DEFAULT_MODEL_PATH));
    }

    // open file
    fp = fopen(path, "rb");
    if (fp == nullptr) {
        c2_err("failed to open file %s, err %s", path, strerror(errno));
        return nullptr;
    }

    // get the length of file
    fseek(fp, 0, SEEK_END);

    int32_t length = ftell(fp);
    uint8_t* model = (uint8_t*)malloc(length * sizeof(uint8_t));
    if (model == nullptr) {
        c2_err("failed to malloc model data");
        fclose(fp);
        return nullptr;
    }

    // read file
    fseek(fp, 0, SEEK_SET);
    if (fread(model, 1, length, fp) < 0) {
        c2_err("failed to fread model file");
        free(model);
        fclose(fp);
        return nullptr;
    }
    *size = length;
    fclose(fp);

    c2_info("rknn load model(%s)", path);
    return model;
}

bool C2RKYolov5Session::RknnOutput::isIdle() {
    return mStatus == IDLE;
}

void C2RKYolov5Session::RknnOutput::setStatus(Status status) {
    mStatus = status;
}

void C2RKYolov5Session::BaseProcessHandler::pendingProcess(RknnOutput *nnOutput) {
    sp<AMessage> msg = new AMessage(kWhatProcess, this);
    msg->setPointer("nnOutput", nnOutput);
    msg->post();
}

void C2RKYolov5Session::BaseProcessHandler::stopHandler() {
    mRunning = false;
    sp<AMessage> reply;
    (new AMessage(kWhatStop, this))->postAndAwaitResponse(&reply);
}

void C2RKYolov5Session::BaseProcessHandler::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatProcess: {
            if (mRunning && mThiz) {
                RknnOutput *nnOutput = nullptr;
                if (msg->findPointer("nnOutput", (void **)&nnOutput)) {
                    onDoProcess(nnOutput);
                }
            } else {
                c2_trace("Ignore process message as we're not running");
            }
        } break;
        case kWhatStop: {
            mRunning = false;

            /* post response */
            sp<AReplyToken> replyID;
            sp<AMessage> response = new AMessage;
            msg->senderAwaitsResponse(&replyID);
            response->postReply(replyID);
        } break;
        default: {
            c2_err("Unrecognized msg: %d", msg->what());
        } break;
    }
}

C2RKYolov5Session::C2RKYolov5Session() :
    mOps(C2RKRknnWrapper::get()),
    mRknnCtx(0),
    mInput(nullptr),
    mInputAttrs(nullptr),
    mOutputAttrs(nullptr),
    mPostProcessContext(nullptr),
    mIsHEVC(false),
    mCallback(nullptr) {
    mDrawRect = (bool) property_get_bool(PROPERTY_NAME_ENABLE_RECT, 0);
}

C2RKYolov5Session::~C2RKYolov5Session() {
    disconnect();
}

bool C2RKYolov5Session::disconnect() {
    stopPostProcessLooper();

    releaseRknnOutputs();

    C2_SAFE_FREE(mInput);
    C2_SAFE_FREE(mInputAttrs);
    C2_SAFE_FREE(mOutputAttrs);

    if (mRknnCtx != 0) {
        mOps->rknnDestory(mRknnCtx);
        mRknnCtx = 0;
    }
    if (mPostProcessContext != nullptr) {
        c2_postprocess_deinit_context(mPostProcessContext);
        mPostProcessContext = nullptr;
    }
    return true;
}

/* multi-thread is used for share the execution time */
bool C2RKYolov5Session::startPostProcessLooper() {
    status_t err = OK;

    if (mRknnRunLooper == nullptr) {
        mRknnRunLooper = new ALooper;
        mRknnRunHandler = new RknnRunHandler(this);

        mRknnRunLooper->setName("C2RknnRunLooper");
        err = mRknnRunLooper->start();
        if (err == OK) {
            mRknnRunLooper->registerHandler(mRknnRunHandler);
        } else {
            return err;
        }
    }
    if (mPostProcessLooper == nullptr) {
        mPostProcessLooper = new ALooper;
        mPostProcessHandler = new PostProcessHandler(this);

        mPostProcessLooper->setName("C2PostProcessLooper");
        err = mPostProcessLooper->start();
        if (err == OK) {
            mPostProcessLooper->registerHandler(mPostProcessHandler);
        } else {
            return err;
        }
    }
    if (mResultLooper == nullptr) {
        mResultLooper = new ALooper;
        mResultHandler = new ResultHandler(this);

        mResultLooper->setName("C2ResultLooper");
        err = mResultLooper->start();
        if (err == OK) {
            mResultLooper->registerHandler(mResultHandler);
        }
    }
    return (err == OK);
}

bool C2RKYolov5Session::stopPostProcessLooper() {
    if (mRknnRunLooper != nullptr) {
        mRknnRunHandler->stopHandler();
        mRknnRunLooper->unregisterHandler(mRknnRunHandler->id());
        mRknnRunHandler.clear();

        mRknnRunLooper->stop();
        mRknnRunLooper.clear();
    }
    if (mPostProcessLooper != nullptr) {
        mPostProcessHandler->stopHandler();
        mPostProcessLooper->unregisterHandler(mPostProcessHandler->id());
        mPostProcessHandler.clear();

        mPostProcessLooper->stop();
        mPostProcessLooper.clear();
    }
    if (mResultLooper != nullptr) {
        mResultHandler->stopHandler();
        mResultLooper->unregisterHandler(mResultHandler->id());
        mResultHandler.clear();

        mResultLooper->stop();
        mResultLooper.clear();
    }

    return true;
}

void C2RKYolov5Session::initRknnOutputs() {
    if (mRknnOutputs.size() >= MAX_RKNN_OUTPUT_SIZE)
        return;

    Mutex::Autolock autoLock(mLock);

    int32_t nOutputs = mNumIO.n_output;
    rknn_tensor_attr *attr = mOutputAttrs;

    for (int j = 0; j < MAX_RKNN_OUTPUT_SIZE; j++) {
        rknn_output *output = (rknn_output*)calloc(1, nOutputs * sizeof(rknn_output));
        for (int i = 0; i < nOutputs; i++) {
            output[i].index = i;
            output[i].want_float = attr->qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
                                   attr->type == RKNN_TENSOR_FLOAT16;
            output[i].size = SEG_OUT_BUF_SIZE;
            output[i].is_prealloc = 1;

            if (output[i].is_prealloc) {
                output[i].buf = calloc(1, output[i].size);
            }
        }

        objectDetectResultList *odResults = new objectDetectResultList;
        odResults->resultsSeg[0].segMask = (uint8_t *)malloc(MAX_SUPPORT_SIZE);

        RknnOutput *nnOutput  = new RknnOutput;
        nnOutput->mIndex      = j;
        nnOutput->mStatus     = RknnOutput::IDLE;
        nnOutput->mOutput     = output;
        nnOutput->mInImage    = new ImageBuffer;
        nnOutput->mCopyImage  = new ImageBuffer;
        nnOutput->mOdResults  = (void *)odResults;

        // init required model size 640x640, RGB888
        nnOutput->mInModelPtr = malloc(SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT * 3);

        memset(nnOutput->mInImage, 0, sizeof(ImageBuffer));
        memset(nnOutput->mCopyImage, 0, sizeof(ImageBuffer));

        mRknnOutputs.push(nnOutput);
    }
}

void C2RKYolov5Session::releaseRknnOutputs() {
    Mutex::Autolock autoLock(mLock);

    while (!mRknnOutputs.isEmpty()) {
        RknnOutput *nnOutput = mRknnOutputs.editItemAt(0);
        if (nnOutput != nullptr) {
            rknn_output *output =  nnOutput->mOutput;
            for (int i = 0; i < mNumIO.n_output; i++) {
                C2_SAFE_FREE(output[i].buf);
            }
            if (nnOutput->mCopyImage && nnOutput->mCopyImage->handle) {
                GraphicBufferAllocator::get().free(
                        (buffer_handle_t)nnOutput->mCopyImage->handle);
            }
            C2_SAFE_DELETE(nnOutput->mInImage);
            C2_SAFE_DELETE(nnOutput->mCopyImage);

            if (nnOutput->mOdResults) {
                objectDetectResultList *odResults =
                        (objectDetectResultList *)nnOutput->mOdResults;
                C2_SAFE_FREE(odResults->resultsSeg[0].segMask);
                delete odResults;
            }
            C2_SAFE_FREE(nnOutput->mInModelPtr);
            C2_SAFE_FREE(nnOutput->mOutput);
            C2_SAFE_DELETE(nnOutput)
        }
        mRknnOutputs.removeAt(0);
    }
}

C2RKYolov5Session::RknnOutput* C2RKYolov5Session::getIdleRknnOutput() {
    for (int i = 0; i < mRknnOutputs.size(); i++) {
        RknnOutput *nnOutput = mRknnOutputs.editItemAt(i);
        if (nnOutput->isIdle()) {
            return nnOutput;
        }
    }
    return nullptr;
}

bool C2RKYolov5Session::createSession(
        const std::shared_ptr<C2RKSessionCallback> &cb, bool isHEVC) {
    int32_t err = 0;
    int32_t modelSize = 0;
    void *modelData = nullptr;

    // rknn api ops wrapper
    if (!mOps->initCheck()) {
        return false;
    }

    modelData = loadModelFile(&modelSize);
    if (!modelData) {
        return false;
    }

    // load rknn model
    err = mOps->rknnInit(&mRknnCtx, modelData, modelSize, 0, nullptr);
    if (err != RKNN_SUCC) {
        c2_err("failed to init rknn, err %d", err);
        goto cleanUp;
    }

    // get sdk and driver version
    rknn_sdk_version ver;
    err = mOps->rknnQuery(mRknnCtx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
    if (err != RKNN_SUCC) {
        c2_err("failed to query version, err %d", err);
        goto cleanUp;
    }

    c2_info("rknn api_version: %s, drv_version: %s", ver.api_version, ver.drv_version);

    // get inputs's and outputs's attr
    err = mOps->rknnQuery(mRknnCtx, RKNN_QUERY_IN_OUT_NUM, &mNumIO, sizeof(mNumIO));
    if (err != RKNN_SUCC) {
        c2_err("failed to rknn_query, err %d", err);
        goto cleanUp;
    }

    if (mNumIO.n_output != SEG_OUT_CHN_NUM) {
        c2_err("invalid output number, maybe not yolov5 model");
        err = RKNN_ERR_FAIL;
        goto cleanUp;
    }

    mInput = (rknn_input*)calloc(1, mNumIO.n_input * sizeof(rknn_input));
    mInputAttrs = (rknn_tensor_attr*)calloc(1, mNumIO.n_input * sizeof(rknn_tensor_attr));
    mOutputAttrs = (rknn_tensor_attr*)calloc(1, mNumIO.n_output * sizeof(rknn_tensor_attr));

    for (int i = 0; i < mNumIO.n_input; i++) {
        mInputAttrs[i].index = i;
        err = mOps->rknnQuery(mRknnCtx, RKNN_QUERY_INPUT_ATTR,
                                &(mInputAttrs[i]), sizeof(rknn_tensor_attr));
        if (err != RKNN_SUCC) {
            c2_err("rknnQuery(RKNN_QUERY_INPUT_ATTR), err %d", err);
            goto cleanUp;
        }
        dumpTensorAttr(&(mInputAttrs[i]));
    }

    for (int i = 0; i < mNumIO.n_output; i++) {
        mOutputAttrs[i].index = i;
        err = mOps->rknnQuery(mRknnCtx, RKNN_QUERY_OUTPUT_ATTR,
                                &(mOutputAttrs[i]), sizeof(rknn_tensor_attr));
        if (err != RKNN_SUCC) {
            c2_err("rknnQuery(RKNN_QUERY_OUTPUT_ATTR), err %d", err);
            goto cleanUp;
        }
        dumpTensorAttr(&(mOutputAttrs[i]));
    }

    // initialize rknn outputs
    initRknnOutputs();

    // NOTE: core_1 ..
    mOps->rknnSetCoreMask(mRknnCtx, RKNN_NPU_CORE_1);

    mIsHEVC = isHEVC;

    if (cb != nullptr) {
        // In asynchronous mode, start post-process looper
        startPostProcessLooper();
        mCallback = cb;
    }

cleanUp:
    if (modelData) {
        free(modelData);
        modelData = nullptr;
    }
    if (err != RKNN_SUCC) {
        disconnect();
        return false;
    }

    return true;
}

bool C2RKYolov5Session::onPostResult(RknnOutput *nnOutput) {
    if (!nnOutput) {
        c2_err("onPostResult null output");
        return false;
    }

    objectMapResultList omResults;

    ImageBuffer *inImage = nnOutput->mInImage;
    objectDetectResultList *odResults = (objectDetectResultList *)nnOutput->mOdResults;

    memset(&omResults, 0, sizeof(omResults));

    C2RKEasyTimer timer;
    timer.startRecord();

    nnOutput->setStatus(RknnOutput::RESULT);

    if (odResults->count > 0) {
        // postprocess od result to class map
        c2_postprocess_seg_mask_to_class_map(
                mPostProcessContext, mIsHEVC, odResults, &omResults);
    }

    timer.stopRecord("segMaskTo Class map");

    timer.startRecord();

    // draw detect object rect
    if (odResults->count > 0 && mDrawRect) {
        c2_postprocess_draw_rect_array(inImage, odResults);
    }

    timer.stopRecord("draw rect");

    timer.startRecord();

    if (mCallback) {
        mCallback->onResultReady(inImage, omResults.foundObjects ? &omResults : nullptr);
    }

    Mutex::Autolock autoLock(mLock);
    nnOutput->setStatus(RknnOutput::IDLE);
    mCondition.signal();

    timer.stopRecord("result callback");

    return true;
}

bool C2RKYolov5Session::onOutputPostProcess(RknnOutput *nnOutput) {
    if (!nnOutput) {
        c2_err("onOutputPostProcess null output");
        return false;
    }

    bool err = false;
    objectDetectResultList *odResults = (objectDetectResultList *)nnOutput->mOdResults;

    C2RKEasyTimer timer;
    timer.startRecord();

    if (!nnOutput->isIdle()) {
        nnOutput->setStatus(RknnOutput::POSTPROCESS);

        // postprocess rknn output and get object detect result.
        err = c2_postprocess_output_model_image(
                    mPostProcessContext, nnOutput->mOutput, odResults);
    }

    timer.stopRecord("postprocess");

    // translate yolov5 detection results to mpp class maps.
    // do async encode callback in this looper also.
    mResultHandler->pendingProcess(nnOutput);

    if (!err && mCallback) {
        mCallback->onError("postprocess");
    }

    return err;
}

bool C2RKYolov5Session::onRknnRunProcess(RknnOutput *nnOutput) {
    if (!nnOutput) {
        c2_err("onRknnRunProcess null output");
        return false;
    }

    C2RKEasyTimer timer;
    timer.startRecord();

    nnOutput->setStatus(RknnOutput::RKNNRUN);

    // Set Input Data
    mInput->index = 0;
    mInput->type  = RKNN_TENSOR_UINT8;
    mInput->fmt   = RKNN_TENSOR_NHWC;
    mInput->size  = SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT * 3;
    mInput->buf   = nnOutput->mInModelPtr;

    int err = mOps->rknnSetInputs(mRknnCtx, mNumIO.n_input, mInput);
    if (err < 0) {
        c2_err("failed to set rknn input, err %d", err);
        goto error;
    }

    err = mOps->rknnRun(mRknnCtx, nullptr);

    // Get Output Data
    err = mOps->rknnGetOutputs(mRknnCtx, mNumIO.n_output, nnOutput->mOutput, nullptr);
    if (err < 0) {
        c2_err("failed to get rknn output, err %d", err);
        goto error;
    }

    timer.stopRecord("rknnRun");

    // the postprocess of yolov5 output.
    // process rknn model output and get object detection results.
    mPostProcessHandler->pendingProcess(nnOutput);

    return true;

error:
    if (mCallback) {
        mCallback->onError("rknnRun");
    }
    return false;
}

bool C2RKYolov5Session::onCopyInputBuffer(RknnOutput *nnOutput) {
    if (!nnOutput) {
        c2_err("onCopyInputBuffer null output");
        return false;
    }

    ImageBuffer *inImage = nnOutput->mInImage;
    ImageBuffer *copyImage = nnOutput->mCopyImage;

    int32_t halFormat = (inImage->format == IMAGE_FORMAT_RGBA8888) ? 0x1 : 0x15;

    if (!copyImage->handle) {
        buffer_handle_t bufferHandle;

        uint32_t stride = 0;
        uint64_t usage  = (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);

        status_t status = GraphicBufferAllocator::get().allocate(
                inImage->wstride, inImage->hstride,
                halFormat, 1u /* layer count */,
                usage, &bufferHandle, &stride, "C2RKYolov5Session");
        if (status) {
            c2_err("failed transaction: allocate");
            return false;
        }

        copyImage->fd = C2RKGrallocOps::get()->getShareFd(bufferHandle);
        copyImage->handle = (void *)bufferHandle;
    }

    copyImage->format  = inImage->format;
    copyImage->width   = inImage->width;
    copyImage->height  = inImage->height;
    copyImage->wstride = inImage->wstride;
    copyImage->hstride = inImage->hstride;

    if (!c2_postprocess_copy_image_buffer(inImage, copyImage)) {
        c2_err("failed to copy input buffer");
        return false;
    }

    // update use copy buffer
    inImage->fd = copyImage->fd;

    return true;
}

bool C2RKYolov5Session::startDetect(ImageBuffer *srcImage) {
    int err = 0;

    if (!mPostProcessContext) {
        err = c2_postprocess_init_context(&mPostProcessContext, srcImage, mOutputAttrs);
        if (!err) {
            c2_err("failed to init post-process context");
            return false;
        }
    }

    C2RKEasyTimer timer;
    timer.startRecord();

    RknnOutput *nnOutput = nullptr;

    while (!nnOutput) {
        Mutex::Autolock autoLock(mLock);
        nnOutput = getIdleRknnOutput();
        if (!nnOutput) {
            mCondition.wait(mLock);
        }
        if (nnOutput) {
            // mark rknn_output is on use
            nnOutput->setStatus(RknnOutput::PREPROCESS);
            break;
        }
    }

    ImageBuffer modelImage;
    memset(&modelImage, 0, sizeof(ImageBuffer));

    modelImage.width   = SEG_MODEL_WIDTH;
    modelImage.height  = SEG_MODEL_HEIGHT;
    modelImage.wstride = SEG_MODEL_WIDTH;
    modelImage.hstride = SEG_MODEL_HEIGHT;
    modelImage.format  = IMAGE_FORMAT_RGB888;
    modelImage.virAddr = (uint8_t *)nnOutput->mInModelPtr;

    memcpy(nnOutput->mInImage, srcImage, sizeof(ImageBuffer));

    // convert to dst model image with rga
    err = c2_preprocess_convert_model_image(mPostProcessContext, srcImage, &modelImage);
    if (!err) {
        if (mCallback) {
            mCallback->onError("preprocess");
        }
        return false;
    }

    timer.stopRecord("pre convert model image");

    if (mCallback) {
        timer.startRecord();

        /*
         * since the timing when the input buffer runs out is not fixed, it is not
         * a methods hold the input buffer all yolov5 execution time, so copy anthor
         * input buffer for result callback encoder.
         */
        onCopyInputBuffer(nnOutput);

        timer.stopRecord("copy input buffer");

        // rknn run looper process, do rknn_run & rknn_outputs_get.
        mRknnRunHandler->pendingProcess(nnOutput);
    } else {
        err = onRknnRunProcess(nnOutput);
        if (!err) return false;

        err = onOutputPostProcess(nnOutput);
        if (!err) return false;

        err = onPostResult(nnOutput);
        if (!err) return false;
    }

    return true;
}

}
