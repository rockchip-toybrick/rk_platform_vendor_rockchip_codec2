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

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKTunneledSession"

#include <dlfcn.h>
#include <string.h>

#include "C2RKLog.h"
#include "C2RKTunneledSession.h"


#define C2_TUNNELED_RESERVED_COUNT    3


namespace android {

typedef int OpenTunnelFunc();
typedef int CloseTunnelFunc(int32_t fd);
typedef int AllocTunnelIdFunc(int32_t fd, int32_t *id);
typedef int FreeTunnelIdFunc(int32_t fd, int32_t id);
typedef int ResetTunnelFunc(int32_t fd, int32_t id);
typedef int ConnectTunnelFunc(int32_t fd, int32_t id, int32_t role);
typedef int DisconnectTunnelFunc(int32_t fd, int32_t id, int32_t role);

typedef int DequeueBufferFunc(int32_t fd, int32_t id, int32_t timeout, VTBuffer_t **buffer);
typedef int QueueBufferFunc(int32_t fd, int32_t id, VTBuffer_t *buffer, int64_t presentTime);
typedef int CancelBufferFunc(int32_t fd, int32_t id, VTBuffer_t *buffer);

typedef int FreeVtBufferFunc(VTBuffer_t **buffer);
typedef VTBuffer_t* MallocVtBufferFunc();

class C2RKTunneledSession::Impl {
public:
    Impl() {
        mDevFd = 0;
        mTunnelId = 0;
    }

    ~Impl() {
        closeConnection();
    }

    int openConnection() {
        mLibFd = dlopen("librkvt.so", RTLD_LAZY);
        if (mLibFd == nullptr) {
            c2_err("failed to open librkvt, %s", dlerror());
            return 0;
        }

        mCreateFunc  = (OpenTunnelFunc *)dlsym(mLibFd, "rk_vt_open");
        mDestroyFunc = (CloseTunnelFunc *)dlsym(mLibFd, "rk_vt_close");
        mAllocIdFunc = (AllocTunnelIdFunc *)dlsym(mLibFd, "rk_vt_alloc_id");
        mFreeIdFunc  = (FreeTunnelIdFunc *)dlsym(mLibFd, "rk_vt_free_id");
        mResetFunc   = (ResetTunnelFunc *)dlsym(mLibFd, "rk_vt_reset");
        mConnectFunc = (ConnectTunnelFunc *)dlsym(mLibFd, "rk_vt_connect");
        mDisconnectFunc = (DisconnectTunnelFunc *)dlsym(mLibFd, "rk_vt_disconnect");

        mDequeueBufferFunc  = (DequeueBufferFunc *)dlsym(mLibFd, "rk_vt_dequeue_buffer");
        mQueueBufferFunc    = (QueueBufferFunc *)dlsym(mLibFd, "rk_vt_queue_buffer");
        mCancelBufferFunc   = (CancelBufferFunc *)dlsym(mLibFd, "rk_vt_cancel_buffer");
        mMallocVTBufferFunc = (MallocVtBufferFunc *)dlsym(mLibFd, "rk_vt_buffer_malloc");
        mFreeVTBufferFunc   = (FreeVtBufferFunc *)dlsym(mLibFd, "rk_vt_buffer_free");

        if (!mCreateFunc || !mDestroyFunc || !mAllocIdFunc || !mFreeIdFunc ||
            !mResetFunc || !mConnectFunc || !mDisconnectFunc || !mDequeueBufferFunc ||
            !mQueueBufferFunc || !mCancelBufferFunc ||
            !mMallocVTBufferFunc || !mFreeVTBufferFunc) {
            c2_err("could not find symbol, %s", dlerror());
            mDevFd = 0;
            mTunnelId = 0;
            goto error;
        }

        if (mDevFd <= 0) {
            mDevFd = mCreateFunc();
            if (mDevFd <= 0) {
                c2_err("open error");
                goto error;
            }
            if (mAllocIdFunc(mDevFd, &mTunnelId)) {
                c2_err("alloc error");
                goto error;
            }
            if (mConnectFunc(mDevFd, mTunnelId, RKVT_ROLE_PRODUCER)) {
                c2_err("connect error");
                goto error;
            }
        }

        c2_trace("open tunnel sesion: devFd %d tunnleId %d", mDevFd, mTunnelId);
        return mTunnelId;

    error:
        closeConnection();
        return 0;
    }

