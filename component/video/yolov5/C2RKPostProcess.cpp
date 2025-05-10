/*
 * Copyright (C) 2025 Rockchip Electronics Co. LTD
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
 */

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKPostProcess"

#include <stdlib.h>
#include <string.h>
#include <vector>
#include <set>
#include <math.h>
#include <arm_neon.h>
#include <cutils/properties.h>

#include "C2RKPostProcess.h"
#include "C2RKYolov5Session.h"
#include "C2RKRknnWrapper.h"
#include "C2RKLog.h"

#include "im2d.h"
#include "drmrga.h"

namespace android {

// post-process output seg mask dump
#define PROPERTY_NAME_SEG_MASK_DUMP     "codec2_yolov5_seg_mask_dump"
#define DEFAULT_SEG_MASK_DUMP_PATH      "/data/video/seg_dump.txt"

#define NMS_THRESH          0.45
#define BOX_THRESH          0.25
#define PROTO_CHANNEL       32
#define PROTO_HEIGHT        160
#define PROTO_WEIGHT        160

#define OBJ_CLASS_NUM       80
#define PROP_BOX_SIZE       (5 + OBJ_CLASS_NUM)

#define _MAX(a, b)          ((a) > (b) ? (a) : (b))
#define _MIN(a, b)          ((a) < (b) ? (a) : (b))

static const int gAnchor[3][6] = {
    { 10, 13, 16, 30, 33, 23 },
    { 30, 61, 62, 45, 59, 119 },
    { 116, 90, 156, 198, 373, 326 },
};

typedef struct {
    int32_t    originWidth;
    int32_t    originHeight;
    uint8_t   *omResultMap;
    float     *protoData;
    uint8_t   *segMask;
    uint8_t   *matmulOut;
    uint8_t   *allMaskInOne;
    uint8_t   *croppedSegMask;
    LetterBox *letterbox;
    bool       resultMask;
    rknn_tensor_attr *nnOutputAttr;

    rknn_matmul_ctx     matmulCtx;
    rknn_matmul_shape   shapes[SEG_NUMB_MAX_SIZE];
    rknn_matmul_io_attr ioAttr[SEG_NUMB_MAX_SIZE];
    rknn_tensor_mem    *tensorA;
    rknn_tensor_mem    *tensorB;
    rknn_tensor_mem    *tensorC;
    uint16_t           *vectorB; /* float32 to float16 */

    // output seg mask dump
    FILE *dumpFp;
} PostProcessContextImpl;

static int _toRgaFormat(ImageFormat fmt) {
    switch (fmt) {
    case IMAGE_FORMAT_RGB888:
        return RK_FORMAT_RGB_888;
    case IMAGE_FORMAT_RGBA8888:
        return RK_FORMAT_RGBA_8888;
    case IMAGE_FORMAT_YUV420SP_NV12:
        return RK_FORMAT_YCbCr_420_SP;
    case IMAGE_FORMAT_YUV420SP_NV21:
        return RK_FORMAT_YCrCb_420_SP;
    case IMAGE_FORMAT_YUV420P:
        return RK_FORMAT_YCbCr_420_P;
    default:
        return -1;
    }
}

inline static int32_t __clip(float val, float min, float max) {
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

static float _deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

static int8_t _qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)__clip(dst_val, -128, 127);
    return res;
}

int _clamp(float val, int min, int max) {
    return val > min ? (val < max ? val : max) : min;
}

int _box_reverse(int position, int boundary, int pad, float scale) {
    return (int)((_clamp(position, 0, boundary) - pad) / scale);
}

void _resize_by_rga_uint8(
        uint8_t *inputPtr, int inputW, int inputH,
        int boxesNum, uint8_t *outputPtr, int outputW, int outputH) {
    IM_STATUS ret = IM_STATUS_SUCCESS;
    rga_buffer_t rgaSrc, rgaDst;
    rga_buffer_handle_t rgaSrcHdl, rgaDstHdl;

    for (int b = 0; b < boxesNum; b++) {
        memset(&rgaSrc, 0, sizeof(rgaSrc));
        memset(&rgaDst, 0, sizeof(rgaDst));

        rgaSrcHdl = importbuffer_virtualaddr(
                        inputPtr + (b * inputW * inputH), inputW * inputH);
        rgaDstHdl = importbuffer_virtualaddr(
                        outputPtr + (b * outputW * outputH), outputW * outputH);

        rgaSrc = wrapbuffer_handle(rgaSrcHdl, inputW, inputH, RK_FORMAT_YCbCr_400);
        rgaDst = wrapbuffer_handle(rgaDstHdl, outputW, outputH, RK_FORMAT_YCbCr_400);

        ret = imresize(rgaSrc, rgaDst);
        if (ret != IM_STATUS_SUCCESS) {
            c2_err("failed imresize");
        }
    }
}

void _seg_reverse(
        uint8_t *segMask, uint8_t *croppedSeg, uint8_t *segMaskReal,
        int modelH, int modelW, int croppedH, int croppedW,
        int oriInH, int oriInW, int yPad, int xPad) {
    if (yPad == 0 && xPad == 0 && oriInH == modelH && oriInW == modelW) {
        memcpy(segMaskReal, segMask, oriInH * oriInW);
        return;
    }

    int croppedIndex = 0;
    for (int i = 0; i < modelH; i++) {
        for (int j = 0; j < modelW; j++) {
            if (i >= yPad && i < modelH - yPad && j >= xPad && j < modelW - xPad) {
                int segIndex = i * modelW + j;
                croppedSeg[croppedIndex] = segMask[segIndex];
                croppedIndex++;
            }
        }
    }

    // Note: Here are different methods provided for implementing single-channel
    // image scaling. The method of using rga to resize the image requires that
    // the image size is 2 aligned.
    _resize_by_rga_uint8(croppedSeg, croppedW, croppedH, 1, segMaskReal, oriInW, oriInH);
}

 static void _convert_neon(const float *src, uint16_t *dst, int n) {
     for (int i = 0; i < n; i += 4) {
         float32x4_t f32 = vld1q_f32(src + i);
         float16x4_t f16 = vcvt_f16_f32(f32);
         vst1_u16(dst + i, (uint16x4_t)f16);
     }
 }

