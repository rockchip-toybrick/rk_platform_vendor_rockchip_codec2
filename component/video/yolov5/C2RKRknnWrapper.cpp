/*
 * Copyright (C) 2025 Rockchip Electronics Co. LTD
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
#define ROCKCHIP_LOG_TAG    "C2RKRknnWrapper"

#include <dlfcn.h>
#include <string.h>

#include "C2RKLog.h"
#include "C2RKRknnWrapper.h"

namespace android {

C2RKRknnWrapper::C2RKRknnWrapper() {
    mReady = false;
    mLibFd = nullptr;
}

C2RKRknnWrapper::~C2RKRknnWrapper() {
    if (mLibFd != nullptr) {
        dlclose(mLibFd);
        mLibFd = nullptr;
    }
}

bool C2RKRknnWrapper::initCheck() {
    if (mReady)
        return true;

    mLibFd = dlopen("librknnrt.so", RTLD_LAZY);
    if (mLibFd == nullptr) {
        c2_err("failed to open librkvt, %s", dlerror());
        return false;
    }

    mInitFunc           = (rknnInitFunc *)dlsym(mLibFd, "rknn_init");
    mDestroyFunc        = (rknnDestroyFunc *)dlsym(mLibFd, "rknn_destroy");
    mQueryFunc          = (rknnQueryFunc *)dlsym(mLibFd, "rknn_query");
    mSetInputsFunc      = (rknnSetInputsFunc *)dlsym(mLibFd, "rknn_inputs_set");
    mGetOutputsFunc     = (rknnGetOutputsFunc *)dlsym(mLibFd, "rknn_outputs_get");
    mRunFunc            = (rknnRunFunc *)dlsym(mLibFd, "rknn_run");
    mReleaseOutputsFunc = (rknnReleaseOutputsFunc *)dlsym(mLibFd, "rknn_outputs_release");
    mSetCoreMaskFunc    = (rknnSetCoreMaskFunc *)dlsym(mLibFd, "rknn_set_core_mask");
    mCreateMemFunc      = (rknnCreateMemFunc *)dlsym(mLibFd, "rknn_create_mem");
    mDestroyMemFunc     = (rknnDestroyMemFunc *)dlsym(mLibFd, "rknn_destroy_mem");

    mMatmulCreateShapeFunc = (rknnMatmulCreateShapeFunc *)dlsym(mLibFd, "rknn_matmul_create_dynamic_shape");
    mMatmulDestroyFunc     = (rknnMatmulDestroyFunc *)dlsym(mLibFd, "rknn_matmul_destroy");
    mMatmulSetShapeFunc    = (rknnMatmulSetShapeFunc *)dlsym(mLibFd, "rknn_matmul_set_dynamic_shape");
    mMatmulSetIOMemFunc    = (rknnMatmulSetIOMemFunc *)dlsym(mLibFd, "rknn_matmul_set_io_mem");
    mMatmulRunFunc         = (rknnMatmulRunFunc *)dlsym(mLibFd, "rknn_matmul_run");

    if (!mInitFunc || !mDestroyFunc || !mQueryFunc || !mSetInputsFunc ||
        !mGetOutputsFunc || !mRunFunc || !mReleaseOutputsFunc ||
        !mSetCoreMaskFunc || !mCreateMemFunc || !mDestroyMemFunc) {
        c2_err("could not find rknn api symbol, %s", dlerror());
        dlclose(mLibFd);
        return false;
    }

    if (!mMatmulCreateShapeFunc || !mMatmulDestroyFunc ||
        !mMatmulSetShapeFunc || !mMatmulRunFunc || !mMatmulSetIOMemFunc) {
        c2_err("could not find rknn api matmul symbol, %s", dlerror());
        dlclose(mLibFd);
        return false;
    }

    mReady = true;
    return true;
}

int C2RKRknnWrapper::rknnInit(
        rknn_context *context, void *model,
        uint32_t size, uint32_t flag, rknn_init_extend *extend) {
    return mInitFunc(context, model, size, flag, extend);
}

int C2RKRknnWrapper::rknnDestory(rknn_context context) {
    return mDestroyFunc(context);
}

int C2RKRknnWrapper::rknnQuery(
        rknn_context context, rknn_query_cmd cmd, void* info, uint32_t size) {
    return mQueryFunc(context, cmd, info, size);
}

int C2RKRknnWrapper::rknnSetInputs(
        rknn_context context, uint32_t nInputs, rknn_input inputs[]) {
    return mSetInputsFunc(context, nInputs, inputs);
}

int C2RKRknnWrapper::rknnGetOutputs(
        rknn_context context, uint32_t nOutputs,
        rknn_output outputs[], rknn_output_extend* extend) {
    return mGetOutputsFunc(context, nOutputs, outputs, extend);
}

int C2RKRknnWrapper::rknnRun(rknn_context context, rknn_run_extend* extend) {
    return mRunFunc(context, extend);
}

int C2RKRknnWrapper::rknnReleaseOutputs(
        rknn_context context, uint32_t n_ouputs, rknn_output outputs[]) {
    return mReleaseOutputsFunc(context, n_ouputs, outputs);
}

int C2RKRknnWrapper::rknnSetCoreMask(
        rknn_context context, rknn_core_mask coreMask) {
    return mSetCoreMaskFunc(context, coreMask);
}

rknn_tensor_mem* C2RKRknnWrapper::rknnCreateMem(
        rknn_context context, uint32_t size) {
    return mCreateMemFunc(context, size);
}

int C2RKRknnWrapper::rknnDestroyMem(rknn_context context, rknn_tensor_mem* mem) {
    return mDestroyMemFunc(context, mem);
}

int C2RKRknnWrapper::rknnMatmulCreateShape(
        rknn_matmul_ctx* ctx, rknn_matmul_info* info,
        int shapeNum, rknn_matmul_shape shapes[],
        rknn_matmul_io_attr ioAttrs[]) {
    return mMatmulCreateShapeFunc(ctx, info, shapeNum, shapes, ioAttrs);
}

int C2RKRknnWrapper::rknnMatmulDestroy(rknn_matmul_ctx ctx) {
    return mMatmulDestroyFunc(ctx);
}

int C2RKRknnWrapper::rknnMatmulSetShape(
        rknn_matmul_ctx ctx, rknn_matmul_shape* shape) {
    return mMatmulSetShapeFunc(ctx, shape);
}

int C2RKRknnWrapper::rknnMatmulSetIOMem(
        rknn_matmul_ctx ctx, rknn_tensor_mem* mem, rknn_matmul_tensor_attr* attr) {
    return mMatmulSetIOMemFunc(ctx, mem, attr);
}

int C2RKRknnWrapper::rknnMatmulRun(rknn_matmul_ctx ctx) {
    return mMatmulRunFunc(ctx);
}

}