    void closeConnection() {
        if (mDevFd > 0) {
            if (mTunnelId > 0) {
                mFreeIdFunc(mDevFd, mTunnelId);
                mDisconnectFunc(mDevFd, mTunnelId, RKVT_ROLE_PRODUCER);
            }
            mDestroyFunc(mDevFd);
        }
        if (mLibFd) {
            dlclose(mLibFd);
        }

        mDevFd = 0;
        mLibFd = 0;
        mTunnelId = 0;
    }

    bool reset() {
        return (mResetFunc(mDevFd, mTunnelId) == 0);
    }

    bool dequeueBuffer(VTBuffer_t **buffer, int timeout) {
        return (mDequeueBufferFunc(mDevFd, mTunnelId, timeout, buffer) == 0);
    }

    bool queueBuffer(VTBuffer_t *buffer, int64_t presentTime) {
        return (mQueueBufferFunc(mDevFd, mTunnelId, buffer, presentTime) == 0);
    }

    bool cancelBuffer(VTBuffer_t *buffer) {
        return (mCancelBufferFunc(mDevFd, mTunnelId, buffer) == 0);
    }

    bool allocBuffer(VTBuffer_t **buffer) {
        (*buffer) = mMallocVTBufferFunc();
        return ((*buffer) != nullptr);
    }

    bool freeBuffer(VTBuffer_t *buffer) {
        return (mFreeVTBufferFunc(&buffer) == 0);
    }

private:
    OpenTunnelFunc       *mCreateFunc;
    CloseTunnelFunc      *mDestroyFunc;
    AllocTunnelIdFunc    *mAllocIdFunc;
    FreeTunnelIdFunc     *mFreeIdFunc;
    ResetTunnelFunc      *mResetFunc;
    ConnectTunnelFunc    *mConnectFunc;
    DisconnectTunnelFunc *mDisconnectFunc;