static int _process_i8(
        rknn_output *allInput, int inputId, int *anchor,
        int gridH, int gridW, int height, int width, int stride,
        std::vector<float> &boxes, std::vector<float> &segments,
        float *proto, std::vector<float> &objProbs, std::vector<int> &classId,
        float threshold, rknn_tensor_attr *outputAttrs, uint16_t *vectorB) {
    (void)width;
    (void)height;

    int validCount = 0;

    if (inputId % 2 == 1) {
        return validCount;
    }

    int8_t *input = (int8_t *)allInput[inputId].buf;
    int32_t zp    = outputAttrs[inputId].zp;
    float scale   = outputAttrs[inputId].scale;

    if (inputId == 6) { /* prototype masks */
        int maxCount = PROTO_CHANNEL * PROTO_HEIGHT * PROTO_WEIGHT;

        for (int i = 0; i < maxCount; i++) {
            proto[i] = _deqnt_affine_to_f32(input[i], zp, scale);
        }
        // convert neon
        _convert_neon(proto, vectorB, maxCount);

        return validCount;
    }

    int32_t gridLen  = gridH * gridW;
    int8_t *inputSeg = (int8_t *)allInput[inputId + 1].buf;
    int32_t zpSeg    = outputAttrs[inputId + 1].zp;
    float scaleSeg   = outputAttrs[inputId + 1].scale;
    int8_t thresI8   = _qnt_f32_to_affine(threshold, zp, scale);

    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < gridH; i++) {
            for (int j = 0; j < gridW; j++) {
                int8_t boxConf = input[(PROP_BOX_SIZE * a + 4) * gridLen + i * gridW + j];
                if (boxConf >= thresI8) {
                    int offset = (PROP_BOX_SIZE * a) * gridLen + i * gridW + j;
                    int offsetSeg = (PROTO_CHANNEL * a) * gridLen + i * gridW + j;
                    int8_t *inPtr = input + offset;
                    int8_t *inPtrSeg = inputSeg + offsetSeg;

                    float boxX = (_deqnt_affine_to_f32(*inPtr, zp, scale)) * 2.0 - 0.5;
                    float boxY = (_deqnt_affine_to_f32(inPtr[gridLen], zp, scale)) * 2.0 - 0.5;
                    float boxW = (_deqnt_affine_to_f32(inPtr[2 * gridLen], zp, scale)) * 2.0;
                    float boxH = (_deqnt_affine_to_f32(inPtr[3 * gridLen], zp, scale)) * 2.0;

                    boxX = (boxX + j) * (float)stride;
                    boxY = (boxY + i) * (float)stride;
                    boxW = boxW * boxW * (float)anchor[a * 2];
                    boxH = boxH * boxH * (float)anchor[a * 2 + 1];
                    boxX -= (boxW / 2.0);
                    boxY -= (boxH / 2.0);

                    int8_t maxClassProbs = inPtr[5 * gridLen];
                    int maxClassId = 0;
                    for (int k = 1; k < OBJ_CLASS_NUM; ++k) {
                        int8_t prob = inPtr[(5 + k) * gridLen];
                        if (prob > maxClassProbs) {
                            maxClassId = k;
                            maxClassProbs = prob;
                        }
                    }

                    float boxConfF32   = _deqnt_affine_to_f32(boxConf, zp, scale);
                    float classProbF32 = _deqnt_affine_to_f32(maxClassProbs, zp, scale);
                    float limitScore   = boxConfF32 * classProbF32;

                    if (limitScore > threshold) {
                        for (int k = 0; k < PROTO_CHANNEL; k++) {
                            float segElementFp =
                                    _deqnt_affine_to_f32(inPtrSeg[(k)*gridLen], zpSeg, scaleSeg);
                            segments.push_back(segElementFp);
                        }

                        objProbs.push_back(
                                (_deqnt_affine_to_f32(maxClassProbs, zp, scale)) *
                                (_deqnt_affine_to_f32(boxConf, zp, scale)));
                        classId.push_back(maxClassId);
                        boxes.push_back(boxX);
                        boxes.push_back(boxY);
                        boxes.push_back(boxW);
                        boxes.push_back(boxH);
                        validCount++;
                    }
                }
            }
        }
    }
    return validCount;
}

