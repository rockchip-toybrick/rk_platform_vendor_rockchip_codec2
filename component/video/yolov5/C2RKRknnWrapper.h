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

#include "rknn_api.h"
#include "rknn_matmul_api.h"
#include "Float16.h"

#ifndef ANDROID_RK_C2_RKNN_WRAPPER_H_
#define ANDROID_RK_C2_RKNN_WRAPPER_H_

namespace android {

// rknn api

typedef int rknnInitFunc(
        rknn_context *context, void *model,
        uint32_t size, uint32_t flag, rknn_init_extend *extend);

typedef int rknnDestroyFunc(rknn_context context);

typedef int rknnQueryFunc(
        rknn_context context, rknn_query_cmd cmd, void* info, uint32_t size);

typedef int rknnSetInputsFunc(
        rknn_context context, uint32_t nInputs, rknn_input inputs[]);

typedef int rknnGetOutputsFunc(
        rknn_context context, uint32_t nOutputs,
        rknn_output outputs[], rknn_output_extend* extend);

typedef int rknnRunFunc(
        rknn_context context, rknn_run_extend* extend);

typedef int rknnReleaseOutputsFunc(
        rknn_context context, uint32_t nOuputs, rknn_output outputs[]);

typedef int rknnSetCoreMaskFunc(rknn_context context, rknn_core_mask coreMask);

typedef rknn_tensor_mem* rknnCreateMemFunc(rknn_context context, uint32_t size);

typedef int rknnDestroyMemFunc(rknn_context context, rknn_tensor_mem *mem);

// rknn matmul api

typedef int rknnMatmulCreateShapeFunc(
        rknn_matmul_ctx* ctx, rknn_matmul_info* info, int shapeNum,
        rknn_matmul_shape shapes[], rknn_matmul_io_attr ioAttrs[]);

typedef int rknnMatmulDestroyFunc(rknn_matmul_ctx ctx);

typedef int rknnMatmulSetShapeFunc(rknn_matmul_ctx ctx, rknn_matmul_shape* shape);

typedef int rknnMatmulSetIOMemFunc(
        rknn_matmul_ctx ctx, rknn_tensor_mem* mem, rknn_matmul_tensor_attr* attr);

typedef int rknnMatmulRunFunc(rknn_matmul_ctx ctx);


class C2RKRknnWrapper {
public:
    static C2RKRknnWrapper* get() {
        static C2RKRknnWrapper _gInstance;
        return &_gInstance;
    }

    bool initCheck();

    /* rknn api wrapper functions */
    int rknnInit(rknn_context *context, void *model,
                 uint32_t size, uint32_t flag, rknn_init_extend *extend);
    int rknnDestory(rknn_context context);
    int rknnQuery(rknn_context context, rknn_query_cmd cmd, void* info, uint32_t size);
    int rknnSetInputs(rknn_context context, uint32_t nInputs, rknn_input inputs[]);
    int rknnGetOutputs(rknn_context context, uint32_t nOutputs,
                       rknn_output outputs[], rknn_output_extend* extend);
    int rknnRun(rknn_context context, rknn_run_extend* extend);
    int rknnReleaseOutputs(rknn_context context, uint32_t n_ouputs, rknn_output outputs[]);
    int rknnSetCoreMask(rknn_context context, rknn_core_mask coreMask);
    rknn_tensor_mem* rknnCreateMem(rknn_context context, uint32_t size);
    int rknnDestroyMem(rknn_context context, rknn_tensor_mem* mem);

    /* rknn api matmul wrapper functions */
    int rknnMatmulCreateShape(rknn_matmul_ctx* ctx, rknn_matmul_info* info,
                              int shapeNum, rknn_matmul_shape shapes[],
                              rknn_matmul_io_attr ioAttrs[]);
    int rknnMatmulDestroy(rknn_matmul_ctx ctx);
    int rknnMatmulSetShape(rknn_matmul_ctx ctx, rknn_matmul_shape* shape);
    int rknnMatmulSetIOMem(rknn_matmul_ctx ctx, rknn_tensor_mem* mem,
                           rknn_matmul_tensor_attr* attr);
    int rknnMatmulRun(rknn_matmul_ctx ctx);

private:
    C2RKRknnWrapper();
    virtual ~C2RKRknnWrapper();

private:
    bool  mReady;
    void *mLibFd;

    rknnInitFunc            *mInitFunc;
    rknnDestroyFunc         *mDestroyFunc;
    rknnQueryFunc           *mQueryFunc;
    rknnSetInputsFunc       *mSetInputsFunc;
    rknnGetOutputsFunc      *mGetOutputsFunc;
    rknnRunFunc             *mRunFunc;
    rknnReleaseOutputsFunc  *mReleaseOutputsFunc;
    rknnSetCoreMaskFunc     *mSetCoreMaskFunc;
    rknnCreateMemFunc       *mCreateMemFunc;
    rknnDestroyMemFunc      *mDestroyMemFunc;

    rknnMatmulCreateShapeFunc *mMatmulCreateShapeFunc;
    rknnMatmulDestroyFunc     *mMatmulDestroyFunc;
    rknnMatmulSetShapeFunc    *mMatmulSetShapeFunc;
    rknnMatmulSetIOMemFunc    *mMatmulSetIOMemFunc;
    rknnMatmulRunFunc         *mMatmulRunFunc;
};

}

#endif // ANDROID_RK_C2_RKNN_WRAPPER_H_

