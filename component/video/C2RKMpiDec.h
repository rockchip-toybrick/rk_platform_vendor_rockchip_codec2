/*
 * Copyright (C) 2020 Rockchip Electronics Co. LTD
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

#ifndef ANDROID_C2_RK_MPI_DEC_H_
#define ANDROID_C2_RK_MPI_DEC_H_

#include "C2RKComponent.h"
#include "C2RKInterface.h"
#include "C2RKDump.h"
#include "rk_mpi.h"

#include <utils/Vector.h>

namespace android {

class C2RKTunneledSession;
struct VTBuffer;
struct ColorAspects;

class C2RKMpiDec : public C2RKComponent {
public:
    class IntfImpl;
    C2RKMpiDec(
            const char *name,
            const char *mime,
            c2_node_id_t id,
            const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2RKMpiDec();

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;

    void process(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool) override;

    void postFrameReady();
    c2_status_t onFrameReady();

private:
    struct OutBuffer {
        enum Status {
            OWNED_BY_MPP = 0,
            OWNED_BY_US,
        };

        /* index to find this buffer */
        uint32_t  mBufferId;
        /* who own this buffer */
        Status    mStatus;
        /* mpp buffer */
        MppBuffer mMppBuffer;
        /* tunneled playback buffer */
        VTBuffer *mTunnelBuffer;
        /* block shared by surface */
        std::shared_ptr<C2GraphicBlock> mBlock;

        /* signal buffer available to decoder */
        void importThisBuffer();
        /* signal buffer occupied by users */
        void holdThisBuffer();
    };

    struct OutWorkEntry {
        enum Flags {
            FLAGS_EOS         = 0x1,
            FLAGS_ERROR_FRAME = 0x2,
            FLAGS_INFO_CHANGE = 0x4,
        };

        std::shared_ptr<C2GraphicBlock> outblock;
        uint64_t timestamp;
        uint32_t flags;
    };

    /*
     * low memory mode config from user
     *
     * - 0x0: default value, normal device mode
     * - 0x1: low memory case, reduce smoothFactor count from frameworks only.
     * - 0x2: low memory case, use protocol delayRef.
     */
    enum LowMemoryMode {
        MODE_NONE             = 0x0,
        MODE_REDUCE_SMOOTH    = 0x1,
        MODE_USE_PROTOCOL_REF = 0x2,
    };

    class WorkHandler : public AHandler {
    public:
        enum {
            kWhatFrameReady,
            kWhatFlushMessage,
        };

        WorkHandler() {}
        ~WorkHandler() override = default;

        void waitAllMsgFlushed();

    protected:
        void onMessageReceived(const sp<AMessage> &msg) override;
    };

    const char* mName;
    const char* mMime;
    std::shared_ptr<IntfImpl> mIntf;

    Mutex     mBufferLock;
    C2RKDump *mDump;

    Mutex     mEosLock;
    Condition mEosCondition;

    sp<ALooper>     mLooper;
    sp<WorkHandler> mHandler;

    /* MPI interface parameters */
    MppCtx          mMppCtx;
    MppApi         *mMppMpi;
    MppCodingType   mCodingType;
    MppFrameFormat  mColorFormat;
    MppBufferGroup  mFrmGrp;
    // Indicates that these buffers should be decoded but not rendered.
    Vector<uint64_t>     mDropFramesPts;
    Vector<OutBuffer*>   mOutBuffers;
    C2RKTunneledSession *mTunneledSession;

    int32_t mWidth;
    int32_t mHeight;
    int32_t mHorStride;
    int32_t mVerStride;
    int32_t mOutputDelay;
    int32_t mGrallocVersion;
    int32_t mPixelFormat;
    int32_t mScaleMode;
    int32_t mFdPerf;

    bool mStarted;
    bool mFlushed;
    bool mSignalledInputEos;
    bool mOutputEos;
    bool mSignalledError;
    bool mLowLatencyMode;
    bool mIsGBSource;
    bool mHdrMetaEnabled;
    bool mTunneled;

    /*
       1. BufferMode:  without surcace
       2. SurfaceMode: with surface
    */
    bool mBufferMode;
    bool mUseRgaBlit;

    struct AllocParams {
        int32_t needUpdate;
        int32_t width;
        int32_t height;
        int64_t usage;
        int32_t format;
    } mAllocParams;

    struct FbcConfig {
        uint32_t mode;
        // fbc decode output padding
        uint32_t paddingX;
        uint32_t paddingY;
    } mFbcCfg;

    struct ScaleThumbInfo {
        int32_t width;
        int32_t height;
        int32_t hstride;
        int32_t vstride;
        int32_t format;
    } mScaleInfo;

    std::shared_ptr<C2GraphicBlock> mOutBlock;
    std::shared_ptr<C2BlockPool>    mBlockPool;

    // Color aspects. These are ISO values and are meant to detect changes
    // in aspects to avoid converting them to C2 values for each frame.
    struct VuiColorAspects {
        uint8_t primaries;
        uint8_t transfer;
        uint8_t coeffs;
        uint8_t fullRange;

        // default color aspects
        VuiColorAspects()
            : primaries(2), transfer(2), coeffs(2), fullRange(0) { }

        bool operator==(const VuiColorAspects &o) {
            return primaries == o.primaries && transfer == o.transfer &&
                    coeffs == o.coeffs && fullRange == o.fullRange;
        }
    } mBitstreamColorAspects;

    c2_status_t setupAndStartLooper();
    void stopAndReleaseLooper();

    uint32_t getFbcOutputMode(const std::unique_ptr<C2Work> &work = nullptr);
    c2_status_t updateSurfaceConfig(const std::shared_ptr<C2BlockPool> &pool);
    c2_status_t configOutputDelay(const std::unique_ptr<C2Work> &work = nullptr);
    c2_status_t configTunneledPlayback(const std::unique_ptr<C2Work> &work);
    void finishWork(OutWorkEntry entry);

    c2_status_t initDecoder(const std::unique_ptr<C2Work> &work);
    c2_status_t setMppPerformance(bool on);
    void setDefaultCodecColorAspectsIfNeeded(ColorAspects &aspects);
    void getVuiParams(MppFrame frame);
    c2_status_t updateFbcModeIfNeeded();
    c2_status_t updateAllocParamsIfNeeded(AllocParams *params);
    c2_status_t importBufferToMpp(std::shared_ptr<C2GraphicBlock> block);
    c2_status_t ensureTunneledState();
    c2_status_t ensureDecoderState();
    c2_status_t sendpacket(uint8_t *data, size_t size, uint64_t pts, uint32_t flags);
    c2_status_t getoutframe(OutWorkEntry *entry);

    c2_status_t configFrameScaleMeta(MppFrame frame, std::shared_ptr<C2GraphicBlock> block);
    c2_status_t configFrameHdrMeta(MppFrame frame, std::shared_ptr<C2GraphicBlock> block);

    bool isDropFrame(uint64_t pts) {
        Vector<uint64_t>::iterator it;
        for (it = mDropFramesPts.begin(); it != mDropFramesPts.end(); it++) {
            if (*it == pts) {
                mDropFramesPts.erase(it);
                return true;
            }
        }
        return false;
    }

    /*
     * OutBuffer vector operations
     */
    OutBuffer* findOutBuffer(uint32_t bufferId) {
        for (int i = 0; i < mOutBuffers.size(); i++) {
            OutBuffer *buffer = mOutBuffers.editItemAt(i);
            if (buffer->mBufferId == bufferId) {
                return buffer;
            }
        }
        return nullptr;
    }

    OutBuffer* findOutBuffer(MppBuffer mppBuffer) {
        for (int i = 0; i < mOutBuffers.size(); i++) {
            OutBuffer *buffer = mOutBuffers.editItemAt(i);
            if (buffer->mMppBuffer == mppBuffer) {
                return buffer;
            }
        }
        return nullptr;
    }

    void clearOutBuffers() {
        while (!mOutBuffers.isEmpty()) {
            OutBuffer *buffer = mOutBuffers.editItemAt(0);
            if (buffer != nullptr) {
                if (buffer->mStatus != OutBuffer::OWNED_BY_MPP) {
                    buffer->importThisBuffer();
                }
                delete buffer;
            }
            mOutBuffers.removeAt(0);
        }
    }

    int getOutBufferCountOwnByMpi() {
        int count = 0;
        for (int i = 0; i < mOutBuffers.size(); i++) {
            OutBuffer *buffer = mOutBuffers.editItemAt(i);
            if (buffer->mStatus == OutBuffer::OWNED_BY_MPP) {
                count++;
            }
        }
        return count;
    }

    C2_DO_NOT_COPY(C2RKMpiDec);
};

C2ComponentFactory* CreateRKMpiDecFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_SOFT_MPI_DEC_H_
