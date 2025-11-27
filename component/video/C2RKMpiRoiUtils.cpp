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

#include <stdlib.h>
#include <string.h>

#include "C2RKLogger.h"
#include "C2RKChipCapDef.h"
#include "C2RKMpiRoiUtils.h"
#include "C2RKDmaBufSync.h"

namespace android {

C2_LOGGER_ENABLE("C2RKMpiRoiUtils");

#define _ALIGN(x, a)            (((x)+(a)-1)&~((a)-1))
#define _FREE(ptr)              do { if(ptr) free(ptr); ptr = NULL; } while (0)
#define _MALLOC(type, count)    (type*) malloc(sizeof(type) * (count));
#define _CALLOC(type, count)    (type*) calloc(count, sizeof(type));

#define VEPU541_MAX_ROI_NUM     8
#define CU_BASE_CFG_BYTE        64
#define CU_QP_CFG_BYTE          192

typedef enum RoiType_e {
    ROI_TYPE_AUTO       = -2,
    ROI_TYPE_NONE       = -1,
    ROI_TYPE_0          = 0,    /* vepu roi, not support yet */
    ROI_TYPE_1          = 1,    /* rv1109/rk3566/rk3568 roi */
    ROI_TYPE_2          = 2,    /* rk3588 roi */

    /* legacy  */
    ROI_TYPE_LEGACY     = 0x10, /* legacy region config way */
    ROI_TYPE_BUTT,
} RoiType;

typedef struct Vepu541RoiCfg_t {
    /*
     * Force_intra
     * 1 - The corresponding 16x16cu is forced to be intra
     * 0 - Not force to intra
     */
    uint16_t force_intra  : 1;
    uint16_t reserved     : 3;
    /*
     * Qp area index
     * The choosed qp area index.
     */
    uint16_t qp_area_idx  : 3;
    /*
     * Area qp limit function enable flag
     * Force to be true in vepu541
     */
    uint16_t qp_area_en   : 1;
    /*
     * Qp_adj
     * Qp_adj
     * in absolute qp mode qp_adj is the final qp used by encoder
     * in relative qp mode qp_adj is a adjustment to final qp
     */
    int16_t qp_adj        : 7;
    /*
     * Qp_adj_mode
     * Qp adjustment mode
     * 1 - absolute qp mode:
     *     the 16x16 MB qp is set to the qp_adj value
     * 0 - relative qp mode
     *     the 16x16 MB qp is adjusted by qp_adj value
     */
    uint16_t qp_adj_mode  : 1;
} Vepu541RoiCfg;

typedef struct Vepu580RoiH264BsCfg_t {
    uint64_t force_inter   : 42;
    uint64_t mode_mask     : 9;
    uint64_t reserved      : 10;
    uint64_t force_intra   : 1;
    uint64_t qp_adj_en     : 1;
    uint64_t amv_en        : 1;
} Vepu580RoiH264BsCfg;

typedef struct Vepu580RoiH265BsCfg_t {
    uint8_t amv_en        : 1;
    uint8_t qp_adj        : 1;
    uint8_t force_split   : 1;
    uint8_t force_intra   : 2;
    uint8_t force_inter   : 2;
} Vepu580RoiH265BsCfg;

typedef struct Vepu580RoiQpCfg_t {
    uint16_t reserved     : 4;
    /*
     * Qp area index
     * The choosed qp area index.
     */
    uint16_t qp_area_idx  : 4;
    /*
     * Qp_adj
     * Qp_adj
     * in absolute qp mode qp_adj is the final qp used by encoder
     * in relative qp mode qp_adj is a adjustment to final qp
     */
    int16_t qp_adj       : 7;
    /*
     * Qp_adj_mode
     * Qp adjustment mode
     * 1 - absolute qp mode:
     *     the 16x16 MB qp is set to the qp_adj value
     * 0 - relative qp mode
     *     the 16x16 MB qp is adjusted by qp_adj value
     */
    uint16_t qp_adj_mode  : 1;
} Vepu580RoiQpCfg;

typedef struct MppEncRoiImpl_t {
    /* common parameters */
    int32_t             w;
    int32_t             h;
    MppCodingType       type;

    /* region config set */
    RoiRegionCfg        *regions;
    int32_t             max_count;
    int32_t             count;

    /*
     * roi_type is for the different encoder roi config
     *
     * 0 - rv1109/rk3566/rk3568 roi
     * 1 - rk3588 roi
     *
     * others - legacy roi config
     */
    RoiType             roi_type;

    /* For roi type legacy config */
    MppEncROICfg        legacy_roi_cfg;
    MppEncROIRegion     *legacy_roi_region;

    /* For roi type 1&2 config */
    MppBufferGroup      roi_grp;
    MppEncROICfg2       roi_cfg;

    /* buffer address and size of MppBuffer in MppEncROICfg2 */
    void                *dst_base;
    void                *dst_qp;
    void                *dst_amv;
    void                *dst_mv;
    uint32_t            base_cfg_size;
    uint32_t            qp_cfg_size;
    uint32_t            amv_cfg_size;
    uint32_t            mv_cfg_size;

    uint8_t             *cu_map;
    uint32_t            cu_size;

    /* tmp buffer for convert vepu54x roi cfg to vepu58x roi cfg */
    Vepu541RoiCfg       *tmp;
} MppEncRoiImpl;

static uint32_t raster2scan8[64] = {
    0,  1,  4,  5,  16, 17, 20, 21,
    2,  3,  6,  7,  18, 19, 22, 23,
    8,  9,  12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63
};

static uint32_t raster2zscan16[16] = {
    0,  1,  4,  5,
    2,  3,  6,  7,
    8,  9,  12, 13,
    10, 11, 14, 15
};

static MPP_RET vepu54x_h265_set_roi(void *dst_buf, void *src_buf, int32_t w, int32_t h) {
    Vepu541RoiCfg *src = (Vepu541RoiCfg *)src_buf;
    Vepu541RoiCfg *dst = (Vepu541RoiCfg *)dst_buf;
    int32_t mb_w = _ALIGN(w, 64) / 64;
    int32_t mb_h = _ALIGN(h, 64) / 64;
    int32_t ctu_line = mb_w;
    int32_t j;

    for (j = 0; j < mb_h; j++) {
        int32_t i;

        for (i = 0; i < mb_w; i++) {
            int32_t ctu_addr = j * ctu_line + i;
            int32_t cu16_num_line = ctu_line * 4;
            int32_t cu16cnt;

            for (cu16cnt = 0; cu16cnt < 16; cu16cnt++) {
                int32_t cu16_addr_in_frame;
                int32_t cu16_x = cu16cnt % 4;
                int32_t cu16_y = cu16cnt / 4;

                cu16_x += i * 4;
                cu16_y += j * 4;
                cu16_addr_in_frame = cu16_x + cu16_y * cu16_num_line;

                dst[ctu_addr * 16 + cu16cnt] = src[cu16_addr_in_frame];
            }
        }
    }

    return MPP_OK;
}

static MPP_RET gen_vepu54x_roi(MppEncRoiImpl *ctx, Vepu541RoiCfg *dst) {
    RoiRegionCfg *region = ctx->regions;
    int32_t mb_w = _ALIGN(ctx->w, 16) / 16;
    int32_t mb_h = _ALIGN(ctx->h, 16) / 16;
    int32_t stride_h = _ALIGN(mb_w, 4);
    int32_t stride_v = _ALIGN(mb_h, 4);
    Vepu541RoiCfg cfg;
    MPP_RET ret = MPP_NOK;
    int32_t i;

    memset(ctx->cu_map, 0, ctx->cu_size);

    cfg.force_intra = 0;
    cfg.reserved    = 0;
    cfg.qp_area_idx = 0;
    cfg.qp_area_en  = 1;
    cfg.qp_adj      = 0;
    cfg.qp_adj_mode = 0;

    /* step 1. reset all the config */
    for (i = 0; i < stride_h * stride_v; i++)
        memcpy(dst + i, &cfg, sizeof(cfg));

    if (ctx->w <= 0 || ctx->h <= 0) {
        Log.E("invalid size [%d:%d]", ctx->w, ctx->h);
        goto done;
    }

    /* check region config */
    ret = MPP_OK;
    for (i = 0; i < ctx->count; i++, region++) {
        if (region->x + region->w > ctx->w || region->y + region->h > ctx->h)
            ret = MPP_NOK;

        if (region->force_intra > 1 || region->qp_mode > 1)
            ret = MPP_NOK;

        if ((region->qp_mode && region->qp_val > 51) ||
            (!region->qp_mode && (region->qp_val > 51 || region->qp_val < -51)))
            ret = MPP_NOK;

        if (ret) {
            Log.E("region %d invalid param:", i);
            Log.E("position [%d:%d:%d:%d] vs [%d:%d]",
                   region->x, region->y, region->w, region->h, ctx->w, ctx->h);
            Log.E("force intra %d qp mode %d val %d",
                   region->force_intra, region->qp_mode, region->qp_val);
            goto done;
        }
    }

    region = ctx->regions;
    /* step 2. setup region for top to bottom */
    for (i = 0; i < ctx->count; i++, region++) {
        Vepu541RoiCfg *p = dst;
        uint8_t *map = ctx->cu_map;
        int32_t roi_width  = (region->w + 15) / 16;
        int32_t roi_height = (region->h + 15) / 16;
        int32_t pos_x_init = (region->x + 15) / 16;
        int32_t pos_y_init = (region->y + 15) / 16;
        int32_t x, y;

        cfg.force_intra = region->force_intra;
        cfg.reserved    = 0;
        cfg.qp_area_idx = 0;
        // NOTE: When roi is enabled the qp_area_en should be one.
        cfg.qp_area_en  = 1; // region->area_map_en;
        cfg.qp_adj      = region->qp_val;
        cfg.qp_adj_mode = region->qp_mode;

        p += pos_y_init * stride_h + pos_x_init;
        map += pos_y_init * stride_h + pos_x_init;
        for (y = 0; y < roi_height; y++) {
            for (x = 0; x < roi_width; x++) {
                memcpy(p + x, &cfg, sizeof(cfg));
                if (ctx->type == MPP_VIDEO_CodingAVC) {
                    *(map + x) = 1;
                }

            }
            p += stride_h;
            map += stride_h;
        }

        if (ctx->type == MPP_VIDEO_CodingHEVC) {
            map = ctx->cu_map;
            uint32_t stride_cu64_h = stride_h * 16 / 64;

            roi_width  = (region->w + 64) / 64;
            roi_height = (region->h + 64) / 64;

            if (region->x < 64) {
                pos_x_init = 0;
                roi_width += 2;
            } else if (region->x % 64) {
                pos_x_init = (region->x - 64) / 64;
                roi_width += 2;
            } else
                pos_x_init = region->x / 64;

            if (region->y < 64) {
                pos_y_init = 0;
                roi_height += 2;
            } else if (region->y % 64) {
                pos_y_init = (region->y - 64) / 64;
                roi_height += 2;
            } else
                pos_y_init = region->y / 64;

            map += pos_y_init * stride_cu64_h + pos_x_init;
            for (y = 0; y < roi_height; y++) {
                for (x = 0; x < roi_width; x++) {
                    *(map + x) = 1;
                }
                map += stride_cu64_h;
            }
        }
    }

done:
    return ret;
}


static MPP_RET set_roi_pos_val(uint32_t *buf, uint32_t pos, uint32_t value) {
    uint32_t index = pos / 32;
    uint32_t bits  = pos % 32;

    buf[index] = buf[index] | (value << bits);
    return MPP_OK;
}

#define set_roi_qpadj(buf, index, val) \
    do { \
        uint32_t offset = 425 + index; \
        set_roi_pos_val(buf, offset, val); \
    } while(0)

#define set_roi_force_split(buf, index, val) \
    do { \
        uint32_t offset = 340 + index; \
        set_roi_pos_val(buf, offset, val); \
    } while(0)

#define set_roi_force_intra(buf, index, val) \
    do { \
        uint32_t offset = 170 + index * 2; \
        set_roi_pos_val(buf, offset, val); \
    } while(0)

#define set_roi_force_inter(buf, index, val) \
    do { \
        uint32_t offset = index * 2; \
        set_roi_pos_val(buf, offset, val); \
    } while(0)

static void set_roi_cu8_base_cfg(uint32_t *buf, uint32_t index, Vepu580RoiH265BsCfg val) {
    set_roi_qpadj(buf, index, val.qp_adj);
    set_roi_force_split(buf, index, val.force_split);
    set_roi_force_intra(buf, index, val.force_intra);
    set_roi_force_inter(buf, index, val.force_inter);
}

static void set_roi_cu16_base_cfg(uint32_t *buf, uint32_t index, Vepu580RoiH265BsCfg val) {
    index += 64;
    set_roi_qpadj(buf, index, val.qp_adj);
    set_roi_force_split(buf, index, val.force_split);
    set_roi_force_intra(buf, index, val.force_intra);
    set_roi_force_inter(buf, index, val.force_inter);
}

static void set_roi_cu32_base_cfg(uint32_t *buf, uint32_t index, Vepu580RoiH265BsCfg val) {
    index += 80;
    set_roi_qpadj(buf, index, val.qp_adj);
    set_roi_force_split(buf, index, val.force_split);
    set_roi_force_intra(buf, index, val.force_intra);
    set_roi_force_inter(buf, index, val.force_inter);
}

static void set_roi_cu64_base_cfg(uint32_t *buf, Vepu580RoiH265BsCfg val) {
    set_roi_qpadj(buf, 84, val.qp_adj);
    set_roi_force_split(buf, 84, val.force_split);
    set_roi_force_intra(buf, 84, val.force_intra);
    set_roi_force_inter(buf, 84, val.force_inter);
}

static void set_roi_qp_cfg(void *buf, uint32_t index, Vepu541RoiCfg *cfg) {
    Vepu580RoiQpCfg *qp_cfg_base = (Vepu580RoiQpCfg *)buf;
    Vepu580RoiQpCfg *qp_cfg = &qp_cfg_base[index];

    qp_cfg->qp_adj = cfg->qp_adj;
    qp_cfg->qp_adj_mode = cfg->qp_adj_mode;
    qp_cfg->qp_area_idx = cfg->qp_area_idx;
}

#define set_roi_cu8_qp_cfg(buf, index, cfg) \
    do { \
        uint32_t offset = index; \
        set_roi_qp_cfg(buf, offset, cfg); \
    } while(0)

#define set_roi_cu16_qp_cfg(buf, index, cfg) \
    do { \
        uint32_t offset = 64 + index; \
        set_roi_qp_cfg(buf, offset, cfg); \
    } while(0)

#define set_roi_cu32_qp_cfg(buf, index, cfg) \
    do { \
        uint32_t offset = 80 + index; \
        set_roi_qp_cfg(buf, offset, cfg); \
    } while(0)

#define set_roi_cu64_qp_cfg(buf, cfg) \
    do { \
        uint32_t offset = 84; \
        set_roi_qp_cfg(buf, offset, cfg); \
    } while(0)

void set_roi_amv(uint32_t *buf, Vepu580RoiH265BsCfg val) {
    set_roi_pos_val(buf, 511, val.amv_en);
}

static MPP_RET gen_vepu580_roi_h264(MppEncRoiImpl *ctx) {
    int32_t mb_w = _ALIGN(ctx->w, 16) / 16;
    int32_t mb_h = _ALIGN(ctx->h, 16) / 16;
    int32_t stride_h = _ALIGN(mb_w, 4);
    int32_t stride_v = _ALIGN(mb_h, 4);
    int32_t roi_buf_size = stride_h * stride_v * 8;
    int32_t roi_qp_size = stride_h * stride_v * 2;

    Vepu541RoiCfg *src = (Vepu541RoiCfg *)ctx->tmp;
    Vepu580RoiQpCfg *dst_qp = (Vepu580RoiQpCfg *)ctx->dst_qp;
    Vepu580RoiH264BsCfg *dst_base = (Vepu580RoiH264BsCfg *)ctx->dst_base;
    int32_t j, k;

    if (!src || !dst_qp || !dst_base)
        return MPP_NOK;

    memset(dst_base, 0, roi_buf_size);
    memset(dst_qp, 0, roi_qp_size);

    for (j = 0; j < mb_h; j++) {
        for (k = 0; k < stride_h; k++) {
            if (ctx->cu_map[j * stride_h + k]) {
                Vepu541RoiCfg *cu_cfg = &src[j * stride_h + k];
                Vepu580RoiQpCfg *qp_cfg = &dst_qp[j * stride_h + k];
                Vepu580RoiH264BsCfg *base_cfg = &dst_base[j * stride_h + k];

                qp_cfg->qp_adj = cu_cfg->qp_adj;
                qp_cfg->qp_adj_mode = cu_cfg->qp_adj_mode;
                qp_cfg->qp_area_idx = cu_cfg->qp_area_idx;
                base_cfg->force_intra = cu_cfg->force_intra;
                base_cfg->qp_adj_en = !!cu_cfg->qp_adj;
#if 0
                if (j < 8 && k < 8) {
                    RK_U64 *tmp = (RK_U64 *)base_cfg;
                    RK_U16 *qp = (RK_U16 *)qp_cfg;

                    mpp_log("force_intra %d, qp_adj_en %d qp_adj %d, qp_adj_mode %d",
                            base_cfg->force_intra, base_cfg->qp_adj_en, qp_cfg->qp_adj, qp_cfg->qp_adj_mode);
                    mpp_log("val low %8x hight %8x", *tmp & 0xffffffff, ((*tmp >> 32) & 0xffffffff));

                    mpp_log("qp cfg %4x", *qp);
                }
#endif
            }
        }
    }

    return MPP_OK;
}

void set_roi_cu16_split_cu8(uint32_t *buf, uint32_t cu16index, Vepu580RoiH265BsCfg val) {
    int32_t cu16_x = cu16index % 4;
    int32_t cu16_y = cu16index / 4;
    uint32_t cu8cnt;

    for (cu8cnt = 0; cu8cnt < 4; cu8cnt++) {
        uint32_t zindex = 0;
        uint32_t cu8_x = cu8cnt % 2;
        uint32_t cu8_y = cu8cnt / 2;
        uint32_t cu8raster_index = (cu16_y * 2 + cu8_y) * 8 + cu16_x * 2 + cu8_x;

        zindex = raster2scan8[cu8raster_index];
        set_roi_cu8_base_cfg(buf, zindex, val);
    }
}

static MPP_RET gen_vepu580_roi_h265(MppEncRoiImpl *ctx) {
    int32_t ctu_w = _ALIGN(ctx->w, 64) / 64;
    int32_t ctu_h = _ALIGN(ctx->h, 64) / 64;
    int32_t roi_buf_size = ctu_w * ctu_h * 64;
    int32_t roi_qp_size =  ctu_w * ctu_h * 256;
    int32_t ctu_line = ctu_w;

    Vepu541RoiCfg *src = (Vepu541RoiCfg *)ctx->tmp;
    uint8_t *dst_qp = (uint8_t *)ctx->dst_qp;
    uint32_t *dst_base = (uint32_t *)ctx->dst_base;
    int32_t i, j, k, cu16cnt;

    if (!src || !dst_qp || !dst_base)
        return MPP_NOK;

    memset(dst_qp, 0, roi_qp_size);
    memset(dst_base, 0, roi_buf_size);

    for (j = 0; j < ctu_h; j++) {
        for (k = 0; k < ctu_w; k++) {
            int32_t cu16_num_line = ctu_line * 4;
            uint32_t adjust_cnt = 0;

            if (ctx->cu_map[j * ctu_w + k]) {
                for (cu16cnt = 0; cu16cnt < 16; cu16cnt++) {
                    int32_t cu16_x;
                    int32_t cu16_y;
                    int32_t cu16_addr_in_frame;
                    uint32_t zindex = 0;
                    Vepu541RoiCfg *cu16_cfg = NULL;
                    Vepu580RoiH265BsCfg val;

                    memset(&val, 0, sizeof(val));
                    cu16_x = cu16cnt & 3;
                    cu16_y = cu16cnt / 4;
                    cu16_x += k * 4;
                    cu16_y += j * 4;
                    cu16_addr_in_frame = cu16_x + cu16_y * cu16_num_line;
                    cu16_cfg = &src[cu16_addr_in_frame];
                    zindex = raster2zscan16[cu16cnt];

                    val.force_intra = cu16_cfg->force_intra;
                    val.qp_adj = !!cu16_cfg->qp_adj;
                    if (val.force_intra || val.qp_adj) {
                        adjust_cnt++;
                    }

                    set_roi_cu16_split_cu8(dst_base, cu16cnt, val);
                    set_roi_cu16_base_cfg(dst_base, zindex, val);
                    set_roi_cu16_qp_cfg(dst_qp, zindex, cu16_cfg);
                    /*
                     * if all cu16 adjust c64 and cu32 must adjust
                     * or we will force split to cu 16
                     */
                    if (adjust_cnt == 16 && cu16cnt == 15) {
                        // cu64
                        set_roi_cu64_base_cfg(dst_base, val);
                        set_roi_cu64_qp_cfg(dst_qp, cu16_cfg);
                        // cu32
                        for (i = 0; i < 4; i++) {
                            set_roi_cu32_base_cfg(dst_base, i, val);
                            set_roi_cu32_qp_cfg(dst_qp, i, cu16_cfg);
                        }

                        for (i = 0; i < 64; i ++) {
                            set_roi_cu8_base_cfg(dst_base, i, val);
                            set_roi_qp_cfg(dst_qp, i, cu16_cfg);
                        }
                    } else if (cu16cnt == 15 && adjust_cnt > 0) {
                        val.force_split = 1;
                        set_roi_force_split(dst_base, 84, val.force_split);
                        for (i = 0; i < 4; i++) {
                            set_roi_force_split(dst_base, 80 + i, val.force_split);
                        }
                        for (i = 0; i < 16; i++) {
                            set_roi_force_split(dst_base, 64 + i, val.force_split);
                        }
                    }
                }
            }
#if 0
            if (j < 3 && (k < 3 )) {
                mpp_log("j %d, k %d need_update = %d", j, k, need_update);
                RK_U16 *qp_val = (RK_U16 *)dst_qp;
                for (i = 0; i < CU_BASE_CFG_BYTE / 4; i++) {
                    mpp_log("cfg[%d] = %08x", i, dst_base[i]);
                }
                for (i = 0; i < CU_QP_CFG_BYTE / 2; i++) {
                    mpp_log("qp[%d] = %04x", i, qp_val[i]);
                }
            }
#endif
            dst_base += CU_BASE_CFG_BYTE / 4;
            dst_qp += CU_QP_CFG_BYTE;
        }
    }
    return MPP_OK;
}


MPP_RET mpp_enc_roi_init(MppEncRoiCtx *ctx, int32_t w, int32_t h, MppCodingType type) {
    C2ChipType chip_type = C2RKChipCapDef::get()->getChipType();
    RoiType roi_type = ROI_TYPE_AUTO;
    MppEncRoiImpl *impl = NULL;
    MPP_RET ret = MPP_NOK;

    switch (chip_type) {
    case RK_CHIP_1126 :
    case RK_CHIP_356X :{
        roi_type = ROI_TYPE_1;
    } break;
    case RK_CHIP_3588 : {
        roi_type = ROI_TYPE_2;
    } break;
    default : {
        roi_type = ROI_TYPE_LEGACY;
        Log.I("%s run with legacy roi cfg", C2RKChipCapDef::get()->getChipName());
    } break;
    }

    impl = _CALLOC(MppEncRoiImpl, 1);
    if (!impl) {
        Log.E("can't create roi context");
        goto done;
    }

    impl->w = w;
    impl->h = h;
    impl->type = type;
    impl->roi_type = roi_type;
    impl->max_count = MPP_MAX_ROI_REGION_COUNT;
    impl->regions = _CALLOC(RoiRegionCfg, MPP_MAX_ROI_REGION_COUNT);

    switch (roi_type) {
    case ROI_TYPE_1 : {
        int32_t mb_w = _ALIGN(impl->w, 16) / 16;
        int32_t mb_h = _ALIGN(impl->h, 16) / 16;
        uint32_t ctu_w = _ALIGN(impl->w, 64) / 64;
        uint32_t ctu_h  = _ALIGN(impl->w, 64) / 64;
        int32_t stride_h = _ALIGN(mb_w, 4);
        int32_t stride_v = _ALIGN(mb_h, 4);

        Log.D("set to vepu54x roi generation");

        impl->base_cfg_size = stride_h * stride_v * sizeof(Vepu541RoiCfg);
        mpp_buffer_group_get_internal(&impl->roi_grp, MPP_BUFFER_TYPE_ION | MPP_BUFFER_FLAGS_CACHABLE);

        mpp_buffer_get(impl->roi_grp, &impl->roi_cfg.base_cfg_buf, impl->base_cfg_size);
        if (!impl->roi_cfg.base_cfg_buf) {
            goto done;
        }
        impl->dst_base = mpp_buffer_get_ptr(impl->roi_cfg.base_cfg_buf);

        /* create tmp buffer for hevc */
        if (type == MPP_VIDEO_CodingHEVC) {
            impl->tmp = _CALLOC(Vepu541RoiCfg, stride_h * stride_v);
            impl->cu_map  = _CALLOC(uint8_t, ctu_w * ctu_h);
            impl->cu_size = ctu_w * ctu_h;
            if (!impl->tmp || !impl->cu_map)
                goto done;
        } else {
            impl->cu_map   = _CALLOC(uint8_t, mb_w * mb_h);
            impl->cu_size  = mb_w * mb_h;
            if (!impl->cu_map)
                goto done;
        }

        ret = MPP_OK;
    } break;
    case ROI_TYPE_2 : {
        if (type == MPP_VIDEO_CodingHEVC) {
            uint32_t ctu_w = _ALIGN(w, 64) / 64;
            uint32_t ctu_h  = _ALIGN(h, 64) / 64;

            impl->base_cfg_size = ctu_w * ctu_h * 64;
            impl->qp_cfg_size   = ctu_w * ctu_h * 256;
            impl->amv_cfg_size  = ctu_w * ctu_h * 512;
            impl->mv_cfg_size   = ctu_w * ctu_h * 4;
            impl->cu_map        = _CALLOC(uint8_t, ctu_w * ctu_h);
            impl->cu_size       = ctu_w * ctu_h;
        } else {
            uint32_t mb_w = _ALIGN(w, 64) / 16;
            uint32_t mb_h = _ALIGN(h, 64) / 16;

            impl->base_cfg_size = mb_w * mb_h * 8;
            impl->qp_cfg_size   = mb_w * mb_h * 2;
            impl->amv_cfg_size  = mb_w * mb_h / 4;
            impl->mv_cfg_size   = mb_w * mb_h * 96 / 4;
            impl->cu_map        = _CALLOC(uint8_t, mb_w * mb_h);
            impl->cu_size       = mb_w * mb_h;
        }

        Log.D("set to vepu58x roi generation");

        impl->roi_cfg.roi_qp_en = 1;
        mpp_buffer_group_get_internal(&impl->roi_grp, MPP_BUFFER_TYPE_ION | MPP_BUFFER_FLAGS_CACHABLE);
        mpp_buffer_get(impl->roi_grp, &impl->roi_cfg.base_cfg_buf, impl->base_cfg_size);
        if (!impl->roi_cfg.base_cfg_buf) {
            goto done;
        }
        impl->dst_base = mpp_buffer_get_ptr(impl->roi_cfg.base_cfg_buf);
        mpp_buffer_get(impl->roi_grp, &impl->roi_cfg.qp_cfg_buf, impl->qp_cfg_size);
        if (!impl->roi_cfg.qp_cfg_buf) {
            goto done;
        }
        impl->dst_qp = mpp_buffer_get_ptr(impl->roi_cfg.qp_cfg_buf);
        mpp_buffer_get(impl->roi_grp, &impl->roi_cfg.amv_cfg_buf, impl->amv_cfg_size);
        if (!impl->roi_cfg.amv_cfg_buf) {
            goto done;
        }
        impl->dst_amv = mpp_buffer_get_ptr(impl->roi_cfg.amv_cfg_buf);
        mpp_buffer_get(impl->roi_grp, &impl->roi_cfg.mv_cfg_buf, impl->mv_cfg_size);
        if (!impl->roi_cfg.mv_cfg_buf) {
            goto done;
        }
        impl->dst_mv = mpp_buffer_get_ptr(impl->roi_cfg.mv_cfg_buf);

        {
            // create tmp buffer for vepu54x H.264 config first
            int32_t mb_w = _ALIGN(impl->w, 16) / 16;
            int32_t mb_h = _ALIGN(impl->h, 16) / 16;
            int32_t stride_h = _ALIGN(mb_w, 4);
            int32_t stride_v = _ALIGN(mb_h, 4);

            impl->tmp = _CALLOC(Vepu541RoiCfg, stride_h * stride_v);
            if (!impl->tmp)
                goto done;
        }
        ret = MPP_OK;
    } break;
    case ROI_TYPE_LEGACY : {
        impl->legacy_roi_region = _CALLOC(MppEncROIRegion, MPP_MAX_ROI_REGION_COUNT);
        impl->legacy_roi_cfg.regions = impl->legacy_roi_region;
        ret = MPP_OK;
    } break;
    default : {
    } break;
    }

done:
    if (ret) {
        if (impl) {
            mpp_enc_roi_deinit(impl);
            impl = NULL;
        }
    }

    *ctx = impl;
    return ret;
}

MPP_RET mpp_enc_roi_deinit(MppEncRoiCtx ctx) {
    MppEncRoiImpl *impl = (MppEncRoiImpl *)ctx;

    if (!impl)
        return MPP_OK;

    if (impl->roi_cfg.base_cfg_buf) {
        mpp_buffer_put(impl->roi_cfg.base_cfg_buf);
        impl->roi_cfg.base_cfg_buf = NULL;
    }

    if (impl->roi_cfg.qp_cfg_buf) {
        mpp_buffer_put(impl->roi_cfg.qp_cfg_buf);
        impl->roi_cfg.qp_cfg_buf = NULL;
    }
    if (impl->roi_cfg.amv_cfg_buf) {
        mpp_buffer_put(impl->roi_cfg.amv_cfg_buf);
        impl->roi_cfg.amv_cfg_buf = NULL;
    }
    if (impl->roi_cfg.mv_cfg_buf) {
        mpp_buffer_put(impl->roi_cfg.mv_cfg_buf);
        impl->roi_cfg.mv_cfg_buf = NULL;
    }

    if (impl->roi_grp) {
        mpp_buffer_group_put(impl->roi_grp);
        impl->roi_grp = NULL;
    }
    _FREE(impl->cu_map);
    _FREE(impl->legacy_roi_region);
    _FREE(impl->regions);
    _FREE(impl->tmp);
    _FREE(impl);

    return MPP_OK;
}

MPP_RET mpp_enc_roi_add_region(MppEncRoiCtx ctx, RoiRegionCfg *region) {
    MppEncRoiImpl *impl = (MppEncRoiImpl *)ctx;

    if (impl->count >= impl->max_count) {
        Log.E("can not add more region with max %d\n", impl->max_count);
        return MPP_NOK;
    }

    memcpy(impl->regions + impl->count, region, sizeof(*impl->regions));
    impl->count++;

    return MPP_OK;
}

MPP_RET mpp_enc_roi_setup_meta(MppEncRoiCtx ctx, MppMeta meta) {
    MppEncRoiImpl *impl = (MppEncRoiImpl *)ctx;

    switch (impl->roi_type) {
    case ROI_TYPE_1 : {
        switch (impl->type) {
        case MPP_VIDEO_CodingAVC : {
            gen_vepu54x_roi(impl, (Vepu541RoiCfg *)impl->dst_base);
        } break;
        case MPP_VIDEO_CodingHEVC : {
            gen_vepu54x_roi(impl, impl->tmp);
            vepu54x_h265_set_roi(impl->dst_base, impl->tmp, impl->w, impl->h);
        } break;
        default : {
        } break;
        }

        mpp_meta_set_ptr(meta, KEY_ROI_DATA2, (void*)&impl->roi_cfg);
        dma_sync_cpu_to_device(mpp_buffer_get_fd(impl->roi_cfg.base_cfg_buf));
    } break;
    case ROI_TYPE_2 : {
        gen_vepu54x_roi(impl, impl->tmp);

        switch (impl->type) {
        case MPP_VIDEO_CodingAVC : {
            gen_vepu580_roi_h264(impl);
        } break;
        case MPP_VIDEO_CodingHEVC : {
            gen_vepu580_roi_h265(impl);
        } break;
        default : {
        } break;
        }

        mpp_meta_set_ptr(meta, KEY_ROI_DATA2, (void*)&impl->roi_cfg);
        dma_sync_cpu_to_device(mpp_buffer_get_fd(impl->roi_cfg.base_cfg_buf));
        dma_sync_cpu_to_device(mpp_buffer_get_fd(impl->roi_cfg.qp_cfg_buf));
    } break;
    case ROI_TYPE_LEGACY : {
        MppEncROIRegion *region = impl->legacy_roi_region;
        MppEncROICfg *roi_cfg = &impl->legacy_roi_cfg;
        RoiRegionCfg *regions = impl->regions;
        int32_t i;

        for (i = 0; i < impl->count; i++) {
            region[i].x = regions[i].x;
            region[i].y = regions[i].y;
            region[i].w = regions[i].w;
            region[i].h = regions[i].h;

            region[i].intra = regions[i].force_intra;
            region[i].abs_qp_en = regions[i].qp_mode;
            region[i].quality = regions[i].qp_val;
            region[i].area_map_en = 1;
            region[i].qp_area_idx = 0;
        }

        roi_cfg->number = impl->count;
        roi_cfg->regions = region;

        mpp_meta_set_ptr(meta, KEY_ROI_DATA, (void*)roi_cfg);
    } break;
    default : {
    } break;
    }

    impl->count = 0;

    return MPP_OK;
}

}
