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

#ifndef ANDROID_C2_RK_POST_PROCESS_H__
#define ANDROID_C2_RK_POST_PROCESS_H__

#include <stdio.h>

#include "C2RKYolov5Session.h"

namespace android {

#define OBJ_NUMB_MAX_SIZE   16

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} ImageRect;

typedef struct {
    int   xPad;
    int   yPad;
    float scale;
} LetterBox;

typedef struct {
    int      foundObjects;
    uint8_t *objectSegMap;
} objectMapResultList;

typedef struct {
    ImageRect box;
    float     prop;
    int32_t   clsId;
} objectDetectResult;

typedef struct {
    uint8_t *segMask;
} objectSegmentResult;

typedef struct {
    int id;
    int count;
    objectDetectResult results[OBJ_NUMB_MAX_SIZE];
    objectSegmentResult resultsSeg[OBJ_NUMB_MAX_SIZE];
} objectDetectResultList;

/**
 * @brief init post-process context
 *
 * @param originImage [in] Source Image
 * @param outputAttr [in] rknn output attrs
 * @param ctx [out] post-process context
 * @return bool
 */
bool c2_postprocess_init_context(
        PostProcessContext *ctx, ImageBuffer *originImage, rknn_tensor_attr *outputAttr);

/**
 * @brief deinit post-process context
 *
 * @param ctx [in] post-process context
 * @return bool
 */
bool c2_postprocess_deinit_context(PostProcessContext ctx);

/**
 * @brief convert src image to dst model image
 *
 * @param ctx [in] post-process context
 * @param srcImage [in] Source Image
 * @param modelImage [out] Target Model Image
 * @return bool
 */
bool c2_preprocess_convert_model_image(
        PostProcessContext ctx, ImageBuffer *srcImage, ImageBuffer *modelImage);

/**
 * @brief process model rknn output and get odResults
 * @param ctx [in] post-process context
 * @param outputs [in] rknn output
 * @param odResults [out] detect results
 * @return bool
 */
bool c2_postprocess_output_model_image(
        PostProcessContext ctx, rknn_output *outputs, objectDetectResultList *results);

/**
 * @brief process od result to class map
 *
 * @param ctx [in] post-process context
 * @param isHevc [in] Input is hevc
 * @param odResults [in] Model odResults
 * @param omResults [out] cleass map
 * @return bool
 */
bool c2_postprocess_seg_mask_to_class_map(
        PostProcessContext ctx, bool isHevc,
        objectDetectResultList *odResults, objectMapResultList *omResults);

/**
 * @brief copy src image to dst image
 *
 * @param srcImage [in] Source Image
 * @param dstImage [out] Dst image
 * @return bool
 */
bool c2_postprocess_copy_image_buffer(ImageBuffer *srcImage, ImageBuffer *dstImage);

/**
 * @brief draw detect object rect array
 *
 * @param srcImage [in][out] Source Image
 * @param odResults [in] Model odResults
 * @return bool
 */
bool c2_postprocess_draw_rect_array(
        ImageBuffer *srcImage, objectDetectResultList *odResults);

}

#endif // ANDROID_C2_RK_POST_PROCESS_H__

