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


#define C2_TUNNELED_RESEVED_SIZE    3


namespace android {

typedef int32_t OpenTunnelFunc();
typedef int32_t CloseTunnelFunc(int32_t fd);
typedef int32_t AllocTunnelIdFunc(int32_t fd, int32_t *id);
typedef int32_t FreeTunnelIdFunc(int32_t fd, int32_t id);
typedef int32_t ResetTunnelFunc(int32_t fd, int32_t id);
typedef int32_t ConnectTunnelFunc(int32_t fd, int32_t id, int32_t role);
typedef int32_t DisconnectTunnelFunc(int32_t fd, int32_t id, int32_t role);

typedef int32_t DequeueBufferFunc(int32_t fd, int32_t id, int32_t timeout, VTBuffer_t **buffer);
typedef int32_t QueueBufferFunc(int32_t fd, int32_t id, VTBuffer_t *buffer, int64_t presentTime);
typedef int32_t CancelBufferFunc(int32_t fd, int32_t id, VTBuffer_t *buffer);

typedef int32_t FreeVtBufferFunc(VTBuffer_t **buffer);
typedef VTBuffer_t* MallocVtBufferFunc();

class VTunnelImpl {
public:
    VTunnelImpl() {
        mDevFd = 0;
        mTunnelId = 0;
    }

    ~VTunnelImpl() {
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

    void reset() {
        if (mDevFd > 0) {
             mResetFunc(mDevFd, mTunnelId);
        }
    }

    bool dequeueBuffer(VTBuffer_t **buffer, int timeout) {
        int32_t err = mDequeueBufferFunc(mDevFd, mTunnelId, timeout, buffer);
        return (err == 0) ? true : false;
    }

    bool queueBuffer(VTBuffer_t *buffer, int64_t presentTime) {
        int32_t err = mQueueBufferFunc(mDevFd, mTunnelId, buffer, presentTime);
        return (err == 0) ? true : false;
    }

    bool cancelBuffer(VTBuffer_t *buffer) {
        int32_t err = mCancelBufferFunc(mDevFd, mTunnelId, buffer);
        return (err == 0) ? true : false;
    }

    void allocVTBuffer(VTBuffer_t **buffer) {
        (*buffer) = mMallocVTBufferFunc();
    }

    void freeVTBuffer(VTBuffer_t *buffer) {
        mFreeVTBufferFunc(&buffer);
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
    mImpl = new VTunnelImpl();

    mTunnelId = 0;
    mNeedReservedCnt = C2_TUNNELED_RESEVED_SIZE;
}

C2RKTunneledSession::~C2RKTunneledSession() {
    if (mImpl) {
        mImpl->closeConnection();
        delete mImpl;
        mImpl = nullptr;
    }
}

bool C2RKTunneledSession::congigure(TunnelParams_t params) {
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
    this->reset();
    mImpl->closeConnection();
    return true;
}

bool C2RKTunneledSession::reset() {
    c2_trace_func_enter();
    mImpl->reset();

    while (!mVTBuffers.isEmpty()) {
        VTBuffer_t *buffer = mVTBuffers.editItemAt(0);
        if (buffer != nullptr) {
            freeVTBuffer(buffer);
        }
        mVTBuffers.removeAt(0);
    }

    mNeedReservedCnt = C2_TUNNELED_RESEVED_SIZE;
    return true;
}

bool C2RKTunneledSession::needReservedBuffer() {
    return (mNeedReservedCnt > 0);
}

bool C2RKTunneledSession::dequeueBuffer(VTBuffer_t **buffer, int32_t timeout) {
    mImpl->dequeueBuffer(buffer, timeout);
    if (*buffer) {
        c2_trace("dequeue buffer %p", (*buffer));
        (*buffer)->state = RKVT_STATE_DEQUEUED;
        return true;
    }
    return false;
}

bool C2RKTunneledSession::renderBuffer(VTBuffer_t *buffer, int64_t presentTime) {
    if (mImpl->queueBuffer(buffer, presentTime)) {
        c2_trace("render buffer %p", buffer);
        buffer->state = RKVT_STATE_QUEUED;
        return true;
    }
    return false;
}

bool C2RKTunneledSession::cancelBuffer(VTBuffer_t *buffer) {
    if (mImpl->cancelBuffer(buffer)) {
        c2_trace("reseved buffer %p", buffer);
        mNeedReservedCnt -= 1;
        buffer->state = RKVT_STATE_QUEUED;
        return true;
    }
    return false;
}

void C2RKTunneledSession::newVTBuffer(VTBuffer_t **buffer) {
    mImpl->allocVTBuffer(buffer);
    if (*buffer) {
        c2_trace("alloc buffer %p", (*buffer));
        (*buffer)->state = (mNeedReservedCnt > 0) ? RKVT_STATE_RESERVED : RTVT_STATE_NEW;
        (*buffer)->crop = mSideband.crop;

        mVTBuffers.push((*buffer));
    }
}

void C2RKTunneledSession::freeVTBuffer(VTBuffer_t *buffer) {
    c2_trace("free buffer %p", buffer);
    mImpl->freeVTBuffer(buffer);
}

void* C2RKTunneledSession::getTunnelSideband() {
    return &mSideband;
}

int32_t C2RKTunneledSession::getNeedDequeueCnt() {
    int32_t count = 0;

    for (int32_t i = 0; i < mVTBuffers.size(); i++) {
        VTBuffer_t *buffer = mVTBuffers.editItemAt(i);
        if (buffer->state == RKVT_STATE_QUEUED) {
            count++;
        }
    }

    return (count - C2_TUNNELED_RESEVED_SIZE);
}

}
