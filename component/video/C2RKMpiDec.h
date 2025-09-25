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
#include "rk_mpi.h"

#include <map>
#include <utils/Vector.h>

namespace android {

struct ColorAspects;
class C2RKDumpStateService;
class C2RKTunneledSession;

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
    c2_status_t drainWork(const std::unique_ptr<C2Work> &work = nullptr);

private:
    class WorkHandler : public AHandler {
    public:
        enum {
            kWhatFrameReady,
            kWhatFlushMessage,
        };

        WorkHandler() : mRunning(true) {}
        ~WorkHandler() override = default;

        void flushAllMessages();
        void stop();

    protected:
        void onMessageReceived(const sp<AMessage> &msg) override;

    private:
        bool mRunning;
    };

    // Output buffer structure
    struct OutBuffer {
        int32_t   mBufferId;
        bool      mOwnedByDecoder;
        MppBuffer mMppBuffer;
        std::shared_ptr<C2GraphicBlock> mBlock;

        OutBuffer(
                int32_t bufferId,
                MppBuffer mppBuffer,
                const std::shared_ptr<C2GraphicBlock> &block) :
                mBufferId(bufferId), mOwnedByDecoder(false),
                mMppBuffer(mppBuffer), mBlock(block) {}

        bool ownedByDecoder();

        void updateBlock(std::shared_ptr<C2GraphicBlock> block) { mBlock = block; }
        std::shared_ptr<C2GraphicBlock> getBlock() { return mBlock; }

        void submitToDecoder();
        void setInusedByClient();
    };

    struct WorkEntry {
        enum Flags {
            FLAGS_EOS         = 0x1,
            FLAGS_INFO_CHANGE = 0x2,
            FLAGS_DROP_FRAME  = 0x4,
        };

        int64_t frameIndex;
        int64_t timestamp;
        int32_t flags;
        std::shared_ptr<C2GraphicBlock> block;
    };

    /*
     * low memory mode config from user
     *
     * 0x0: default value, normal device mode
     * 0x1: low memory case, reduce smoothFactor count from frameworks only.
     * 0x2: low memory case, use protocol delayRef.
     */
    enum LowMemoryMode {
        MODE_NONE             = 0x0,
        MODE_REDUCE_SMOOTH    = 0x1,
        MODE_USE_PROTOCOL_REF = 0x2,
    };

    const char* mName;
    const char* mMime;
    std::shared_ptr<IntfImpl> mIntf;
    std::shared_ptr<C2RKTunneledSession> mTunneledSession;

    C2RKDumpStateService *mDumpService;

    Mutex            mBufferLock;
    sp<ALooper>      mLooper;
    sp<WorkHandler>  mHandler;

    /* MPI interface parameters */
    MppCtx           mMppCtx;
    MppApi          *mMppMpi;
    MppCodingType    mCodingType;
    MppFrameFormat   mColorFormat;
    MppBufferGroup   mBufferGroup;
    // Indicates that these buffers should be decoded but not rendered.
    Vector<uint64_t> mDropFrames;
    std::map<int32_t, std::shared_ptr<OutBuffer>> mBuffers;

    int32_t mWidth;
    int32_t mHeight;
    int32_t mHorStride;
    int32_t mVerStride;
    // fbc output has padding inside, set crop before display
    int32_t mLeftCorner;
    int32_t mTopCorner;
    int32_t mNumOutputSlots;
    int32_t mSlotsToReduce;
    int32_t mPixelFormat;
    int32_t mScaleMode;
    int32_t mFdPerf;

    bool mStarted;
    bool mFlushed;
    bool mInputEOS;
    bool mOutputEOS;
    bool mSignalledError;
    bool mGraphicSourceMode;
    bool mHdrMetaEnabled;
    bool mTunneled;
    bool mBufferMode;
    bool mUseRgaBlit;
    bool mStandardWorkFlow;

    std::shared_ptr<C2GraphicBlock> mOutBlock;
    std::shared_ptr<C2BlockPool>    mBlockPool;

    struct AllocParams {
        int32_t width;
        int32_t height;
        int64_t usage;
        int32_t format;
    } mAllocParams;

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

    c2_status_t drainEOS(const std::unique_ptr<C2Work> &work);

    int32_t getFbcOutputMode(const std::unique_ptr<C2Work> &work = nullptr);
    c2_status_t getSurfaceFeatures(const std::shared_ptr<C2BlockPool> &pool);
    c2_status_t configOutputDelay(const std::unique_ptr<C2Work> &work = nullptr);
    c2_status_t configTunneledPlayback(const std::unique_ptr<C2Work> &work);
    void fillEmptyWork(const std::unique_ptr<C2Work> &work);
    void finishWork(const std::unique_ptr<C2Work> &work, WorkEntry entry);

    c2_status_t initDecoder(const std::unique_ptr<C2Work> &work);
    c2_status_t updateDecoderArgs(const std::shared_ptr<C2BlockPool> &pool);
    c2_status_t updateAllocParams();
    c2_status_t updateMppFrameInfo(int32_t fbcMode);
    void setMppPerformance(bool on);
    void setDefaultCodecColorAspectsIfNeeded(ColorAspects &aspects);
    void getVuiParams(MppFrame frame);
    c2_status_t updateFbcModeIfNeeded();
    c2_status_t importBufferToDecoder(std::shared_ptr<C2GraphicBlock> block);
    c2_status_t ensureTunneledState();
    c2_status_t ensureDecoderState();
    c2_status_t sendpacket(
            uint8_t *data, size_t size, uint64_t pts,
            uint64_t frameIndex, uint32_t flags);
    c2_status_t getoutframe(WorkEntry *entry);

    c2_status_t configFrameMetaIfNeeded(MppFrame frame, std::shared_ptr<C2GraphicBlock> block);
    c2_status_t checkUseScaleMeta(buffer_handle_t handle);
    c2_status_t checkUseScaleDown(buffer_handle_t handle);

    void releaseAllBuffers();
    std::shared_ptr<OutBuffer> findOutBuffer(int32_t bufferId);

    inline bool isDropFrame(uint64_t pts) {
        Vector<uint64_t>::iterator it;
        for (it = mDropFrames.begin(); it != mDropFrames.end(); it++) {
            if (*it == pts) {
                mDropFrames.erase(it);
                return true;
            }
        }
        return false;
    }


    C2_DO_NOT_COPY(C2RKMpiDec);
};

C2ComponentFactory* CreateRKMpiDecFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_SOFT_MPI_DEC_H_
