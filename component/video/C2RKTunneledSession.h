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

#ifndef ANDROID_C2_RK_TUNNEL_PLAY_H__
#define ANDROID_C2_RK_TUNNEL_PLAY_H__

#include <stdint.h>
#include <cutils/native_handle.h>
#include <map>

namespace android {

typedef enum VTRole {
    RKVT_ROLE_PRODUCER,
    RKVT_ROLE_CONSUMER,
    RKVT_ROLE_INVALID,
} VTRole_t;

typedef enum VTBufferMode {
    RKVT_BUFFER_INTERNAL,
    RKVT_BUFFER_EXTERNAL,
    RKVT_BUFFER_MODE_BUTT,
} VTBufferMode_t;

typedef struct VTRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} VTRect_t;

typedef enum VTBufferState {
    RTVT_STATE_NEW,
    RKVT_STATE_RESERVED,
    RKVT_STATE_DEQUEUED,
    RKVT_STATE_QUEUED,
} VTBufferState_t;

typedef struct VTBuffer {
    int32_t          magic;
    int32_t          structSize;
    native_handle_t *handle;
    int              fenceFd;
    uint64_t         bufferId;
    VTRect           crop;
    VTRect           disRect;
    int64_t          privateData;
    VTBufferMode     bufferMode;
    int32_t          uniqueId;
    int32_t          state;
    int32_t          reserve[4];
} VTBuffer_t;

typedef struct SidebandHandler {
    int32_t     version;
    int32_t     tunnelId;
    uint64_t    sessionId;
    VTRect      crop;
    int32_t     width;
    int32_t     height;
    int32_t     hstride;
    int32_t     vstride;
    int32_t     byteStride;
    int32_t     format;
    int32_t     transform;
    int32_t     size;
    int32_t     modifier;
    uint64_t    usage;
    uint64_t    dataSpace;
    uint64_t    fps;
    int32_t     compressMode;
    int32_t     reserved[13];
} SidebandHandler_t;

typedef struct TunnelParams {
    int32_t     left;
    int32_t     top;
    int32_t     right;
    int32_t     bottom;
    int32_t     width;
    int32_t     height;
    int32_t     format;
    uint64_t    usage;
    uint64_t    dataSpace;
    int32_t     compressMode;
} TunnelParams_t;

class C2RKTunneledSession {
public:
    C2RKTunneledSession();
    virtual ~C2RKTunneledSession();

    bool configure(TunnelParams_t params);
    bool disconnect();
    bool reset();

    bool dequeueBuffer(int32_t *bufferId);
    bool renderBuffer(int32_t bufferId, int64_t presentTime = 0);
    bool cancelBuffer(int32_t bufferId);

    bool isReservedBuffer(int32_t bufferId);
    bool newBuffer(native_handle_t *handle, int32_t bufferId);
    bool freeBuffer(int32_t bufferId);

    void*   getTunnelSideband();
    int32_t getNeedDequeueCnt();
    int32_t getSmoothnessFactor();

private:
    int32_t mTunnelId;
    int32_t mNeedDequeueCnt;
    int32_t mNeedReservedCnt;

    SidebandHandler mSideband;

    class Impl;
    std::shared_ptr<Impl> mImpl;

    std::map<int32_t, VTBuffer_t*> mBuffers;

    VTBuffer* findBuffer(int32_t bufferId);
};

}
#endif  // ANDROID_C2_RK_TUNNEL_PLAY_H__