static int _process_fp32(
        rknn_output *allInput, int inputId, int *anchor, int gridH,
        int gridW, int height, int width, int stride, std::vector<float> &boxes,
        std::vector<float> &segments, float *proto, std::vector<float> &objProbs,
        std::vector<int> &classId, float threshold) {
    (void)width;
    (void)height;

    int validCount = 0;

    if (inputId % 2 == 1) {
        return validCount;
    }

    float *input = (float *)allInput[inputId].buf;

    if (inputId == 6) {
        int maxCount = PROTO_CHANNEL * PROTO_HEIGHT * PROTO_WEIGHT;

        for (int i = 0; i < maxCount; i++) {
            proto[i] = input[i];
        }

        return validCount;
    }

    int gridLen = gridH * gridW;
    float *inputSeg = (float *)allInput[inputId + 1].buf;

    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < gridH; i++) {
            for (int j = 0; j < gridW; j++) {
                float boxConf = input[(PROP_BOX_SIZE * a + 4) * gridLen + i * gridW + j];
                if (boxConf >= threshold) {
                    int offset = (PROP_BOX_SIZE * a) * gridLen + i * gridW + j;
                    int offsetSeg = (PROTO_CHANNEL * a) * gridLen + i * gridW + j;
                    float *inPtr = input + offset;
                    float *inPtrSeg = inputSeg + offsetSeg;

                    float boxX = *inPtr * 2.0 - 0.5;
                    float boxY = inPtr[gridLen] * 2.0 - 0.5;
                    float boxW = inPtr[2 * gridLen] * 2.0;
                    float boxH = inPtr[3 * gridLen] * 2.0;

                    boxX = (boxX + j) * (float)stride;
                    boxY = (boxY + i) * (float)stride;
                    boxW = boxW * boxW * (float)anchor[a * 2];
                    boxH = boxH * boxH * (float)anchor[a * 2 + 1];
                    boxX -= (boxW / 2.0);
                    boxY -= (boxH / 2.0);

                    float maxClassProbs = inPtr[5 * gridLen];
                    int maxClassId = 0;
                    for (int k = 1; k < OBJ_CLASS_NUM; ++k) {
                        float prob = inPtr[(5 + k) * gridLen];
                        if (prob > maxClassProbs) {
                            maxClassId = k;
                            maxClassProbs = prob;
                        }
                    }
                    float limitScore = maxClassProbs * boxConf;
                    if (limitScore > threshold) {
                        for (int k = 0; k < PROTO_CHANNEL; k++) {
                            float segElementF32 = inPtrSeg[(k) * gridLen];
                            segments.push_back(segElementF32);
                        }
                        objProbs.push_back(maxClassProbs * boxConf);
                        classId.push_back(maxClassId);
                        boxes.push_back(boxX);
                        boxes.push_back(boxY);
                        boxes.push_back(boxW);
                        boxes.push_back(boxH);
                        validCount++;
                    }
                }
            }
        }
    }
    return validCount;
}

static int _quick_sort_indice_inverse(
        std::vector<float> &input, int left, int right, std::vector<int> &indices) {
    float key;
    int key_index;
    int low = left;
    int high = right;

    if (left < right) {
        key_index = indices[left];
        key = input[left];
        while (low < high) {
            while (low < high && input[high] <= key) {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        _quick_sort_indice_inverse(input, left, low - 1, indices);
        _quick_sort_indice_inverse(input, low + 1, right, indices);
    }

    return low;
}

static float _calculateOverlap(
            float xmin0, float ymin0, float xmax0,
            float ymax0, float xmin1, float ymin1,
            float xmax1, float ymax1) {
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) +
              (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int _nms(int validCount, std::vector<float> &outputLocations,
               std::vector<int> classIds, std::vector<int> &order,
               int filterId, float threshold) {
    for (int i = 0; i < validCount; ++i) {
        if (order[i] == -1 || classIds[i] != filterId) {
            continue;
        }
        int n = order[i];
        for (int j = i + 1; j < validCount; ++j) {
            int m = order[j];
            if (m == -1 || classIds[i] != filterId) {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = _calculateOverlap(
                            xmin0, ymin0, xmax0,
                            ymax0, xmin1, ymin1, xmax1, ymax1);
            if (iou > threshold) {
                order[j] = -1;
            }
        }
    }
    return 0;
}

void _crop_mask_fp(
        float *segMask, uint8_t *allMaskInOne, float *boxes,
        int boxesNum, int *clsId, int height, int width) {
    for (int b = 0; b < boxesNum; b++) {
        float x1 = boxes[b * 4 + 0];
        float y1 = boxes[b * 4 + 1];
        float x2 = boxes[b * 4 + 2];
        float y2 = boxes[b * 4 + 3];

        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                if (j >= x1 && j < x2 && i >= y1 && i < y2) {
                    if (allMaskInOne[i * width + j] == 0) {
                        if (segMask[b * width * height + i * width + j] > 0) {
                            allMaskInOne[i * width + j] = (clsId[b] + 1);
                        } else {
                            allMaskInOne[i * width + j] = 0;
                        }
                    }
                }
            }
        }
    }
}

static void _crop_mask_uint8(
        uint8_t *segMask, uint8_t *allMaskInOne, float *boxes,
        int boxesNum, int *clsId, int height, int width) {
    for (int b = 0; b < boxesNum; b++) {
        float x1 = boxes[b * 4 + 0];
        float y1 = boxes[b * 4 + 1];
        float x2 = boxes[b * 4 + 2];
        float y2 = boxes[b * 4 + 3];

        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                if (j >= x1 && j < x2 && i >= y1 && i < y2) {
                    if (allMaskInOne[i * width + j] == 0) {
                        if (segMask[b * width * height + i * width + j] > 0) {
                            allMaskInOne[i * width + j] = (clsId[b] + 1);
                        }
                    }
                }
            }
        }
    }
}

static void _crop_mask_uint8_merge(
        uint8_t *segMask, uint8_t *allMaskInOne, float *boxes,
        int boxesNum, int *clsId, int width, int height) {
    (void)clsId;
    for (int b = 0; b < boxesNum; b++) {
        float x1 = boxes[b * 4 + 0];
        float y1 = boxes[b * 4 + 1];
        float x2 = boxes[b * 4 + 2];
        float y2 = boxes[b * 4 + 3];

        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                if (j >= x1 && j < x2 && i >= y1 && i < y2 &&
                    allMaskInOne[i * width + j] == 0) {
                    allMaskInOne[i * width + j] = segMask[i * width + j] > 0;
                }
            }
        }
    }
}