    DequeueBufferFunc    *mDequeueBufferFunc;
    QueueBufferFunc      *mQueueBufferFunc;
    CancelBufferFunc     *mCancelBufferFunc;
    MallocVtBufferFunc   *mMallocVTBufferFunc;
    FreeVtBufferFunc     *mFreeVTBufferFunc;

private:
    void    *mLibFd;
    int32_t  mDevFd;
    int32_t  mTunnelId;
};

C2RKTunneledSession::C2RKTunneledSession() {
    mImpl = std::make_shared<Impl>();

    mTunnelId = 0;
    mNeedDequeueCnt = 0;
    mNeedReservedCnt = C2_TUNNELED_RESERVED_COUNT;
}

C2RKTunneledSession::~C2RKTunneledSession() {
    this->disconnect();
}

bool C2RKTunneledSession::configure(TunnelParams_t params) {
    if (mTunnelId <= 0) {
        mTunnelId = mImpl->openConnection();
        if (mTunnelId <= 0) {
            return false;
        }
    }

    static uint64_t gSessionId = 0;

    memset(&mSideband, 0, sizeof(mSideband));

    ++gSessionId;
    mSideband.version = sizeof(SidebandHandler_t);
    mSideband.tunnelId = mTunnelId;
    mSideband.sessionId = gSessionId;
    mSideband.crop.left = params.left;
    mSideband.crop.top = params.top;
    mSideband.crop.right = params.right;
    mSideband.crop.bottom = params.bottom;
    mSideband.width = params.width;
    mSideband.height = params.height;
    mSideband.format = params.format;
    mSideband.transform = 0;
    mSideband.usage = params.usage;
    mSideband.dataSpace = params.dataSpace;
    mSideband.compressMode = params.compressMode;

    c2_info("sideband config: w %d h %d crop[%d %d %d %d] fmt 0x%x compress %d id %d",
            mSideband.width, mSideband.height, mSideband.crop.left,
            mSideband.crop.top, mSideband.crop.right, mSideband.crop.bottom,
            mSideband.format, mSideband.compressMode, mSideband.tunnelId);

    return true;
}

bool C2RKTunneledSession::disconnect() {
    c2_trace_func_enter();
    this->reset();
    mImpl->closeConnection();
    return true;
}

bool C2RKTunneledSession::reset() {
    c2_trace_func_enter();
    mImpl->reset();

    auto it = mBuffers.begin();
    if (it != mBuffers.end()) {
        if (!freeBuffer(it->first)) {
            // TODO buffer->handle release ?
            c2_err("reset: failed to free buffer %d", it->first);
        }
        it = mBuffers.erase(it);
    }

    mNeedDequeueCnt = 0;
    mNeedReservedCnt = C2_TUNNELED_RESERVED_COUNT;

    return true;
}

bool C2RKTunneledSession::dequeueBuffer(int32_t *bufferId) {
    VTBuffer *buffer = nullptr;
    mImpl->dequeueBuffer(&buffer, 0);
    if (buffer) {
        c2_trace("dequeue buffer %d", buffer->uniqueId);
        buffer->state = RKVT_STATE_DEQUEUED;
        (*bufferId) = buffer->uniqueId;
        mNeedDequeueCnt -= 1;
        return true;
    }
    return false;
}

bool C2RKTunneledSession::renderBuffer(int32_t bufferId, int64_t presentTime) {
    VTBuffer *buffer = findBuffer(bufferId);
    if (buffer && mImpl->queueBuffer(buffer, presentTime)) {
        c2_trace("render buffer %d", bufferId);
        buffer->state = RKVT_STATE_QUEUED;
        mNeedDequeueCnt += 1;
        return true;
    }
    return false;
}

bool C2RKTunneledSession::cancelBuffer(int32_t bufferId) {
    VTBuffer *buffer = findBuffer(bufferId);
    if (buffer && mImpl->cancelBuffer(buffer)) {
        c2_trace("reseved buffer %d", bufferId);
        mNeedReservedCnt -= 1;
        buffer->state = RKVT_STATE_RESERVED;
        return true;
    }
    return false;
}

bool C2RKTunneledSession::isReservedBuffer(int32_t bufferId) {
    VTBuffer *buffer = findBuffer(bufferId);
    if (buffer) {
        return (buffer->state == RKVT_STATE_RESERVED);
    }
    return false;
}

bool C2RKTunneledSession::newBuffer(native_handle_t *handle, int32_t bufferId) {
    VTBuffer *buffer = nullptr;
    mImpl->allocBuffer(&buffer);
    if (buffer) {
        c2_trace("alloc buffer %d", bufferId);
        buffer->handle   = handle;
        buffer->uniqueId = bufferId;
        buffer->crop     = mSideband.crop;
        buffer->state    = RTVT_STATE_NEW;

        mBuffers.insert(std::make_pair(bufferId, buffer));

        if (mNeedReservedCnt > 0) {
            cancelBuffer(bufferId);
        }
    }
    return (buffer != nullptr);
}

bool C2RKTunneledSession::freeBuffer(int32_t bufferId) {
    VTBuffer *buffer = findBuffer(bufferId);
    if (buffer && mImpl->freeBuffer(buffer)) {
        c2_trace("free buffer %d", bufferId);
        return true;
    }
    return false;
}

VTBuffer* C2RKTunneledSession::findBuffer(int32_t bufferId) {
    auto it = mBuffers.find(bufferId);
    if (it != mBuffers.end()) {
        return it->second;
    }
    return nullptr;
}

void* C2RKTunneledSession::getTunnelSideband() {
    return &mSideband;
}

int32_t C2RKTunneledSession::getNeedDequeueCnt() {
    return mNeedDequeueCnt;
}

int32_t C2RKTunneledSession::getSmoothnessFactor() {
    return C2_TUNNELED_RESERVED_COUNT;
}

}
