/*
 * Copyright 2024 Rockchip Electronics Co. LTD
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

#include "C2RKPropsDef.h"
#include <cutils/properties.h>

static bool propInit();
static int32_t sIsUseSpsOutputDelay = 0;
static int32_t sLoadingCheckDisable = 0;
static int32_t sHdrDisable = 0;
static int32_t sScaleDisable = 0;
static int32_t sEncSuperMode = 0;
static bool sPropInited = propInit();

static bool propInit() {
    sIsUseSpsOutputDelay = property_get_int32("codec2_use_sps_output_delay", 0);

    sHdrDisable = property_get_int32("codec2_hdr_meta_disable", 0);

    sScaleDisable = property_get_int32("codec2_scale_disable", 0);

    sEncSuperMode = property_get_int32("codec2_enc_super_mode", 0);

    sLoadingCheckDisable = property_get_int32("codec2_disable_load_check", 0);

    return true;
}

int32_t C2RKPropsDef::getIsUseSpsOutputDelay() {
    return sIsUseSpsOutputDelay;
}

int32_t C2RKPropsDef::getHdrDisable() {
    return sHdrDisable;
}

int32_t C2RKPropsDef::getScaleDisable() {
    return sScaleDisable;
}

int32_t C2RKPropsDef::getEncSuperMode() {
    return sEncSuperMode;
}

int32_t C2RKPropsDef::getLoadingCheckDisable() {
    return sLoadingCheckDisable;
}

