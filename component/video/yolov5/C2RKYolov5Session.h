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

#ifndef ANDROID_RK_C2_YOLOV5_SESSIONS_H_
#define ANDROID_RK_C2_YOLOV5_SESSIONS_H_

#include "rknn_api.h"

#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/AMessage.h>
#include <utils/Vector.h>

namespace android {

#define C2_SAFE_FREE(p)             { if (p) {free(p); (p)=NULL;} }
#define C2_SAFE_DELETE(p)           { if (p) {delete(p); (p)=NULL;} }

#define SEG_OUT_CHN_NUM             (7)         /* rknn yolov5 seg output number */
#define SEG_OUT_BUF_SIZE            (1632000)   /* rknn yolov5 seg output size */
#define SEG_MODEL_WIDTH             (640)
#define SEG_MODEL_HEIGHT            (640)
#define SEG_MODEL_CHANNEL           (3)
#define SEG_MODEL_BUF_SIZE          (640*640*3)
#define SEG_NUMB_MAX_SIZE           (8)        /* maximum number of detect regions */

class C2RKRknnWrapper;
typedef void* PostProcessContext;

enum ImageFormat {
    IMAGE_FORMAT_GRAY8,
    IMAGE_FORMAT_RGB888,
    IMAGE_FORMAT_RGBA8888,
    IMAGE_FORMAT_YUV420SP_NV21,
    IMAGE_FORMAT_YUV420SP_NV12,
    IMAGE_FORMAT_YUV420P
};

struct ImageRect {
    int left;
    int top;
    int right;
    int bottom;
};

struct ImageBuffer {
    int32_t  fd;
    uint8_t *virAddr;
    int32_t  width;
    int32_t  height;
    int32_t  hstride;
    int32_t  vstride;
    int32_t  size;
    int32_t  flags;
    uint64_t pts;
    ImageFormat format;
    void *handle; /* copy buffer handler */
};

struct DetectRegions {
    int32_t count;
    ImageRect rects[SEG_NUMB_MAX_SIZE];
};

class C2RKSessionCallback {
public:
    virtual ~C2RKSessionCallback() = default;
    virtual void onError(const char *error);
    virtual void onResultReady(ImageBuffer *srcImage, void *result) = 0;
};

class C2RKYolov5Session {
public:
    // Rknn output wrapper
    struct RknnOutput {
        enum Status {
            IDLE = 0,
            PREPROCESS,
            RKNNRUN,
            POSTPROCESS,
            RESULT,
        };

        /* buffer index */
        int32_t      mIndex;
        /* buffer status */
        Status       mStatus;
        /* rknn output buffer */
        rknn_output *mOutput;
        /* rknn input image */
        ImageBuffer *mInImage;
        /* rknn copy input image */
        ImageBuffer *mCopyImage;
        /* yolov5 required model size 640x640, RGB888  */
        ImageBuffer *mModelImage;
        /* object detect results, alloc segMask memory, so don't memset it */
        void        *mOdResults;

        bool isIdle();
        void setStatus(Status status);
    };

    C2RKYolov5Session();
    ~C2RKYolov5Session();

    bool createSession(const std::shared_ptr<C2RKSessionCallback> &cb, int32_t ctuSize);
    void disconnect();

    bool startDetect(ImageBuffer *srcImage);

    /* yolov5 result: 1. proto mask 2. roi rect array without postprocess */
    bool isMaskResultType();

    bool onPostResult(RknnOutput *nnOutput);
    bool onOutputPostProcess(RknnOutput *nnOutput);
    bool onRknnRunProcess(RknnOutput *nnOutput);
    bool onCopyInputBuffer(RknnOutput *nnOutput);


private:
    class BaseProcessHandler : public AHandler {
    public:
        enum {
            kWhatProcess,
            kWhatStop,
        };

        BaseProcessHandler(C2RKYolov5Session *thiz) { mRunning = true; mThiz = thiz; }
        ~BaseProcessHandler() override = default;

        void pendingProcess(RknnOutput *nnOutput);
        void stopHandler();

        virtual void onDoProcess(RknnOutput *nnOutput) = 0;

    public:
        C2RKYolov5Session *mThiz;
        bool mRunning;

    protected:
        void onMessageReceived(const sp<AMessage> &msg) override;
    };

    /*
     * Rknn run looper process
     * do rknn_run & rknn_outputs_get
     */
    class RknnRunHandler : public BaseProcessHandler {
    public:
        RknnRunHandler(C2RKYolov5Session *thiz) : BaseProcessHandler(thiz) {}
        ~RknnRunHandler() override = default;

        void onDoProcess(RknnOutput *nnOutput) override {
            std::ignore = mThiz->onRknnRunProcess(nnOutput);
        }
    };

    /*
     * the postprocess of yolov5 output
     * process rknn model output and get object detection results.
     */
    class PostProcessHandler : public BaseProcessHandler {
    public:
        PostProcessHandler(C2RKYolov5Session *thiz) : BaseProcessHandler(thiz) {}
        ~PostProcessHandler() override = default;

        void onDoProcess(RknnOutput *nnOutput) override {
            std::ignore = mThiz->onOutputPostProcess(nnOutput);
        }
    };

    /*
     * translate yolov5 detection results to mpp class maps.
     * do async encode callback in this looper also.
     */
    class ResultHandler : public BaseProcessHandler {
    public:
        ResultHandler(C2RKYolov5Session *thiz) : BaseProcessHandler(thiz) {}
        ~ResultHandler() override = default;

        void onDoProcess(RknnOutput *nnOutput) override {
            std::ignore = mThiz->onPostResult(nnOutput);
        }
    };

    /* rknn model context */
    C2RKRknnWrapper       *mOps;    // rknn api ops wrapper
    rknn_context           mRknnCtx;
    rknn_input            *mInput;
    rknn_tensor_attr      *mInputAttrs;
    rknn_tensor_attr      *mOutputAttrs;
    rknn_input_output_num  mNumIO;

    /* multi-thread is used for share the execution time */
    sp<ALooper>            mRknnRunLooper;
    sp<BaseProcessHandler> mRknnRunHandler;
    sp<ALooper>            mPostProcessLooper;
    sp<BaseProcessHandler> mPostProcessHandler;
    sp<ALooper>            mResultLooper;
    sp<BaseProcessHandler> mResultHandler;

    Mutex                  mLock;
    Condition              mCondition;
    Vector<RknnOutput*>    mRknnOutputs;

    /* post process context */
    PostProcessContext     mPostProcessContext;

    /* ctu size */
    int32_t                mCtuSize;

    /* draw detecton rect */
    bool                   mDrawRect;

    /* yolov5 result: 1. proto mask 2. roi rect array without postprocess */
    bool                   mResultProtoMask;

    std::shared_ptr<C2RKSessionCallback> mCallback;

private:
    bool startPostProcessLooper();
    void stopPostProcessLooper();

    // due to performance considerations, multi-threaded processing is
    // neccessary. so we maintained a set of output buffers.
    void initRknnOutputs();
    void releaseRknnOutputs();
    RknnOutput* getIdleRknnOutput();
};

}

#endif // ANDROID_RK_C2_YOLOV5_SESSIONS_H_
