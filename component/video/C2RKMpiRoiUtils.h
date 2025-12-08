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

#include "rk_venc_cmd.h"

namespace android {

#define MPP_MAX_ROI_REGION_COUNT    8

typedef void* MppEncRoiCtx;

/*
 * NOTE: this structure is changeful. Do NOT expect binary compatible on it.
 */
struct RoiRegionCfg {
    int32_t x;              /**< horizontal position of top left corner */
    int32_t y;              /**< vertical position of top left corner */
    int32_t w;              /**< width of ROI rectangle */
    int32_t h;              /**< height of ROI rectangle */

    int32_t force_intra;    /**< flag of forced intra macroblock */
    int32_t qp_mode;        /**< 0 - relative qp 1 - absolute qp */
    int32_t qp_val;         /**< absolute / relative qp of macroblock */
};

MPP_RET mpp_enc_roi_init(MppEncRoiCtx *ctx, int32_t w, int32_t h, MppCodingType type);
void    mpp_enc_roi_deinit(MppEncRoiCtx ctx);

MPP_RET mpp_enc_roi_add_region(MppEncRoiCtx ctx, RoiRegionCfg *region);
MPP_RET mpp_enc_roi_setup_meta(MppEncRoiCtx ctx, MppMeta meta);

}
