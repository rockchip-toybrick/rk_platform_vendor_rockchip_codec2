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
 * limitations under the License.00
 *
 */

#ifndef ANDROID_C2_RK_GRAPHIC_BUFFER_MAPPER_H__
#define ANDROID_C2_RK_GRAPHIC_BUFFER_MAPPER_H__

#include <stdint.h>
#include <cutils/native_handle.h>
#include <utils/Errors.h>

#include <C2AllocatorGralloc.h>

using namespace android;

typedef struct rkvdec_scaling_metadata_t {
    uint64_t version;
    // mask
    uint64_t requestMask;
    uint64_t replyMask;

    // buffer info
    uint32_t width;   // pixel_w
    uint32_t height;  // pixel_h
    uint32_t format;  // drm_fourcc
    uint64_t modifier;// modifier
    uint32_t usage;   // usage
    uint32_t pixelStride; // pixel_stride

    // image info
    uint32_t srcLeft;
    uint32_t srcTop;
    uint32_t srcRight;
    uint32_t srcBottom;

    // buffer layout
    uint32_t layerCnt;
    uint32_t fd[4];
    uint32_t offset[4];
    uint32_t byteStride[4];
} rkvdec_scaling_metadata_t;

class C2RKGraphicBufferMapper {
public:
    static C2RKGraphicBufferMapper* get() {
        static C2RKGraphicBufferMapper _gInstance;
        return &_gInstance;
    }

    int32_t  getMapperVersion();
    int32_t  getShareFd(buffer_handle_t handle);
    int32_t  getWidth(buffer_handle_t handle);
    int32_t  getHeight(buffer_handle_t handle);
    int32_t  getFormatRequested(buffer_handle_t handle);
    int32_t  getAllocationSize(buffer_handle_t handle);
    int32_t  getPixelStride(buffer_handle_t handle);
    int32_t  getByteStride(buffer_handle_t handle);
    uint64_t getUsage(buffer_handle_t handle);
    uint64_t getBufferId(buffer_handle_t handle);

    // The imported outHandle must be freed with freeBuffer when no longer
    // needed. c2Handle is owned by the caller.
    status_t importBuffer(const C2Handle *const c2Handle, buffer_handle_t *outHandle);
    status_t freeBuffer(buffer_handle_t handle);

    /* rk mapper metadata */
    int32_t  setDynamicHdrMeta(buffer_handle_t handle, int64_t offset);
    int64_t  getDynamicHdrMeta(buffer_handle_t handle);
    int32_t  mapScaleMeta(buffer_handle_t handle, rkvdec_scaling_metadata_t** metadata);
    int32_t  unmapScaleMeta(buffer_handle_t handle);

private:
    C2RKGraphicBufferMapper();
    virtual ~C2RKGraphicBufferMapper() {};

    int32_t mMapperVersion;
};

#endif /* ANDROID_C2_RK_GRAPHIC_BUFFER_MAPPER_H__ */