void _matmul_by_npu_fp(
        PostProcessContextImpl *impl, std::vector<float> &AInput,
        uint8_t *CInput, int ROWSA, int COLSA) {
    int ret = 0;
    rknn_matmul_ctx matCtx = impl->matmulCtx;
    rknn_matmul_io_attr *ioAttr = &impl->ioAttr[ROWSA - 1];

    ret = C2RKRknnWrapper::get()->rknnMatmulSetShape(matCtx, &impl->shapes[ROWSA - 1]);
    if (ret != 0) {
        c2_err("failed to rknn_matmul_set_dynamic_shapes, ret %d", ret);
        return;
    }

    rknpu2::float16 int8VectorA[ROWSA * COLSA];
    for (int i = 0; i < ROWSA * COLSA; ++i) {
        int8VectorA[i] = (rknpu2::float16)AInput[i];
    }

    memcpy(impl->tensorA->virt_addr, int8VectorA, ioAttr->A.size);
    memcpy(impl->tensorB->virt_addr, impl->vectorB, ioAttr->B.size);

    ret = C2RKRknnWrapper::get()->rknnMatmulSetIOMem(matCtx, impl->tensorA, &ioAttr->A);
    ret = C2RKRknnWrapper::get()->rknnMatmulSetIOMem(matCtx, impl->tensorB, &ioAttr->B);
    ret = C2RKRknnWrapper::get()->rknnMatmulSetIOMem(matCtx, impl->tensorC, &ioAttr->C);

    ret = C2RKRknnWrapper::get()->rknnMatmulRun(matCtx);

    int boxesNum = ROWSA;
    int tensorCLen = ioAttr->C.size / sizeof(float);
    int tensorMergeLen = tensorCLen / boxesNum;
    float mergeCResult = 0;

    for (int i = 0; i < tensorMergeLen; ++i) {
        mergeCResult = 0;
        for (int j = 0; j < boxesNum; ++j) {
            mergeCResult += ((float *)impl->tensorC->virt_addr)[j * tensorMergeLen + i] > 0;
        }
        CInput[i] = mergeCResult > 0 ? 4 : 0;
    }
}

static void _get_blk_object(
        int blkPosX, int blkPosY, int picWidth,int picHeight,
        uint8_t *segMask, uint8_t *objectMap, int posIn16x16Blk) {
    int roiCalcList[6];
    int k, l, posIdx, m;
    int blkEndX, blkEndY;

    if (blkPosX > picWidth || blkPosY > picHeight) {
        objectMap[posIn16x16Blk] = 0; // 0 means background
        return;
    }

    // calculate the block end position
    blkEndX = _MIN(blkPosX + 15, picWidth - 1);
    blkEndY = _MIN(blkPosY + 15, picHeight - 1);

    memset(&roiCalcList, 0, sizeof(int) * 6);
    // calculate the number of pixels (in a 16x16 block) in each category
    for (k = blkPosY; k <= blkEndY; k += 2) {
        for (l = blkPosX; l <= blkEndX; l += 2) {
            posIdx = l + k * picWidth;
            if (segMask[posIdx] == 0)
                roiCalcList[0]++;
            else if (segMask[posIdx] == 1)
                roiCalcList[1]++;
            else if (segMask[posIdx] == 2)
                roiCalcList[2]++;
            else if (segMask[posIdx] == 3)
                roiCalcList[3]++;
            else if (segMask[posIdx] == 4)
                roiCalcList[4]++;
            else if (segMask[posIdx] == 5)
                roiCalcList[5]++;
        }
    }
    // default value is 6, which means this block has different object
    // or at the boundary of the image
    objectMap[posIn16x16Blk] = 6;
    // get the category with the most pixels
    for (m = 0; m < 6; m++) {
        if (roiCalcList[m] > (blkEndY - blkPosY + 1) * (blkEndX - blkPosX + 1) / 4 * 8 / 10) {
            objectMap[posIn16x16Blk] = m;
            break;
        }
    }
}

bool c2_postprocess_init_context(
        PostProcessContext *ctx, ImageBuffer *originImage,
        rknn_tensor_attr *outputAttr, bool resultMask) {
    if (originImage == nullptr || outputAttr == nullptr) {
        c2_err("invalid null params");
        return false;
    }

    PostProcessContextImpl *impl =
            (PostProcessContextImpl*)calloc(1, sizeof(PostProcessContextImpl));

    impl->letterbox = (LetterBox*)calloc(1, sizeof(LetterBox));

    // malloc seg map memory
    impl->omResultMap = (uint8_t *)malloc(originImage->hstride * originImage->vstride);
    impl->protoData = (float*)malloc(PROTO_CHANNEL * PROTO_HEIGHT * PROTO_WEIGHT * sizeof(float));
    impl->segMask = (uint8_t*)malloc(SEG_NUMB_MAX_SIZE * SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT);
    impl->matmulOut = (uint8_t*)malloc(SEG_NUMB_MAX_SIZE * SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT);
    impl->allMaskInOne = (uint8_t*)malloc(SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT);
    impl->croppedSegMask = (uint8_t*)malloc(SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT);

    // init rknn matmul
    {
        rknn_matmul_info info;

        int err = 0;
        int maxSizeA = 0, maxSizeB = 0, maxSizeC = 0;

        memset(impl->ioAttr, 0, sizeof(rknn_matmul_io_attr) * SEG_NUMB_MAX_SIZE);
        memset(&info, 0, sizeof(rknn_matmul_info));

        info.type = RKNN_FLOAT16_MM_FLOAT16_TO_FLOAT32;
        info.B_layout = RKNN_MM_LAYOUT_NORM;
        info.AC_layout = RKNN_MM_LAYOUT_NORM;

        for (int i = 0; i < SEG_NUMB_MAX_SIZE; ++i) {
            impl->shapes[i].M = i + 1;
            impl->shapes[i].K = PROTO_CHANNEL;
            impl->shapes[i].N = PROTO_HEIGHT * PROTO_WEIGHT;
        }

        err = C2RKRknnWrapper::get()->rknnMatmulCreateShape(
                &impl->matmulCtx, &info, SEG_NUMB_MAX_SIZE, impl->shapes, impl->ioAttr);
        if (err < 0) {
            c2_err("failed to rknn_matmul_create_dynamic_shape, err %d", err);
            return false;
        }

        for (int i = 0; i < SEG_NUMB_MAX_SIZE; ++i) {
            maxSizeA = _MAX(impl->ioAttr[i].A.size, maxSizeA);
            maxSizeB = _MAX(impl->ioAttr[i].B.size, maxSizeB);
            maxSizeC = _MAX(impl->ioAttr[i].C.size, maxSizeC);
        }
        c2_trace("tensor_a max size %d tensor_b max size %d tensor_c max size %d",
                 maxSizeA, maxSizeB, maxSizeC);

        impl->tensorA = C2RKRknnWrapper::get()->rknnCreateMem(impl->matmulCtx, maxSizeA);
        impl->tensorB = C2RKRknnWrapper::get()->rknnCreateMem(impl->matmulCtx, maxSizeB);
        impl->tensorC = C2RKRknnWrapper::get()->rknnCreateMem(impl->matmulCtx, maxSizeC);

        impl->vectorB = (uint16_t *)calloc(
                1, PROTO_CHANNEL * PROTO_HEIGHT * PROTO_WEIGHT * sizeof(uint16_t));
    }

    impl->originWidth  = originImage->width;
    impl->originHeight = originImage->height;
    impl->resultMask   = resultMask;
    impl->nnOutputAttr = outputAttr;

    // output seg mask dump
    if (property_get_bool(PROPERTY_NAME_SEG_MASK_DUMP, 0) && !impl->dumpFp) {
        impl->dumpFp = fopen(DEFAULT_SEG_MASK_DUMP_PATH, "w");
        if (impl->dumpFp) {
            c2_info("dump seg mask to %s", DEFAULT_SEG_MASK_DUMP_PATH);
        }
    }

    *ctx = impl;
    return true;
}

