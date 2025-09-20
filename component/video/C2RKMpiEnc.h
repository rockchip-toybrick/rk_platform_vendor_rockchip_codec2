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

#ifndef ANDROID_C2_RK_MPI_ENC_H__
#define ANDROID_C2_RK_MPI_ENC_H__

#include "C2RKComponent.h"
#include "C2RKInterface.h"

#include "rk_mpi.h"

#include <utils/Vector.h>

namespace android {

class C2RKMlvecLegacy;
class C2RKDump;
class C2RKYolov5Session;
struct ImageBuffer;
struct RoiRegionCfg;

struct C2RKMpiEnc : public C2RKComponent {
public:
    class IntfImpl;

    C2RKMpiEnc(
        const char *name,
        const char *mime,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2RKMpiEnc();

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

    c2_status_t onDrainWork(const std::unique_ptr<C2Work> &work = nullptr);
    c2_status_t onDetectResultReady(ImageBuffer *srcImage, void *result);

private:
    /* DMA buffer memery */
    typedef struct {
        int32_t  fd;
        int32_t  size;
        void    *handler; /* buffer_handle_t */
        void    *npuMaps;
    } MyDmaBuffer_t;

    /* Supported lists for InputFormat */
    typedef enum {
        C2_INPUT_FMT_UNKNOWN = 0,
        C2_INPUT_FMT_YUV420SP,
        C2_IPNUT_FMT_RGBA,
    } MyInputFormat;

    /* Super Encoding Mode */
    typedef enum {
        C2_SUPER_MODE_NONE = 0,
        C2_SUPER_MODE_V1_QUALITY_FIRST,        // image quality first
        C2_SUPER_MODE_V1_COMPRESS_FIRST,       // compressibility first
        C2_SUPER_MODE_V3_QUALITY_FIRST,
        C2_SUPER_MODE_V3_COMPRESS_FIRST,
        C2_SUPER_MODE_BUTT,
    } MySuperMode;

    class WorkHandler : public AHandler {
    public:
        enum {
            kWhatDrainWork,
            kWhatStop,
        };

        WorkHandler() { mRunning = true; }
        ~WorkHandler() override = default;

        void setComponent(C2RKMpiEnc *thiz);
        void startWork();
        void stopWork();
        void waitDrainEOS();

    protected:
        void onMessageReceived(const sp<AMessage> &msg) override;

    private:
        C2RKMpiEnc *mThiz;
        bool mRunning;
    };

    const char* mName;
    const char* mMime;
    std::shared_ptr<IntfImpl>          mIntf;
    std::shared_ptr<C2BlockPool>       mBlockPool;

    std::shared_ptr<C2RKMlvecLegacy>   mMlvec;
    std::shared_ptr<C2RKDump>          mDumper;
    // npu object detection
    std::shared_ptr<C2RKYolov5Session> mRknnSession;
    std::unique_ptr<MyDmaBuffer_t>     mDmaMem;

    sp<ALooper>      mLooper;
    sp<WorkHandler>  mHandler;

    void            *mRoiCtx;

    /* MPI interface parameters */
    MppCtx           mMppCtx;
    MppApi          *mMppMpi;
    MppBuffer        mMdInfo; /* motion info buffer */
    MppBufferGroup   mGroup;
    MppEncCfg        mEncCfg;
    MppCodingType    mCodingType;
    MppFrameFormat   mInputMppFmt;
    int32_t          mChipType;

    bool             mStarted;
    bool             mInputScalar;
    bool             mSpsPpsHeaderReceived;
    bool             mSawInputEOS;
    bool             mOutputEOS;
    bool             mSignalledError;

    int32_t          mHorStride;
    int32_t          mVerStride;
    int32_t          mCurLayerCount;
    int32_t          mInputCount;

    // configurations used by component in process
    // (TODO: keep this in intf but make them internal only)
    uint32_t mProfile;
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;

    void fillEmptyWork(const std::unique_ptr<C2Work> &work);
    void finishWork(
            const std::unique_ptr<C2Work> &work,
            MppPacket entry);

    /* packet output looper */
    c2_status_t setupAndStartLooper();
    c2_status_t stopAndReleaseLooper();

    c2_status_t setupBaseCodec();
    c2_status_t setupInputScalar();
    c2_status_t setupPreProcess();
    c2_status_t setupSuperProcess();
    c2_status_t setupSceneMode();
    c2_status_t setupSliceSize();
    c2_status_t setupReencTimes();
    c2_status_t setupFrameRate();
    c2_status_t setupBitRate();
    c2_status_t setupProfileParams();
    c2_status_t setupQp();
    c2_status_t setupVuiParams();
    c2_status_t setupTemporalLayers();
    c2_status_t setupPrependHeaderSetting();
    c2_status_t setupIntraRefresh();
    c2_status_t setupSuperModeIfNeeded();
    c2_status_t setupMlvecIfNeeded();
    c2_status_t setupEncCfg();

    c2_status_t initEncoder();
    c2_status_t handleCommonDynamicCfg();
    c2_status_t handleRequestSyncFrame();
    c2_status_t handleMlvecDynamicCfg(MppMeta meta);
    c2_status_t handleRoiRegionRequest(MppMeta meta, Vector<RoiRegionCfg> regions);
    c2_status_t handleRknnDetection(
            const std::unique_ptr<C2Work> &work, MyDmaBuffer_t dbuffer);

    bool needRgaConvert(uint32_t width, uint32_t height, MppFrameFormat fmt);
    // get RGA color space mode for rgba->yuv conversion
    int32_t getRgaColorSpaceMode();

    c2_status_t getInBufferFromWork(
            const std::unique_ptr<C2Work> &work, MyDmaBuffer_t *outBuffer);
    c2_status_t sendframe(MyDmaBuffer_t dBuffer, uint64_t pts, uint32_t flags);
    c2_status_t getoutpacket(MppPacket *entry);

    C2_DO_NOT_COPY(C2RKMpiEnc);
};

C2ComponentFactory* CreateRKMpiEncFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_RK_MPI_ENC_H__

