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

#ifndef ANDROID_C2_RK_NALU_PARSER_H__
#define ANDROID_C2_RK_NALU_PARSER_H__

#include "C2RKBitReader.h"

class C2RKNaluParser {
public:
    static int32_t detectBitDepth(uint8_t *buf, int32_t size, int32_t coding);
    static int32_t detectMaxRefCount(uint8_t *buf, int32_t size, int32_t coding);

private:
    /* Supported lists for InputFormat */
    typedef enum {
        C2_DETECT_FIELD_DEPTH = 0,
        C2_DETECT_FIELD_MAX_REF_COUNT,
        C2_DETECT_FIELD_BUTT,
    } MyDetectField;

    static bool searchAVCNaluInfo(
            uint8_t *buf, int32_t size, int32_t detectFiled, int32_t *outValue);
    static bool searchHEVCNalSPS(
            BitReadContext *gb, int32_t detectFiled, int32_t *outValue);
    static bool searchHEVCNalVPS(
            BitReadContext *gb, int32_t detectFiled, int32_t *outValue);
    static bool searchHEVCNalUnit(
            uint8_t *buf, int32_t size, int32_t detectFiled, int32_t *outValue);
    static bool searchHEVCNaluInfo(
            uint8_t *buf, int32_t size, int32_t detectFiled, int32_t *outValue);
};

#endif  // ANDROID_C2_RK_NALU_PARSER_H__