bool c2_postprocess_deinit_context(PostProcessContext ctx) {
    PostProcessContextImpl *impl = (PostProcessContextImpl*)ctx;
    if (impl) {
        C2_SAFE_FREE(impl->letterbox);
        C2_SAFE_FREE(impl->omResultMap);
        C2_SAFE_FREE(impl->protoData);
        C2_SAFE_FREE(impl->segMask);
        C2_SAFE_FREE(impl->matmulOut);
        C2_SAFE_FREE(impl->allMaskInOne);
        C2_SAFE_FREE(impl->croppedSegMask);
        C2_SAFE_FREE(impl->vectorB);

        if (impl->tensorA) {
            C2RKRknnWrapper::get()->rknnDestroyMem(impl->matmulCtx, impl->tensorA);
        }
        if (impl->tensorB) {
            C2RKRknnWrapper::get()->rknnDestroyMem(impl->matmulCtx, impl->tensorB);
        }
        if (impl->tensorC) {
            C2RKRknnWrapper::get()->rknnDestroyMem(impl->matmulCtx, impl->tensorC);
        }
        if (impl->matmulCtx) {
            C2RKRknnWrapper::get()->rknnMatmulDestroy(impl->matmulCtx);
        }
        if (impl->dumpFp) {
            fclose(impl->dumpFp);
            impl->dumpFp = nullptr;
        }
        C2_SAFE_DELETE(impl);
    }
    return true;
}

int c2_preprocess_convert_image_with_rga(
        ImageBuffer *src, ImageBuffer *dst, ImageRect *srcRect, ImageRect *dstRect) {
    IM_STATUS err = IM_STATUS_SUCCESS;

    im_rect srect, drect, prect;

    int srcFmt = _toRgaFormat(src->format);
    int dstFmt = _toRgaFormat(dst->format);

    memset(&prect, 0, sizeof(im_rect));

    srect.x = srcRect->left;
    srect.y = srcRect->top;
    srect.width = srcRect->right - srcRect->left + 1;
    srect.height = srcRect->bottom - srcRect->top + 1;

    drect.x = dstRect->left;
    drect.y = dstRect->top;
    drect.width = dstRect->right - dstRect->left + 1;
    drect.height = dstRect->bottom - dstRect->top + 1;

    // set rga buffer
    rga_buffer_t rgaSrc;
    rga_buffer_t rgaDst;
    rga_buffer_t rgaPat;
    im_handle_param_t rgaSrcParam;
    im_handle_param_t rgaDstParam;
    rga_buffer_handle_t rgaSrcHdl = 0;
    rga_buffer_handle_t rgaDstHdl = 0;

    memset(&rgaPat, 0, sizeof(rga_buffer_t));

    rgaSrcParam.width  = src->width;
    rgaSrcParam.height = src->height;
    rgaSrcParam.format = srcFmt;
    rgaDstParam.width  = dst->width;
    rgaDstParam.height = dst->height;
    rgaDstParam.format = dstFmt;

    if (src->fd > 0) {
        rgaSrcHdl = importbuffer_fd(src->fd, &rgaSrcParam);
    } else {
        rgaSrcHdl = importbuffer_virtualaddr(src->virAddr, &rgaSrcParam);
    }
    if (rgaSrcHdl <= 0) {
        c2_err("src handle error");
        err = IM_STATUS_FAILED;
        goto cleanUp;
    }
    rgaSrc = wrapbuffer_handle(rgaSrcHdl, src->width,
                    src->height, srcFmt, src->hstride, src->vstride);

    if (dst->fd > 0) {
        rgaDstHdl = importbuffer_fd(dst->fd, &rgaDstParam);
    } else {
        rgaDstHdl = importbuffer_virtualaddr(dst->virAddr, &rgaDstParam);
    }
    if (rgaDstHdl <= 0) {
        c2_err("dst handle error");
        err = IM_STATUS_FAILED;
        goto cleanUp;
    }
    rgaDst = wrapbuffer_handle(rgaDstHdl, dst->width,
                    dst->height, dstFmt, dst->hstride, dst->vstride);

    if (drect.width != dst->width || drect.height != dst->height) {
        im_rect dstWholeRect = { 0, 0, dst->width, dst->height };
        int bgColor = 114; // pad color for letterbox
        int imcolor;
        char* pImColor = (char *)(&imcolor);
        pImColor[0] = bgColor;
        pImColor[1] = bgColor;
        pImColor[2] = bgColor;
        pImColor[3] = bgColor;
        err = imfill(rgaDst, dstWholeRect, imcolor);
        if (err <= 0) {
            c2_warn("Warning: Can not fill color on target image");
        }
    }

    c2_trace("===========preprocess rga translte info===============");
    c2_trace("rga src [%d,%d,%d,%d] fd %d fmt %d", rgaSrc.width, rgaSrc.height,
              rgaSrc.wstride, rgaSrc.hstride, rgaSrc.fd, rgaSrc.format);
    c2_trace("rga dst [%d,%d,%d,%d] fd %d fmt %d", rgaDst.width, rgaDst.height,
              rgaDst.wstride, rgaDst.hstride, rgaDst.fd, rgaDst.format);

    err = improcess(rgaSrc, rgaDst, rgaPat, srect, drect, prect, 0);
    if (err <= 0) {
        c2_err("Error on improcess STATUS=%d", err);
        c2_err("RGA error message: %s", imStrError(err));
    }

cleanUp:
    if (rgaSrcHdl > 0) {
        releasebuffer_handle(rgaSrcHdl);
    }

    if (rgaDstHdl > 0) {
        releasebuffer_handle(rgaDstHdl);
    }

    return (err > 0);
}

