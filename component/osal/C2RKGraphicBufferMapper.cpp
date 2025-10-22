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

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKGraphicBufferMapper"

#include "C2RKGraphicBufferMapper.h"
#include "C2RKLog.h"

#include <hardware/hardware_rockchip.h>
#include <ui/GraphicBufferMapper.h>
#include <hardware/gralloc.h>
#include <android/hardware/graphics/mapper/4.0/IMapper.h>

using android::hardware::graphics::mapper::V4_0::Error;
using android::hardware::graphics::mapper::V4_0::IMapper;
using android::hardware::hidl_vec;

/* Gralloc2 rk mapper metadata  */
#define PERFORM_SET_OFFSET_OF_DYNAMIC_HDR_METADATA           0x08100017
#define PERFORM_GET_OFFSET_OF_DYNAMIC_HDR_METADATA           0x08100018
#define PERFORM_LOCK_RKVDEC_SCALING_METADATA                 0x08100019
#define PERFORM_UNLOCK_RKVDEC_SCALING_METADATA               0x0810001A

/* Gralloc4 rk mapper metadata  */
#define OFFSET_OF_DYNAMIC_HDR_METADATA      (1)
#define GRALLOC_RK_METADATA_TYPE_NAME       "rk.graphics.RkMetadataType"
const static IMapper::MetadataType RkMetadataType_OFFSET_OF_DYNAMIC_HDR_METADATA {
    GRALLOC_RK_METADATA_TYPE_NAME,
    OFFSET_OF_DYNAMIC_HDR_METADATA
};


static const gralloc_module_t* getGralloc2Module() {
    static const gralloc_module_t *cachedModule = NULL;
    if (cachedModule == NULL) {
        const hw_module_t* module = NULL;
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) != 0) {
            c2_err("Failed to open gralloc module");
        }
        cachedModule = reinterpret_cast<const gralloc_module_t*>(module);
    }
    return cachedModule;
}

static IMapper &getGralloc4Mapper() {
    static android::sp<IMapper> cachedService = IMapper::getService();
    return *cachedService;
}

static status_t decodeRkOffsetOfVideoMetadata(
        const hidl_vec<uint8_t>& input, int64_t* offsetOfMetadata) {
    int64_t offset = 0;

    memcpy(&offset, input.data(), sizeof(offset));

    *offsetOfMetadata = offset;

    return OK;
}

static status_t encodeRkOffsetOfVideoMetadata(
        const int64_t offset, hidl_vec<uint8_t>* output) {
    output->resize(1 * sizeof(int64_t));

    memcpy(output->data(), &offset, sizeof(offset));

    return OK;
}

C2RKGraphicBufferMapper::C2RKGraphicBufferMapper() {
    mMapperVersion = GraphicBufferMapper::get().getMapperVersion();
    c2_info("init with mapper version %d", mMapperVersion);
}

int32_t C2RKGraphicBufferMapper::getShareFd(buffer_handle_t handle) {
    if (handle && handle->numFds > 0) {
        return handle->data[0];
    }

    return -1;
}

int32_t C2RKGraphicBufferMapper::getWidth(buffer_handle_t handle) {
    uint64_t width = 0;

    status_t err = GraphicBufferMapper::get().getWidth(handle, &width);
    if (err != OK) {
        c2_err("Failed to get width. err : %d", err);
        return -1;
    }

    return (int32_t)width;
}

int32_t C2RKGraphicBufferMapper::getHeight(buffer_handle_t handle) {
    uint64_t height = 0;

    int err = GraphicBufferMapper::get().getHeight(handle, &height);
    if (err != OK) {
        c2_err("Failed to get height. err : %d", err);
        return -1;
    }

    return (int32_t)height;
}

int32_t C2RKGraphicBufferMapper::getFormatRequested(buffer_handle_t handle) {
    android::ui::PixelFormat format;

    int err = GraphicBufferMapper::get().getPixelFormatRequested(handle, &format);
    if (err != OK) {
        c2_err("Failed to get pixel_format_requested. err : %d", err);
        return -1;
    }

    return (int32_t)format;
}

