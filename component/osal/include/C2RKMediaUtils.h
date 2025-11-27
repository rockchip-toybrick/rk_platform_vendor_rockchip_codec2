/*
 * Copyright (C) 2020 Rockchip Electronics Co. LTD
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

#ifndef ANDROID_C2_RK_MEDIA_UTILS_H_
#define ANDROID_C2_RK_MEDIA_UTILS_H_

#include "rk_mpi.h"
#include "hardware/hardware_rockchip.h"
#include "hardware/gralloc_rockchip.h"

namespace android {

#ifndef HAL_PIXEL_FORMAT_YUV420_8BIT_RFBC
#define HAL_PIXEL_FORMAT_YUV420_8BIT_RFBC   0x200
#endif

#ifndef HAL_PIXEL_FORMAT_YUV422_8BIT_RFBC
#define HAL_PIXEL_FORMAT_YUV422_8BIT_RFBC   0x202
#endif

#ifndef HAL_PIXEL_FORMAT_YUV420_10BIT_RFBC
#define HAL_PIXEL_FORMAT_YUV420_10BIT_RFBC  0x201
#endif

#ifndef HAL_PIXEL_FORMAT_YUV422_10BIT_RFBC
#define HAL_PIXEL_FORMAT_YUV422_10BIT_RFBC  0x203
#endif

#ifndef HAL_PIXEL_FORMAT_YUV444_8BIT_RFBC
#define HAL_PIXEL_FORMAT_YUV444_8BIT_RFBC   0x204
#endif

#ifndef HAL_PIXEL_FORMAT_YUV444_10BIT_RFBC
#define HAL_PIXEL_FORMAT_YUV444_10BIT_RFBC  0x205
#endif

#ifndef HAL_PIXEL_FORMAT_NV30
#define HAL_PIXEL_FORMAT_NV30   30
#endif

#ifndef GRALLOC_USAGE_RKVDEC_SCALING
#define GRALLOC_USAGE_RKVDEC_SCALING    0x1000000
#endif

#define C2_DEFAULT_REF_FRAME_COUNT  12
#define C2_MAX_REF_FRAME_COUNT      21

#define C2_MAX(a, b)                ((a) > (b) ? (a) : (b))
#define C2_MIN(a, b)                ((a) < (b) ? (a) : (b))
#define C2_ALIGN(x, a)              (((x)+(a)-1)&~((a)-1))
#define C2_IS_ALIGNED(x, a)         (!((x) & ((a)-1)))
#define C2_ALIGN_ODD(x, a)          (((x)+(a)-1)&~((a)-1) | a)
#define C2_CLIP(a, l, h)            ((a) < (l) ? (l) : ((a) > (h) ? (h) : (a)))
#define C2_ARRAY_ELEMS(a)           (sizeof(a) / sizeof((a)[0]))

typedef struct {
    uint8_t *ptr;
    int32_t fd;
    int32_t format;
    int32_t width;
    int32_t height;
    int32_t hstride;
    int32_t vstride;
} C2FrameInfo;

class C2RKMediaUtils {
public:
    // get hal pixer format from mpp format
    static int32_t  getHalPixerFormat(int32_t format);

    // get hal stride alignment usage if support
    static uint64_t getStrideUsage(int32_t width, int32_t stride);
    static uint64_t getHStrideUsage(int32_t height, int32_t hstride);

    // calculate video refCount on the basis of max Dpb Mbs and level
    static uint32_t calculateVideoRefCount(
                MppCodingType type, int32_t width, int32_t height, int32_t level);

    // HAL_PIXEL_FORMAT_YCBCR_P010 requirement was added in T VSR, although
    // it could have been supported prior to this.
    static bool isP010Allowed();

    // frame converter, software processing
    static void translateToRequestFmt(
            C2FrameInfo srcInfo, C2FrameInfo dstInfo, bool cacheSync = false);
    static void convert10BitNV12ToP010(
            C2FrameInfo srcInfo, C2FrameInfo dstInfo, bool cacheSync = false);
    static void convert10BitNV12ToNV12(
            C2FrameInfo srcInfo, C2FrameInfo dstInfo, bool cacheSync = false);
    static void convertNV12ToNV12(
            C2FrameInfo srcInfo, C2FrameInfo dstInfo, bool cacheSync = false);
};

} // namespace android

#endif  // ANDROID_C2_RK_MEDIA_UTILS_H_