bool c2_preprocess_convert_model_image(
        PostProcessContext ctx, ImageBuffer *srcImage, ImageBuffer *modelImage) {
    PostProcessContextImpl *impl = (PostProcessContextImpl*)ctx;
    if (!impl) {
        c2_err("invalid null post-process context");
        return false;
    }

    bool allowSlightChange = true;

    LetterBox *letterbox = impl->letterbox;

    int resizeWidth   = modelImage->width;
    int resizeHeight  = modelImage->height;
    int paddingWidth  = 0;
    int paddingHeight = 0;
    int leftOffset    = 0;
    int topOffset     = 0;

    ImageRect srcRect;
    ImageRect dstRect;

    srcRect.left   = 0;
    srcRect.top    = 0;
    srcRect.right  = srcImage->width - 1;
    srcRect.bottom = srcImage->height - 1;

    dstRect.left   = 0;
    dstRect.top    = 0;
    dstRect.right  = modelImage->width - 1;
    dstRect.bottom = modelImage->height - 1;

    float scale = 1.0;
    float scaleWidth = (float)modelImage->width / srcImage->width;
    float scaleHeight = (float)modelImage->height / srcImage->height;

    if (scaleWidth < scaleHeight) {
        scale = scaleWidth;
        resizeHeight = srcImage->height * scale;
    } else {
        scale = scaleHeight;
        resizeWidth = srcImage->width * scale;
    }

    // slight change image size for align
    if (allowSlightChange && (resizeWidth % 4 != 0)) {
        resizeWidth -= resizeWidth % 4;
    }
    if (allowSlightChange && (resizeHeight % 2 != 0)) {
        resizeHeight -= resizeHeight % 2;
    }

    // padding
    paddingHeight = modelImage->height - resizeHeight;
    paddingWidth = modelImage->width - resizeWidth;

    // center
    if (scaleWidth < scaleHeight) {
        dstRect.top = paddingHeight / 2;
        if (dstRect.top % 2 != 0) {
            dstRect.top -= dstRect.top % 2;
            if (dstRect.top < 0) {
                dstRect.top = 0;
            }
        }
        dstRect.bottom = dstRect.top + resizeHeight - 1;
        topOffset = dstRect.top;
    } else {
        dstRect.left = paddingWidth / 2;
        if (dstRect.left % 2 != 0) {
            dstRect.left -= dstRect.left % 2;
            if (dstRect.left < 0) {
                dstRect.left = 0;
            }
        }
        dstRect.right = dstRect.left + resizeWidth - 1;
        leftOffset = dstRect.left;
    }

    c2_trace("convert: scale %f dstRect(%d,%d,%d,%d) offset(left %d top %d) pad %dx%d",
             scale, dstRect.left, dstRect.top, dstRect.right, dstRect.bottom,
             leftOffset, topOffset, paddingWidth, paddingHeight);

    // set offset and scale
    if (letterbox != nullptr){
        letterbox->scale = scale;
        letterbox->xPad  = leftOffset;
        letterbox->yPad  = topOffset;
    }

    return c2_preprocess_convert_image_with_rga(srcImage, modelImage, &srcRect, &dstRect);
}

