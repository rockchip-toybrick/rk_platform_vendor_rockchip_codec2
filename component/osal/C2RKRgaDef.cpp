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

#include <string.h>

#include "C2RKRgaDef.h"
#include "C2RKLogger.h"
#include "im2d.h"
#include "RockchipRga.h"
#include "hardware/hardware_rockchip.h"

namespace android {

C2_LOGGER_ENABLE("C2RKRgaDef");

struct RgaFormtMap {
    int halFmt;
    int rgaFmt;
    const char *name;
};

static const RgaFormtMap kRgaFormatMaps[] = {
    { HAL_PIXEL_FORMAT_RGB_565,         RK_FORMAT_RGB_565,          "rgb565" },
    { HAL_PIXEL_FORMAT_RGB_888,         RK_FORMAT_RGB_888,          "rgb888" },
    { HAL_PIXEL_FORMAT_RGBA_8888,       RK_FORMAT_RGBA_8888,        "rgba8888" },
    { HAL_PIXEL_FORMAT_BGRA_8888,       RK_FORMAT_BGRA_8888,        "bgra8888" },
    { HAL_PIXEL_FORMAT_YCrCb_NV12,      RK_FORMAT_YCbCr_420_SP,     "nv12" },
    { HAL_PIXEL_FORMAT_YCrCb_NV12_10,   RK_FORMAT_YCbCr_420_SP_10B, "nv12_10" },
};

static const size_t kNumRgaFormatType = sizeof(kRgaFormatMaps) / sizeof(kRgaFormatMaps[0]);


static int getRgaFormat(int halFmt) {
    for (int i = 0; i < kNumRgaFormatType; i++) {
        if (kRgaFormatMaps[i].halFmt == halFmt) {
            return kRgaFormatMaps[i].rgaFmt;
        }
    }
    return RK_FORMAT_UNKNOWN;
}

static const char* toStr_format(int halFmt) {
    for (int i = 0; i < kNumRgaFormatType; i++) {
        if (kRgaFormatMaps[i].halFmt == halFmt) {
            return kRgaFormatMaps[i].name;
        }
    }
    return "unknown";
}

rga_buffer_handle_t importRgaBuffer(RgaInfo *info, int32_t format) {
    im_handle_param_t imParam;

    std::ignore = memset(&imParam, 0, sizeof(im_handle_param_t));

    imParam.width  = info->hstride;
    imParam.height = info->vstride;
    imParam.format = format;

    return importbuffer_fd(info->fd, &imParam);
}

void freeRgaBuffer(rga_buffer_handle_t handle) {
    std::ignore = releasebuffer_handle(handle);
}

void C2RKRgaDef::SetRgaInfo(
        RgaInfo *info, int32_t fd, int32_t format,
        int32_t width, int32_t height, int32_t hstride, int32_t vstride) {
    std::ignore = memset(info, 0, sizeof(RgaInfo));

    info->fd = fd;
    info->format = format;
    info->width = width;
    info->height = height;
    info->hstride = (hstride > 0) ? hstride : width;
    info->vstride = (vstride > 0) ? vstride : height;
}

bool C2RKRgaDef::DoBlit(RgaInfo srcInfo, RgaInfo dstInfo) {
    int err = 0;
    rga_buffer_t src, dst;
    rga_buffer_handle_t srcHdl, dstHdl;

    int32_t srcRgaFmt = getRgaFormat(srcInfo.format);
    int32_t dstRgaFmt = getRgaFormat(dstInfo.format);

    if (srcRgaFmt == RK_FORMAT_UNKNOWN || dstRgaFmt == RK_FORMAT_UNKNOWN) {
        Log.E("[RgaBlit]: unsupport fmt, src %d dst %d", srcInfo.format, dstInfo.format);
        return false;
    }

    Log.D("[RgaBlit]: src fd %d rect[%d, %d, %d, %d] fmt %s",
           srcInfo.fd, srcInfo.width, srcInfo.height,
           srcInfo.hstride, srcInfo.vstride, toStr_format(srcInfo.format));
    Log.D("[RgaBlit]: dst fd %d rect[%d, %d, %d, %d] fmt %s",
           dstInfo.fd, dstInfo.width, dstInfo.height,
           dstInfo.hstride, dstInfo.vstride, toStr_format(dstInfo.format));

    std::ignore = memset((void*)&src, 0, sizeof(rga_buffer_t));
    std::ignore = memset((void*)&dst, 0, sizeof(rga_buffer_t));

    srcHdl = importRgaBuffer(&srcInfo, srcRgaFmt);
    dstHdl = importRgaBuffer(&dstInfo, dstRgaFmt);
    if (!srcHdl || !dstHdl) {
        Log.E("[RgaBlit]: failed to import rga buffer");
        return false;
    }

    src = wrapbuffer_handle(
            srcHdl, srcInfo.width, srcInfo.height,
            srcRgaFmt, srcInfo.hstride, srcInfo.vstride);
    dst = wrapbuffer_handle(
            dstHdl, dstInfo.width, dstInfo.height,
            dstRgaFmt, dstInfo.hstride, dstInfo.vstride);

    err = improcess(src, dst, {}, {}, {}, {}, IM_SYNC);
    if (err <= 0) {
        Log.E("[RgaBlit]: error %d", err);
        Log.E("[RgaBlit]: src fd %d rect[%d, %d, %d, %d] fmt %s",
               srcInfo.fd, srcInfo.width, srcInfo.height,
               srcInfo.hstride, srcInfo.vstride, toStr_format(srcInfo.format));
        Log.E("[RgaBlit]: dst fd %d rect[%d, %d, %d, %d] fmt %s",
               dstInfo.fd, dstInfo.width, dstInfo.height,
               dstInfo.hstride, dstInfo.vstride, toStr_format(dstInfo.format));
    }

    freeRgaBuffer(srcHdl);
    freeRgaBuffer(dstHdl);

    return (err > 0);
}

} // namespace android
