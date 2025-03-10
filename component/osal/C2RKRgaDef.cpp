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

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKRgaDef"

#include <string.h>

#include "C2RKRgaDef.h"
#include "C2RKLog.h"
#include "im2d.h"
#include "RockchipRga.h"
#include "hardware/hardware_rockchip.h"

using namespace android;

rga_buffer_handle_t importRgaBuffer(RgaInfo *info, int32_t format) {
    im_handle_param_t imParam;

    memset(&imParam, 0, sizeof(im_handle_param_t));

    imParam.width  = info->wstride;
    imParam.height = info->hstride;
    imParam.format = format;

    return importbuffer_fd(info->fd, &imParam);
}

void freeRgaBuffer(rga_buffer_handle_t handle) {
    releasebuffer_handle(handle);
}

void C2RKRgaDef::SetRgaInfo(
        RgaInfo *info, int32_t fd, int32_t format,
        int32_t width, int32_t height, int32_t wstride, int32_t hstride) {
    memset(info, 0, sizeof(RgaInfo));

    info->fd = fd;
    info->format = format;
    info->width = width;
    info->height = height;
    info->wstride = (wstride > 0) ? wstride : width;
    info->hstride = (hstride > 0) ? hstride : height;
}

bool C2RKRgaDef::DoBlit(RgaInfo srcInfo, RgaInfo dstInfo) {
    bool ret = true;

    rga_info_t src;
    rga_info_t dst;
    rga_buffer_handle_t srcHdl;
    rga_buffer_handle_t dstHdl;

    RockchipRga& rkRga(RockchipRga::get());

    c2_trace("src fd %d rect[%d, %d, %d, %d] fmt 0x%x",
              srcInfo.fd, srcInfo.width, srcInfo.height,
              srcInfo.wstride, srcInfo.hstride, srcInfo.format);
    c2_trace("dst fd %d rect[%d, %d, %d, %d] fmt 0x%x",
              dstInfo.fd, dstInfo.width, dstInfo.height,
              dstInfo.wstride, dstInfo.hstride, dstInfo.format);

    if ((srcInfo.wstride % 4) != 0) {
        c2_warn("err yuv not align to 4");
        return true;
    }

    memset((void*)&src, 0, sizeof(rga_info_t));
    memset((void*)&dst, 0, sizeof(rga_info_t));

    srcHdl = importRgaBuffer(&srcInfo, srcInfo.format);
    dstHdl = importRgaBuffer(&dstInfo, dstInfo.format);
    if (!srcHdl || !dstHdl) {
        c2_err("failed to import rga buffer");
        return false;
    }

    src.handle = srcHdl;
    dst.handle = dstHdl;
    rga_set_rect(&src.rect, 0, 0, srcInfo.width, srcInfo.height,
                 srcInfo.wstride, srcInfo.hstride, srcInfo.format);
    rga_set_rect(&dst.rect, 0, 0, dstInfo.width, dstInfo.height,
                 dstInfo.wstride, dstInfo.hstride, dstInfo.format);

    if (rkRga.RkRgaBlit(&src, &dst, NULL)) {
        ret = false;
    }

    freeRgaBuffer(srcHdl);
    freeRgaBuffer(dstHdl);

    return ret;
}