bool c2_postprocess_output_model_image(
        PostProcessContext ctx, rknn_output *outputs, objectDetectResultList *odResults) {
    PostProcessContextImpl *impl = (PostProcessContextImpl*)ctx;
    if (!impl || !odResults || !outputs) {
        c2_err("invalid null post-process context");
        return false;
    }

    float     *protoData = impl->protoData;
    LetterBox *letterbox = impl->letterbox;
    rknn_tensor_attr *nnAttrs = impl->nnOutputAttr;

    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int>   classId;
    std::vector<float> filterSegments;
    std::vector<float> filterSegmentsByNms;

    int validCount  = 0;
    int modelWidth  = SEG_MODEL_WIDTH;
    int modelHeight = SEG_MODEL_HEIGHT;

    // memset result count first
    odResults->count = 0;

    for (int i = 0; i < SEG_OUT_CHN_NUM; i++) {
        int gridH = nnAttrs[i].dims[2];
        int gridW = nnAttrs[i].dims[3];
        int stride = modelHeight / gridH;
        bool quant = (nnAttrs->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                      nnAttrs->type != RKNN_TENSOR_FLOAT16);

        if (quant) {
            validCount += _process_i8(
                    outputs, i, (int *)gAnchor[i / 2], gridH, gridW,
                    modelHeight, modelWidth, stride, filterBoxes,
                    filterSegments, protoData, objProbs,
                    classId, BOX_THRESH, nnAttrs, impl->vectorB);
        } else {
            validCount += _process_fp32(
                    outputs, i, (int *)gAnchor[i / 2], gridH, gridW,
                    modelHeight, modelWidth, stride, filterBoxes,
                    filterSegments, protoData, objProbs, classId, BOX_THRESH);
        }
    }


    if (validCount <= 0) {
        // not found detect object
        return true;
    }

    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i) {
        indexArray.push_back(i);
    }

    _quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    std::set<int> classSet(std::begin(classId), std::end(classId));
    for (auto c : classSet) {
        // non-maximum suppression
        _nms(validCount, filterBoxes, classId, indexArray, c, NMS_THRESH);
    }

    int finalBoxNum = 0;

    for (int i = 0; i < validCount; ++i) {
        if (indexArray[i] == -1 || finalBoxNum >= SEG_NUMB_MAX_SIZE)
            continue;

        int n = indexArray[i];
        if (classId[n] != 0 /* LABEL_PERSON */)
            continue;

        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float objConf = objProbs[i];

        for (int k = 0; k < PROTO_CHANNEL; k++)
            filterSegmentsByNms.push_back(filterSegments[n * PROTO_CHANNEL + k]);

        odResults->results[finalBoxNum].box.left = x1;
        odResults->results[finalBoxNum].box.top = y1;
        odResults->results[finalBoxNum].box.right = x2;
        odResults->results[finalBoxNum].box.bottom = y2;

        odResults->results[finalBoxNum].prop = objConf;
        odResults->results[finalBoxNum].clsId = id;
        odResults->count++;
        finalBoxNum++;
    }

    if (odResults->count == 0) {
        return true;
    }

    float *filterBoxesByNms = (float*)malloc(finalBoxNum * 4 * sizeof(float));
    int clsId[finalBoxNum];

    for (int i = 0; i < finalBoxNum; i++) {
        // for crop_mask
        filterBoxesByNms[i * 4 + 0] = odResults->results[i].box.left;
        filterBoxesByNms[i * 4 + 1] = odResults->results[i].box.top;
        filterBoxesByNms[i * 4 + 2] = odResults->results[i].box.right;
        filterBoxesByNms[i * 4 + 3] = odResults->results[i].box.bottom;
        clsId[i] = odResults->results[i].clsId;

        // get real box
        odResults->results[i].box.left = _box_reverse(
                odResults->results[i].box.left,
                modelWidth, letterbox->xPad, letterbox->scale);
        odResults->results[i].box.top = _box_reverse(
                odResults->results[i].box.top, modelHeight,
                letterbox->yPad, letterbox->scale);
        odResults->results[i].box.right = _box_reverse(
                odResults->results[i].box.right, modelWidth,
                letterbox->xPad, letterbox->scale);
        odResults->results[i].box.bottom = _box_reverse(
                odResults->results[i].box.bottom, modelHeight,
                letterbox->yPad, letterbox->scale);

        if (odResults->results[i].box.right > impl->originWidth)
            odResults->results[i].box.right = impl->originWidth;
        if (odResults->results[i].box.bottom > impl->originHeight)
            odResults->results[i].box.bottom = impl->originHeight;
    }

    // For non_seg encode version, get detection box and return.
    if (!impl->resultMask) {
        return true;
    }

    memset(impl->matmulOut, 0, finalBoxNum * PROTO_HEIGHT * PROTO_WEIGHT);
    memset(impl->allMaskInOne, 0, modelWidth * modelHeight * sizeof(uint8_t));
    memset(impl->segMask, 0, SEG_NUMB_MAX_SIZE * SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT);
    memset(impl->croppedSegMask, 0, SEG_MODEL_WIDTH * SEG_MODEL_HEIGHT);

    // compute the mask through matmul
    uint8_t *allMaskInOne   = impl->allMaskInOne;
    uint8_t *segMask        = impl->segMask;
    uint8_t *matmulOut      = impl->matmulOut;
    uint8_t *croppedSegMask = impl->croppedSegMask;

    int rowsA = finalBoxNum;
    int colsA = PROTO_CHANNEL;

    _matmul_by_npu_fp(impl, filterSegmentsByNms, matmulOut, rowsA, colsA);

    _resize_by_rga_uint8(
            matmulOut, PROTO_WEIGHT, PROTO_HEIGHT,
            1, segMask, modelWidth, modelHeight);

    _crop_mask_uint8_merge(
            segMask, allMaskInOne, filterBoxesByNms,
            finalBoxNum, clsId, modelWidth, modelHeight);

    // get real mask
    int croppedH = modelHeight - letterbox->yPad * 2;
    int croppedW = modelWidth - letterbox->xPad * 2;
    int oriInH = impl->originHeight;
    int oriInW = impl->originWidth;
    int yPad = letterbox->yPad;
    int xPad = letterbox->xPad;

    _seg_reverse(
            allMaskInOne, croppedSegMask,
            odResults->resultsSeg[0].segMask, /* output segMaskReal */
            modelHeight, modelWidth, croppedH, croppedW,
            oriInH, oriInW, yPad, xPad);

    C2_SAFE_FREE(filterBoxesByNms)

    return true;
}