int32_t C2RKGraphicBufferMapper::getAllocationSize(buffer_handle_t handle) {
    uint64_t size = 0;

    int err = GraphicBufferMapper::get().getAllocationSize(handle, &size);
    if (err != OK) {
        c2_err("Failed to get allocation_size. err : %d", err);
        return -1;
    }

    return (int32_t)size;
}

int32_t C2RKGraphicBufferMapper::getPixelStride(buffer_handle_t handle) {
    std::vector<ui::PlaneLayout> layouts;
    int32_t formatRequested = 0;

    formatRequested = getFormatRequested(handle);
    if (formatRequested < 0) {
        c2_err("err formatRequested: %d", formatRequested);
        return -1;
    }

    if (formatRequested != HAL_PIXEL_FORMAT_YCrCb_NV12_10) {
        status_t err = GraphicBufferMapper::get().getPlaneLayouts(handle, &layouts);
        if (err != OK || layouts.size() < 1) {
            c2_err("Failed to get plane layouts. err : %d", err);
            return err;
        }

        if (layouts.size() > 1) {
            c2_warn("it's not reasonable to get global pixel_stride with planes more than 1.");
        }

        return (int32_t)(layouts[0].widthInSamples);
    } else {
        int32_t width = getWidth(handle);
        if (width <= 0) {
            c2_err("err width : %d", width);
            return -1;
        }

        return width;
    }
}

int32_t C2RKGraphicBufferMapper::getByteStride(buffer_handle_t handle) {
    std::vector<ui::PlaneLayout> layouts;
    int32_t formatRequested = 0;

    formatRequested = getFormatRequested(handle);
    if (formatRequested < 0) {
        c2_err("err formatRequested: %d", formatRequested);
        return -1;
    }

    if (formatRequested != HAL_PIXEL_FORMAT_YCrCb_NV12_10) {
        status_t err = GraphicBufferMapper::get().getPlaneLayouts(handle, &layouts);
        if (err != OK || layouts.size() < 1) {
            c2_err("Failed to get plane layouts. err : %d", err);
            return err;
        }

        if (layouts.size() > 1) {
            c2_warn("it's not reasonable to get global pixel_stride with planes more than 1.");
        }

        return (int32_t)(layouts[0].strideInBytes);
    } else {
        int32_t width = getWidth(handle);
        if (width <= 0) {
            c2_err("err width : %d", width);
            return -1;
        }

        return width;
    }
}

uint64_t C2RKGraphicBufferMapper::getUsage(buffer_handle_t handle) {
    uint64_t usage = 0;

    int err = GraphicBufferMapper::get().getUsage(handle, &usage);
    if (err != OK) {
        c2_err("Failed to get usage. err : %d", err);
        return 0;
    }

    return usage;
}

uint64_t C2RKGraphicBufferMapper::getBufferId(buffer_handle_t handle) {
    uint64_t id = 0;

    int err = GraphicBufferMapper::get().getBufferId(handle, &id);
    if (err != OK) {
        c2_err("Failed to get buffer id. err : %d", err);
        return 0;
    }

    return id;
}

status_t C2RKGraphicBufferMapper::importBuffer(
        const C2Handle *const c2Handle, buffer_handle_t *outHandle) {
    uint32_t bqSlot, width, height, format, stride, generation;
    uint64_t usage, bqId;

    native_handle_t *gHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

    android::_UnwrapNativeCodec2GrallocMetadata(
            c2Handle, &width, &height, &format, &usage,
            &stride, &generation, &bqId, &bqSlot);

    status_t err = GraphicBufferMapper::get().importBuffer(
            gHandle, width, height, 1, format, usage,
            stride, outHandle);
    if (err != OK) {
        c2_err("failed to import buffer %p", gHandle);
    }

    native_handle_delete(gHandle);
    return err;
}

status_t C2RKGraphicBufferMapper::freeBuffer(buffer_handle_t handle) {
    if (handle) {
        GraphicBufferMapper::get().freeBuffer(handle);
    }
    return OK;
}

