/*
 * Copyright (C) 2024 Rockchip Electronics Co. LTD
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

#ifndef C2_RK_AI_QP_CONFIG_H_
#define C2_RK_AI_QP_CONFIG_H_

#include <cstdlib>
#include <cstring>
#include <cutils/properties.h>

#define AIPQ_UTILS_PROPERTY_ACT_RES         "vendor.vpp.act_res"
#define AIPQ_UTILS_PROPERTY_VIR_RES         "vendor.vpp.vir_res"
#define AIPQ_UTILS_PROPERTY_META_ENABLE     "vendor.vpp.aipq.meta_enable"

#define ENABLE_AIPQ 1

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t horStride;
    uint32_t verStride;
    uint32_t metaEnable;
} C2RKPQConfig;

inline bool c2_get_ai_qp_config(C2RKPQConfig &config) {
#if ENABLE_AIPQ
    char actRes[PROPERTY_VALUE_MAX] = {0};
    char virRes[PROPERTY_VALUE_MAX] = {0};
    bool success = false;

    memset(&config, 0, sizeof(config));
    if (property_get(AIPQ_UTILS_PROPERTY_ACT_RES, actRes, nullptr) > 0
        && property_get(AIPQ_UTILS_PROPERTY_VIR_RES, virRes, nullptr) > 0) {
        sscanf(actRes, "%dx%d", &config.width, &config.height);
        sscanf(virRes, "%dx%d", &config.horStride, &config.verStride);
        if (property_get(AIPQ_UTILS_PROPERTY_META_ENABLE, virRes, nullptr) > 0)
            config.metaEnable = atoi(virRes);
        success = true;
    }

    return success;
#endif
    return false;
}

#endif  // C2_RK_AI_QP_CONFIG_H_