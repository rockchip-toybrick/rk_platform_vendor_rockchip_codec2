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

#ifndef ANDROID_C2_RK_RGA_DEF_H__
#define ANDROID_C2_RK_RGA_DEF_H__

#include <stdio.h>

typedef enum {
    RGA_COLOR_SPACE_DEFAULT = 0,
    RGA_YUV_TO_RGB_BT601_LIMIT,
    RGA_YUV_TO_RGB_BT601_FULL,
    RGA_YUV_TO_RGB_BT709_LIMIT,
    RGA_RGB_TO_YUV_BT601_LIMIT,
    RGA_RGB_TO_YUV_BT601_FULL,
    RGA_RGB_TO_YUV_BT709_LIMIT,
} RgaColorSpaceMode;

typedef struct {
    int32_t fd;
    int32_t format;
    int32_t width;
    int32_t height;
    int32_t hstride;
    int32_t vstride;
} RgaInfo;

class C2RKRgaDef {
public:
    static void SetRgaInfo(
            RgaInfo *param, int32_t fd, int32_t format,
            int32_t width, int32_t height, int32_t hstride = 0, int32_t vstride = 0);

    static bool DoBlit(RgaInfo srcInfo, RgaInfo dstInfo, int colorSpaceMode = 0);
};

#endif  // ANDROID_C2_RK_RGA_DEF_H__