int32_t C2RKGraphicBufferMapper::setDynamicHdrMeta(buffer_handle_t handle, int64_t offset) {
    int err = -1;

    if (mMapperVersion == 5) {
        c2_err("not implement");
    } else if (mMapperVersion == 4) {
        auto &mapper = getGralloc4Mapper();
        hidl_vec<uint8_t> encodedOffset;

        err = encodeRkOffsetOfVideoMetadata(offset, &encodedOffset);
        if (err != OK) {
            c2_err("Failed to encode offset_of_dynamic_hdr_metadata. err : %d", err);
            return err;
        }

        auto ret = mapper.set(const_cast<native_handle_t*>(handle),
                            RkMetadataType_OFFSET_OF_DYNAMIC_HDR_METADATA,
                            encodedOffset);
        const Error error = ret.withDefault(Error::NO_RESOURCES);
        switch (error) {
            case Error::BAD_DESCRIPTOR:
            case Error::BAD_BUFFER:
            case Error::BAD_VALUE:
            case Error::NO_RESOURCES:
                c2_err("set(%s, %lld, ...) failed with %d",
                    RkMetadataType_OFFSET_OF_DYNAMIC_HDR_METADATA.name.c_str(),
                    (long long)RkMetadataType_OFFSET_OF_DYNAMIC_HDR_METADATA.value,
                    error);
                err = -1;
                break;
            // It is not an error to attempt to set metadata that a particular gralloc implementation
            // happens to not support.
            case Error::UNSUPPORTED:
            case Error::NONE:
            default: {
                err = 0;
                break;
            }
        }
    } else {
        const gralloc_module_t* module = getGralloc2Module();
        err = module->perform(module,
                    PERFORM_SET_OFFSET_OF_DYNAMIC_HDR_METADATA, handle, offset);
    }

    if (err != 0) {
        c2_err("Failed to set dynamic hdr metadata, err %d", err);
    }
    return err;
}

int64_t C2RKGraphicBufferMapper::getDynamicHdrMeta(buffer_handle_t handle) {
    int err = 0;
    int64_t offset = -1;

    if (mMapperVersion == 5) {
        c2_err("not implement");
    } else if (mMapperVersion == 4) {
        auto &mapper = getGralloc4Mapper();

        mapper.get(const_cast<native_handle_t *>(handle),
                RkMetadataType_OFFSET_OF_DYNAMIC_HDR_METADATA,
                    [&err, &offset] (Error error, const hidl_vec<uint8_t> &metadata)
                    {
                        if (error != Error::NONE) {
                            err = android::BAD_VALUE;
                            return;
                        }
                        err = decodeRkOffsetOfVideoMetadata(metadata, &offset);
                    });
    } else {
        const gralloc_module_t* module = getGralloc2Module();

        err = module->perform(module,
                    PERFORM_GET_OFFSET_OF_DYNAMIC_HDR_METADATA, handle, &offset);
    }

    if (err != OK) {
        c2_err("Failed to get dynamic hdr metadata, err %d", err);
        return -1;
    }

    return offset;
}

int32_t C2RKGraphicBufferMapper::mapScaleMeta(
        buffer_handle_t handle, metadata_for_rkvdec_scaling_t** metadata) {
    int32_t err = -1;

    if (mMapperVersion == 5) {
        c2_err("not implement");
    } else if (mMapperVersion == 4) {
        c2_err("not implement");
    } else {
        const gralloc_module_t* module = getGralloc2Module();
        err = module->perform(module,
                    PERFORM_LOCK_RKVDEC_SCALING_METADATA, handle, metadata);
        if (err != 0) {
            c2_err("Failed to lock rkdevc_scaling_metadata, err %d", err);
        }
    }

    return err;
}

int32_t C2RKGraphicBufferMapper::unmapScaleMeta(buffer_handle_t handle) {
    int32_t err = -1;

    if (mMapperVersion == 5) {
        c2_err("not implement");
    } else if (mMapperVersion == 4) {
        c2_err("not implement");
    } else {
        const gralloc_module_t* module = getGralloc2Module();
        err = module->perform(module, PERFORM_UNLOCK_RKVDEC_SCALING_METADATA, handle);
        if (err != 0) {
            c2_err("Failed to unlock rkdevc_scaling_metadata, err %d", err);
        }
    }

    return err;
}