bool c2_postprocess_seg_mask_to_class_map(
        PostProcessContext ctx, bool isHevc,
        objectDetectResultList *odResults, objectMapResultList *omResults) {
    PostProcessContextImpl *impl = (PostProcessContextImpl*)ctx;
    if (!impl) {
        c2_err("invalid null post-process context");
        return false;
    }

    if (!impl->resultMask) {
        return true;  // no need
    }

    omResults->foundObjects = 0;

    int blockNum = 0;
    int blkPosX, blkPosY;
    int ctuSize = isHevc ? 32 : 16;
    uint8_t *objectMap = impl->omResultMap;
    uint8_t *segMask = odResults->resultsSeg[0].segMask;

    // output seg mask dump
    static std::string dumpResult;
    static char dumpBuffer[10];

    // if more than one object, convert the object map
    if (odResults->count >= 1) {
        omResults->foundObjects = 1;

        for (int h = 0; h < impl->originHeight; h += ctuSize) {
            for (int w = 0; w < impl->originWidth; w += ctuSize) {
                for (int i = 0; i < ctuSize / 16; i++) {
                    for (int j = 0; j < ctuSize / 16; j++) {
                        blkPosX = w + j * 16;
                        blkPosY = h + i * 16;
                        // calculate the number of pixels (in a 16x16 block) in each category
                        _get_blk_object(
                                blkPosX, blkPosY, impl->originWidth,
                                impl->originHeight, segMask, objectMap, blockNum);
                        // dump output seg mask line after line
                        if (impl->dumpFp) {
                            if (objectMap[blockNum] == 0) {
                                dumpResult.append("  ");
                            } else {
                                sprintf(dumpBuffer, "%d ", objectMap[blockNum]);
                                dumpResult.append(dumpBuffer);
                            }
                        }
                        blockNum++;
                    }
                }
            }
            if (impl->dumpFp) {
                dumpResult.append("\n");
            }
        }
    }

    if (impl->dumpFp) {
        dumpResult.append("\n");
        fwrite(dumpResult.c_str(), 1, dumpResult.size(), impl->dumpFp);
        fflush(impl->dumpFp);
        // dump only once
        fclose(impl->dumpFp);
    }

    omResults->objectSegMap = impl->omResultMap;
    return true;
}

bool c2_postprocess_copy_image_buffer(ImageBuffer *srcImage, ImageBuffer *dstImage) {
    im_handle_param_t srcParam;
    rga_buffer_handle_t srcHandle;

    srcParam.width  = srcImage->hstride;
    srcParam.height = srcImage->vstride;
    srcParam.format = _toRgaFormat(srcImage->format);

    if (srcImage->fd > 0) {
        srcHandle = importbuffer_fd(srcImage->fd, &srcParam);
    } else {
        srcHandle = importbuffer_virtualaddr(srcImage->virAddr, &srcParam);
    }
    if (srcHandle <= 0) {
        c2_err("src handle error");
        return false;
    }

    im_handle_param_t dstParam;
    rga_buffer_handle_t dstHandle;

    dstParam.width  = dstImage->hstride;
    dstParam.height = dstImage->vstride;
    dstParam.format = _toRgaFormat(dstImage->format);

    if (dstImage->fd > 0) {
        dstHandle = importbuffer_fd(dstImage->fd, &dstParam);
    } else {
        dstHandle = importbuffer_virtualaddr(dstImage->virAddr, &dstParam);
    }
    if (dstHandle <= 0) {
        c2_err("dst handle error");
        return false;
    }

    rga_buffer_t src, dst;

    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    src = wrapbuffer_handle(
            srcHandle, srcImage->width, srcImage->height,
            _toRgaFormat(srcImage->format), srcImage->hstride, srcImage->vstride);
    dst = wrapbuffer_handle(
            dstHandle, dstImage->width, dstImage->height,
            _toRgaFormat(dstImage->format), dstImage->hstride, dstImage->vstride);

    int err = imcopy(src, dst);

    releasebuffer_handle(srcHandle);
    releasebuffer_handle(dstHandle);

    return (err > 0);
}

bool c2_postprocess_draw_rect_array(
        ImageBuffer *srcImage, objectDetectResultList *odResults) {
    if (odResults->count <= 0) {
        return true;
    }

    im_rect faceRect[SEG_NUMB_MAX_SIZE] = {};

    for (int i = 0; i < odResults->count; i++) {
        faceRect[i].x = odResults->results[i].box.left & (~0x01);
        faceRect[i].y = odResults->results[i].box.top & (~0x01);
        faceRect[i].width = (odResults->results[i].box.right -
                             odResults->results[i].box.left) & (~0x01);
        faceRect[i].height = (odResults->results[i].box.bottom -
                              odResults->results[i].box.top) & (~0x01);
        c2_trace("draw face[%d] - [%d %d %d %d]", i,
                 faceRect[i].x, faceRect[i].y, faceRect[i].width, faceRect[i].height);
    }

    rga_buffer_t src;
    memset(&src, 0, sizeof(src));

    im_handle_param_t param;
    rga_buffer_handle_t handle;

    param.width = srcImage->hstride;
    param.height = srcImage->vstride;
    param.format = _toRgaFormat(srcImage->format);

    if (srcImage->fd > 0) {
        handle = importbuffer_fd(srcImage->fd, &param);
    } else {
        handle = importbuffer_virtualaddr(srcImage->virAddr, &param);
    }

    if (handle <= 0) {
        c2_err("src handle error");
        return false;
    }

    src = wrapbuffer_handle(
            handle, srcImage->width, srcImage->height,
            _toRgaFormat(srcImage->format), srcImage->hstride, srcImage->vstride);

    int err = imrectangleArray(src, faceRect, odResults->count, 0x0000ff, 2);

    releasebuffer_handle(handle);

    return (err > 0);
}

}
