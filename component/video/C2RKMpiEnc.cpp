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

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKMpiEnc"

#include <Codec2Mapper.h>
#include <C2PlatformSupport.h>
#include <C2AllocatorGralloc.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBufferAllocator.h>
#include <cutils/properties.h>

#include "mpp_rc_api.h"

#include "C2RKMpiEnc.h"
#include "C2RKLog.h"
#include "C2RKPlatformSupport.h"
#include "C2RKMediaUtils.h"
#include "C2RKRgaDef.h"
#include "C2RKExtendParameters.h"
#include "C2RKCodecMapper.h"
#include "C2RKChipCapDef.h"
#include "C2RKGrallocOps.h"
#include "C2RKMemTrace.h"
#include "C2RKPropsDef.h"
#include "C2RKMlvecLegacy.h"
#include "C2RKDump.h"
#include "C2RKMpiRoiUtils.h"
#include "C2RKYolov5Session.h"
#include "C2RKVersion.h"

namespace android {

void ParseGop(
        const C2StreamGopTuning::output &gop,
        uint32_t *syncInterval, uint32_t *iInterval, uint32_t *maxBframes) {
    uint32_t syncInt = 1;
    uint32_t iInt = 1;

    for (size_t i = 0; i < gop.flexCount(); ++i) {
        const C2GopLayerStruct &layer = gop.m.values[i];
        if (layer.count == UINT32_MAX) {
            syncInt = 0;
        } else if (syncInt <= UINT32_MAX / (layer.count + 1)) {
            syncInt *= (layer.count + 1);
        }
        if ((layer.type_ & I_FRAME) == 0) {
            if (layer.count == UINT32_MAX) {
                iInt = 0;
            } else if (iInt <= UINT32_MAX / (layer.count + 1)) {
                iInt *= (layer.count + 1);
            }
        }
        if (layer.type_ == C2Config::picture_type_t(P_FRAME | B_FRAME) && maxBframes) {
            *maxBframes = layer.count;
        }
    }

    if (syncInterval) {
        *syncInterval = syncInt;
    }
    if (iInterval) {
        *iInterval = iInt;
    }
}

struct MlvecParams {
    std::shared_ptr<C2DriverVersion::output> driverInfo;
    std::shared_ptr<C2MaxLayerCount::output> maxLayerCount;
    std::shared_ptr<C2LowLatencyMode::output> lowLatencyMode;
    std::shared_ptr<C2MaxLTRFramesCount::output> maxLTRFramesCount;
    std::shared_ptr<C2PreOPSupport::output> preOPSupport;
    std::shared_ptr<C2MProfileLevel::output> profileLevel;
    std::shared_ptr<C2SliceSpacing::output> sliceSpacing;
    std::shared_ptr<C2RateControl::output> rateControl;
    std::shared_ptr<C2NumLTRFrms::output> numLTRFrms;
    std::shared_ptr<C2SarSize::output> sarSize;
    std::shared_ptr<C2InputQueuCtl::output> inputQueueCtl;
    std::shared_ptr<C2LtrCtlMark::input> ltrMarkFrmCtl;
    std::shared_ptr<C2LtrCtlUse::input> ltrUseFrmCtl;
    std::shared_ptr<C2FrameQPCtl::input> frameQPCtl;
    std::shared_ptr<C2BaseLayerPid::input> baseLayerPid;
    std::shared_ptr<C2TriggerTime::input> triggerTime;
};

class C2RKMpiEnc::IntfImpl : public C2RKInterface<void>::BaseParams {
public:
    explicit IntfImpl(
            const std::shared_ptr<C2ReflectorHelper> &helper,
            C2String name,
            C2Component::kind_t kind,
            C2Component::domain_t domain,
            C2String mediaType)
        : C2RKInterface<void>::BaseParams(
                helper,
                name,
                kind,
                domain,
                mediaType) {
        noPrivateBuffers(); // TODO: account for our buffers here
        noInputReferences();
        noOutputReferences();
        noTimeStretch();
        setDerivedInstance(this);

        mMlvecParams = std::make_shared<MlvecParams>();

        int64_t inputUsage = 0;

        /*
         * Some encoders have input alignment requirement, input buffer need
         * rga conversion in first. detail see needRgaConvert(). so for fit rga
         * compatibility, add following buffer usages:
         * 1. allocate buffer within 4G to avoid rga2 error.
         * 2. add the minimum RGA alignment in all platforms may needs rga conversion.
         */
        if (C2RKChipCapDef::get()->getChipType() == RK_CHIP_3588 ||
            C2RKChipCapDef::get()->getChipType() == RK_CHIP_356X) {
            inputUsage |= RK_GRALLOC_USAGE_WITHIN_4G;
        }
        if (C2RKChipCapDef::get()->getChipType() != RK_CHIP_3588 &&
            C2RKChipCapDef::get()->getChipType() != RK_CHIP_3562 &&
            C2RKChipCapDef::get()->getChipType() != RK_CHIP_3576 &&
            C2RKChipCapDef::get()->getChipType() != RK_CHIP_3528) {
            inputUsage |= RK_GRALLOC_USAGE_STRIDE_ALIGN_16;
        }

        addParameter(
                DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                .withConstValue(new C2StreamUsageTuning::input(
                        0u, inputUsage))
                .build());

        addParameter(
                DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                .withConstValue(new C2ComponentAttributesSetting(
                    C2Component::ATTRIB_IS_TEMPORAL))
                .build());

        addParameter(
                DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::input(0u, 176, 144))
                .withFields({
                    C2F(mSize, width).inRange(90, 7680, 2),
                    C2F(mSize, height).inRange(90, 7680, 2),
                })
                .withSetter(SizeSetter)
                .build());

        addParameter(
                DefineParam(mGop, C2_PARAMKEY_GOP)
                .withDefault(C2StreamGopTuning::output::AllocShared(
                        0 /* flexCount */, 0u /* stream */))
                .withFields({C2F(mGop, m.values[0].type_).any(),
                             C2F(mGop, m.values[0].count).any()})
                .withSetter(GopSetter)
                .build());

        addParameter(
                DefineParam(mRotation, C2_PARAMKEY_ROTATION)
                .withDefault(new C2StreamRotationInfo::output(0u, 0))
                .withFields({C2F(mRotation, flip).any(),
                             C2F(mRotation, value).any()})
                .withSetter(RotationSetter)
                .build());

        addParameter(
                DefineParam(mPictureQuantization, C2_PARAMKEY_PICTURE_QUANTIZATION)
                .withDefault(C2StreamPictureQuantizationTuning::output::AllocShared(
                        0 /* flexCount */, 0u /* stream */))
                .withFields({C2F(mPictureQuantization, m.values[0].type_).oneOf(
                                {C2Config::picture_type_t(I_FRAME),
                                 C2Config::picture_type_t(P_FRAME),
                                 C2Config::picture_type_t(B_FRAME)}),
                             C2F(mPictureQuantization, m.values[0].min).any(),
                             C2F(mPictureQuantization, m.values[0].max).any()})
                .withSetter(PictureQuantizationSetter)
                .build());

        addParameter(
                DefineParam(mActualInputDelay, C2_PARAMKEY_INPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::input(0))
                .withFields({C2F(mActualInputDelay, value).inRange(0, 2)})
                .calculatedAs(InputDelaySetter, mGop)
                .build());

        addParameter(
                DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
                .withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
                // TODO: More restriction?
                .withFields({C2F(mFrameRate, value).greaterThan(0.)})
                .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrateMode, C2_PARAMKEY_BITRATE_MODE)
                .withDefault(new C2StreamBitrateModeTuning::output(
                        0u, C2Config::BITRATE_VARIABLE))
                .withFields({
                    C2F(mBitrateMode, value).oneOf({
                        C2Config::BITRATE_CONST,
                        C2Config::BITRATE_VARIABLE,
                        C2Config::BITRATE_IGNORE})
                })
                .withSetter(
                    Setter<decltype(*mBitrateMode)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
                .withDefault(new C2StreamBitrateInfo::output(0u, 64000))
                .withFields({C2F(mBitrate, value).inRange(4096, 10000000)})
                .withSetter(BitrateSetter)
                .build());

        addParameter(
                DefineParam(mIntraRefresh, C2_PARAMKEY_INTRA_REFRESH)
                .withDefault(new C2StreamIntraRefreshTuning::output(
                        0u, C2Config::INTRA_REFRESH_DISABLED, 0.))
                .withFields({
                    C2F(mIntraRefresh, mode).oneOf({
                        C2Config::INTRA_REFRESH_DISABLED, C2Config::INTRA_REFRESH_ARBITRARY }),
                    C2F(mIntraRefresh, period).any()
                })
                .withSetter(IntraRefreshSetter)
                .build());

        if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::output(
                            0u, PROFILE_AVC_BASELINE, LEVEL_AVC_3_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                            PROFILE_AVC_BASELINE, PROFILE_AVC_MAIN, PROFILE_AVC_HIGH,
                        }),
                        C2F(mProfileLevel, level).oneOf({
                            LEVEL_AVC_1,   LEVEL_AVC_1B,  LEVEL_AVC_1_1,  LEVEL_AVC_1_2,
                            LEVEL_AVC_1_3, LEVEL_AVC_2,   LEVEL_AVC_2_1,  LEVEL_AVC_2_2,
                            LEVEL_AVC_3,   LEVEL_AVC_3_1, LEVEL_AVC_3_2,  LEVEL_AVC_4,
                            LEVEL_AVC_4_1, LEVEL_AVC_4_2, LEVEL_AVC_5,    LEVEL_AVC_5_1,
                        }),
                    })
                    .withSetter(AVCProfileLevelSetter, mSize, mFrameRate, mBitrate)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_HEVC) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::output(
                            0u, PROFILE_HEVC_MAIN, LEVEL_HEVC_MAIN_4_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                            PROFILE_HEVC_MAIN,
                        }),
                        C2F(mProfileLevel, level).oneOf({
                            LEVEL_HEVC_MAIN_1,   LEVEL_HEVC_MAIN_2,   LEVEL_HEVC_MAIN_2_1,
                            LEVEL_HEVC_MAIN_3,   LEVEL_HEVC_MAIN_3_1, LEVEL_HEVC_MAIN_4,
                            LEVEL_HEVC_MAIN_4_1, LEVEL_HEVC_MAIN_5,   LEVEL_HEVC_MAIN_5_1,
                        }),
                    })
                    .withSetter(HEVCProfileLevelSetter, mSize, mFrameRate, mBitrate)
                    .build());
        } else {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::output(
                            0u, PROFILE_UNUSED, LEVEL_UNUSED))
                    .withFields({
                        C2F(mProfileLevel, profile).any(),
                        C2F(mProfileLevel, level).any(),
                    })
                    .withSetter(DefaultProfileLevelSetter, mSize, mFrameRate, mBitrate)
                    .build());

        }

        addParameter(
                DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
                .withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
                .withFields({C2F(mRequestSync, value).oneOf({ C2_FALSE, C2_TRUE }) })
                .withSetter(Setter<decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
                .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
                .withFields({C2F(mSyncFramePeriod, value).any()})
                .withSetter(Setter<decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsInfo::input(
                    0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({
                    C2F(mColorAspects, range).inRange(
                                C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                    C2F(mColorAspects, primaries).inRange(
                                C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                    C2F(mColorAspects, transfer).inRange(
                                C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                    C2F(mColorAspects, matrix).inRange(
                                C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
                })
                .withSetter(ColorAspectsSetter)
                .build());

        addParameter(
                DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsInfo::output(
                        0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
                        C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({
                    C2F(mCodedColorAspects, range).inRange(
                                C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                    C2F(mCodedColorAspects, primaries).inRange(
                                C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                    C2F(mCodedColorAspects, transfer).inRange(
                                C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                    C2F(mCodedColorAspects, matrix).inRange(
                                C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
                })
                .withSetter(CodedColorAspectsSetter, mColorAspects)
                .build());

        addParameter(
                DefineParam(mLayering, C2_PARAMKEY_TEMPORAL_LAYERING)
                .withDefault(C2StreamTemporalLayeringTuning::output::AllocShared(0u, 0, 0, 0))
                .withFields({
                    C2F(mLayering, m.layerCount).inRange(0, 4),
                    C2F(mLayering, m.bLayerCount).inRange(0, 0),
                    C2F(mLayering, m.bitrateRatios).inRange(0., 1.)
                })
                .withSetter(LayeringSetter)
                .build());

        addParameter(
                DefineParam(mPrependHeaderMode, C2_PARAMKEY_PREPEND_HEADER_MODE)
                .withDefault(new C2PrependHeaderModeSetting(PREPEND_HEADER_TO_NONE))
                .withFields({C2F(mPrependHeaderMode, value).any()})
                .withSetter(PrependHeaderModeSetter)
                .build());

        addParameter(
                DefineParam(mMinQuality, C2_PARAMKEY_ENCODING_QUALITY_LEVEL)
                .withDefault(new C2EncodingQualityLevel(C2PlatformConfig::encoding_quality_level_t::NONE))
                .withFields({ C2F(mMinQuality, value).oneOf({
                                    C2PlatformConfig::encoding_quality_level_t::NONE,
                                    C2PlatformConfig::encoding_quality_level_t::S_HANDHELD})})
                .withSetter(MinQualitySetter)
                .build());

        /* extend parameter definition */
        addParameter(
                DefineParam(mSceneMode, C2_PARAMKEY_ENC_SCENE_MODE)
                .withDefault(new C2StreamEncSceneModeInfo::input(0))
                .withFields({C2F(mSceneMode, value).any()})
                .withSetter(Setter<decltype(mSceneMode)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mSliceSize, C2_PARAMKEY_ENC_SLICE_SIZE)
                .withDefault(new C2StreamEncSliceSizeInfo::input(0))
                .withFields({C2F(mSliceSize, value).any()})
                .withSetter(Setter<decltype(mSliceSize)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mInputScalar, C2_PARAMKEY_ENC_INPUT_SCALAR)
                .withDefault(new C2StreamEncInputScalar::input(0, 0))
                .withFields({
                    C2F(mInputScalar, width).any(),
                    C2F(mInputScalar, height).any()
                })
                .withSetter(InputScalarSetter)
                .build());

        // super encoding mode settings
        addParameter(
                DefineParam(mSESettings, C2_PARAMKEY_ENC_SE_MODE_SETTING)
                .withDefault(new C2StreamEncSEModeSetting::input(0, 0, 0, 0, 0))
                .withFields({
                    C2F(mSESettings, mode).any(),
                    C2F(mSESettings, bgDeltaQp).any(),
                    C2F(mSESettings, fgDeltaQp).any(),
                    C2F(mSESettings, mapMinQp).any(),
                    C2F(mSESettings, mapMaxQp).any(),
                })
                .withSetter(SESettingsSetter)
                .build());

        addParameter(
                DefineParam(mDisableSEI, C2_PARAMKEY_ENC_DISABLE_SEI)
                .withDefault(new C2StreamEncDisableSEI::input(0))
                .withFields({C2F(mDisableSEI, value).any()})
                .withSetter(Setter<decltype(mDisableSEI)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mRoiRegionCfg, C2_PARAMKEY_ENC_ROI_REGION_CFG)
                .withDefault(new C2StreamEncRoiRegionCfg::input())
                .withFields({
                    C2F(mRoiRegionCfg, left).any(),
                    C2F(mRoiRegionCfg, right).any(),
                    C2F(mRoiRegionCfg, width).any(),
                    C2F(mRoiRegionCfg, height).any(),
                    C2F(mRoiRegionCfg, forceIntra).any(),
                    C2F(mRoiRegionCfg, qpMode).any(),
                    C2F(mRoiRegionCfg, qpVal).any()
                })
                .withSetter(RoiRegionCfgSetter)
                .build());

        addParameter(
                DefineParam(mRoiRegion2Cfg, C2_PARAMKEY_ENC_ROI_REGION2_CFG)
                .withDefault(new C2StreamEncRoiRegion2Cfg::input())
                .withFields({
                    C2F(mRoiRegion2Cfg, left).any(),
                    C2F(mRoiRegion2Cfg, right).any(),
                    C2F(mRoiRegion2Cfg, width).any(),
                    C2F(mRoiRegion2Cfg, height).any(),
                    C2F(mRoiRegion2Cfg, forceIntra).any(),
                    C2F(mRoiRegion2Cfg, qpMode).any(),
                    C2F(mRoiRegion2Cfg, qpVal).any()
                })
                .withSetter(RoiRegion2CfgSetter)
                .build());

        addParameter(
                DefineParam(mRoiRegion3Cfg, C2_PARAMKEY_ENC_ROI_REGION3_CFG)
                .withDefault(new C2StreamEncRoiRegion3Cfg::input())
                .withFields({
                    C2F(mRoiRegion3Cfg, left).any(),
                    C2F(mRoiRegion3Cfg, right).any(),
                    C2F(mRoiRegion3Cfg, width).any(),
                    C2F(mRoiRegion3Cfg, height).any(),
                    C2F(mRoiRegion3Cfg, forceIntra).any(),
                    C2F(mRoiRegion3Cfg, qpMode).any(),
                    C2F(mRoiRegion3Cfg, qpVal).any()
                })
                .withSetter(RoiRegion3CfgSetter)
                .build());

        addParameter(
                DefineParam(mRoiRegion4Cfg, C2_PARAMKEY_ENC_ROI_REGION4_CFG)
                .withDefault(new C2StreamEncRoiRegion4Cfg::input())
                .withFields({
                    C2F(mRoiRegion4Cfg, left).any(),
                    C2F(mRoiRegion4Cfg, right).any(),
                    C2F(mRoiRegion4Cfg, width).any(),
                    C2F(mRoiRegion4Cfg, height).any(),
                    C2F(mRoiRegion4Cfg, forceIntra).any(),
                    C2F(mRoiRegion4Cfg, qpMode).any(),
                    C2F(mRoiRegion4Cfg, qpVal).any()
                })
                .withSetter(RoiRegion4CfgSetter)
                .build());

        addParameter(
                DefineParam(mPreProcess, C2_PARAMKEY_ENC_PRE_PROCESS)
                .withDefault(new C2StreamEncPreProcess::input())
                .withFields({
                    C2F(mPreProcess, mirror).any(),
                    C2F(mPreProcess, flip).any(),
                })
                .withSetter(PreProcessSetter)
                .build());

        addParameter(
                DefineParam(mSuperProcess, C2_PARAMKEY_ENC_SUPER_PROCESS)
                .withDefault(new C2StreamEncSuperProcess::input())
                .withFields({
                    C2F(mSuperProcess, mode).inRange(0, 2),
                    C2F(mSuperProcess, iThd).any(),
                    C2F(mSuperProcess, pThd).any(),
                    C2F(mSuperProcess, reencTimes).any(),
                })
                .withSetter(SuperProcessSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->driverInfo, C2_PARAMKEY_MLVEC_ENC_DRI_VERSION)
                .withConstValue(new C2DriverVersion::output(MLVEC_DRIVER_VERSION))
                .build());

        addParameter(
                DefineParam(mMlvecParams->maxLayerCount, C2_PARAMKEY_MLVEC_MAX_TEMPORAL_LAYERS)
                .withConstValue(new C2MaxLayerCount::output(MLVEC_MAX_LAYER_COUNT))
                .build());

        addParameter(
                DefineParam(mMlvecParams->lowLatencyMode, C2_PARAMKEY_MLVEC_ENC_LOW_LATENCY_MODE)
                .withConstValue(new C2LowLatencyMode::output(MLVEC_LOW_LATENCY_MODE_ENABLE))
                .build());

        addParameter(
                DefineParam(mMlvecParams->maxLTRFramesCount, C2_PARAMKEY_MLVEC_MAX_LTR_FRAMES)
                .withConstValue(new C2MaxLTRFramesCount::output(MLVEC_MAX_LTR_FRAMES_COUNT))
                .build());

        addParameter(
                DefineParam(mMlvecParams->preOPSupport, C2_PARAMKEY_MLVEC_PRE_OP)
                .withConstValue(new C2PreOPSupport::output(
                        MLVEC_PRE_PROCESS_SCALE_SUPPORT, MLVEC_PRE_PROCESS_ROTATION_SUPPORT))
                .build());

        addParameter(
                DefineParam(mMlvecParams->profileLevel, C2_PARAMKEY_MLVEC_PROFILE_LEVEL)
                .withDefault(new C2MProfileLevel::output(0, 0))
                .withFields({
                    C2F(mMlvecParams->profileLevel, profile).any(),
                    C2F(mMlvecParams->profileLevel, level).any()
                })
                .withSetter(MProfileLevelSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->sliceSpacing, C2_PARAMKEY_MLVEC_SLICE_SPACING)
                .withDefault(new C2SliceSpacing::output(0))
                .withFields({C2F(mMlvecParams->sliceSpacing, spacing).any()})
                .withSetter(MSliceSpaceSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->rateControl, C2_PARAMKEY_MLVEC_RATE_CONTROL)
                .withDefault(new C2RateControl::output(-1))
                .withFields({C2F(mMlvecParams->rateControl, value).any()})
                .withSetter(Setter<decltype(
                    mMlvecParams->rateControl)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMlvecParams->numLTRFrms, C2_PARAMKEY_MLVEC_NUM_LTR_FRAMES)
                .withDefault(new C2NumLTRFrms::output(0))
                .withFields({C2F(mMlvecParams->numLTRFrms, num).any()})
                .withSetter(MNumLTRFrmsSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->sarSize, C2_PARAMKEY_MLVEC_SET_SAR_SIZE)
                .withDefault(new C2SarSize::output(0, 0))
                .withFields({
                    C2F(mMlvecParams->sarSize, width).any(),
                    C2F(mMlvecParams->sarSize, height).any(),
                })
                .withSetter(MSarSizeSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->inputQueueCtl, C2_PARAMKEY_MLVEC_INPUT_QUEUE_CTL)
                .withDefault(new C2InputQueuCtl::output(0))
                .withFields({C2F(mMlvecParams->inputQueueCtl, enable).oneOf({0, 1})})
                .withSetter(MInputQueueCtlSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->ltrMarkFrmCtl, C2_PARAMKEY_MLVEC_LTR_CTL_MARK)
                .withDefault(new C2LtrCtlMark::input(-1))
                .withFields({C2F(mMlvecParams->ltrMarkFrmCtl, markFrame).any()})
                .withSetter(MLtrMarkFrmSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->ltrUseFrmCtl, C2_PARAMKEY_MLVEC_LTR_CTL_USE)
                .withDefault(new C2LtrCtlUse::input(-1))
                .withFields({C2F(mMlvecParams->ltrUseFrmCtl, useFrame).any()})
                .withSetter(MLtrUseFrmSetter)
                .build());

        addParameter(
                DefineParam(mMlvecParams->frameQPCtl, C2_PARAMKEY_MLVEC_FRAME_QP_CTL)
                .withDefault(new C2FrameQPCtl::input(-1))
                .withFields({C2F(mMlvecParams->frameQPCtl, value).any()})
                .withSetter(Setter<decltype(
                    mMlvecParams->frameQPCtl)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMlvecParams->baseLayerPid, C2_PARAMKEY_MLVEC_BASE_LAYER_PID)
                .withDefault(new C2BaseLayerPid::input(-1))
                .withFields({C2F(mMlvecParams->baseLayerPid, value).any()})
                .withSetter(Setter<decltype(
                    mMlvecParams->baseLayerPid)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMlvecParams->triggerTime, C2_PARAMKEY_MLVEC_TRIGGER_TIME)
                .withDefault(new C2TriggerTime::input(-1))
                .withFields({C2F(mMlvecParams->triggerTime, timestamp).any()})
                .withSetter(MTriggerTimeSetter)
                .build());
    }

    static C2R InputDelaySetter(
            bool mayBlock,
            C2P<C2PortActualDelayTuning::input> &me,
            const C2P<C2StreamGopTuning::output> &gop) {
        (void)mayBlock;
        uint32_t maxBframes = 0;
        ParseGop(gop.v, nullptr, nullptr, &maxBframes);
        me.set().value = maxBframes;
        return C2R::Ok();
    }

    static C2R BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (me.v.value <= 4096) {
            me.set().value = 4096;
        }
        return res;
    }

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                          C2P<C2StreamPictureSizeInfo::input> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }

    static C2R IntraRefreshSetter(bool mayBlock, C2P<C2StreamIntraRefreshTuning::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (me.v.period < 1) {
            me.set().mode = C2Config::INTRA_REFRESH_DISABLED;
            me.set().period = 0;
        } else {
            // only support arbitrary mode (cyclic in our case)
            me.set().mode = C2Config::INTRA_REFRESH_ARBITRARY;
        }
        return res;
    }

    static C2R GopSetter(bool mayBlock, C2P<C2StreamGopTuning::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R RotationSetter(bool mayBlock, C2P<C2StreamRotationInfo::output> &me) {
        (void)mayBlock;
        // Note: SDK rotation is clock-wise, while C2 rotation is counter-clock-wise
        me.set().value = -(me.v.value);
        return C2R::Ok();
    }

    static C2R PictureQuantizationSetter(bool mayBlock,
                                         C2P<C2StreamPictureQuantizationTuning::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    uint32_t getSyncFramePeriod_l() const {
        if (mSyncFramePeriod->value < 0 || mSyncFramePeriod->value == INT64_MAX) {
            return 0;
        }
        double period = mSyncFramePeriod->value / 1e6 * mFrameRate->value;
        return (uint32_t)c2_max(c2_min(period + 0.5, double(UINT32_MAX)), 1.);
    }

    static C2R AVCProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            me.set().profile = PROFILE_AVC_MAIN;
        }

        struct LevelLimits {
            C2Config::level_t level;
            float mbsPerSec;
            uint64_t mbs;
            uint32_t bitrate;
        };
        constexpr LevelLimits kLimits[] = {
            { LEVEL_AVC_1,     1485,    99,     64000 },
            // Decoder does not properly handle level 1b.
            // { LEVEL_AVC_1B,    1485,   99,   128000 },
            { LEVEL_AVC_1_1,   3000,   396,    192000 },
            { LEVEL_AVC_1_2,   6000,   396,    384000 },
            { LEVEL_AVC_1_3,  11880,   396,    768000 },
            { LEVEL_AVC_2,    11880,   396,   2000000 },
            { LEVEL_AVC_2_1,  19800,   792,   4000000 },
            { LEVEL_AVC_2_2,  20250,  1620,   4000000 },
            { LEVEL_AVC_3,    40500,  1620,  10000000 },
            { LEVEL_AVC_3_1, 108000,  3600,  14000000 },
            { LEVEL_AVC_3_2, 216000,  5120,  20000000 },
            { LEVEL_AVC_4,   245760,  8192,  20000000 },
            { LEVEL_AVC_4_1, 245760,  8192,  50000000 },
            { LEVEL_AVC_4_2, 522240,  8704,  50000000 },
            { LEVEL_AVC_5,   589824, 22080, 135000000 },
        };

        uint64_t mbs = uint64_t((size.v.width + 15) / 16) * ((size.v.height + 15) / 16);
        float mbsPerSec = float(mbs) * frameRate.v.value;

        // Check if the supplied level meets the MB / bitrate requirements. If
        // not, update the level with the lowest level meeting the requirements.

        bool found = false;
        // By default needsUpdate = false in case the supplied level does meet
        // the requirements. For Level 1b, we want to update the level anyway,
        // so we set it to true in that case.
        bool needsUpdate = false;
        if (me.v.level == LEVEL_AVC_1B || !me.F(me.v.level).supportsAtAll(me.v.level)) {
            needsUpdate = true;
        }
        for (const LevelLimits &limit : kLimits) {
            if (mbs <= limit.mbs && mbsPerSec <= limit.mbsPerSec &&
                    bitrate.v.value <= limit.bitrate) {
                // This is the lowest level that meets the requirements, and if
                // we haven't seen the supplied level yet, that means we don't
                // need the update.
                if (needsUpdate) {
                    c2_info("Given level %x does not cover current configuration: "
                        "adjusting to %x", me.v.level, limit.level);
                    me.set().level = limit.level;
                }
                found = true;
                break;
            }
            if (me.v.level == limit.level) {
                // We break out of the loop when the lowest feasible level is
                // found. The fact that we're here means that our level doesn't
                // meet the requirement and needs to be updated.
                needsUpdate = true;
            }
        }
        if (!found || me.v.level > LEVEL_AVC_5) {
            // We set to the highest supported level.
            me.set().level = LEVEL_AVC_5;
        }

        return C2R::Ok();
    }

    static C2R HEVCProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            me.set().profile = PROFILE_HEVC_MAIN;
        }

        struct LevelLimits {
            C2Config::level_t level;
            uint64_t samplesPerSec;
            uint64_t samples;
            uint32_t bitrate;
        };

        constexpr LevelLimits kLimits[] = {
            { LEVEL_HEVC_MAIN_1,       552960,    36864,    128000 },
            { LEVEL_HEVC_MAIN_2,      3686400,   122880,   1500000 },
            { LEVEL_HEVC_MAIN_2_1,    7372800,   245760,   3000000 },
            { LEVEL_HEVC_MAIN_3,     16588800,   552960,   6000000 },
            { LEVEL_HEVC_MAIN_3_1,   33177600,   983040,  10000000 },
            { LEVEL_HEVC_MAIN_4,     66846720,  2228224,  12000000 },
            { LEVEL_HEVC_MAIN_4_1,  133693440,  2228224,  20000000 },
            { LEVEL_HEVC_MAIN_5,    267386880,  8912896,  25000000 },
            { LEVEL_HEVC_MAIN_5_1,  534773760,  8912896,  40000000 },
            { LEVEL_HEVC_MAIN_5_2, 1069547520,  8912896,  60000000 },
            { LEVEL_HEVC_MAIN_6,   1069547520, 35651584,  60000000 },
            { LEVEL_HEVC_MAIN_6_1, 2139095040, 35651584, 120000000 },
            { LEVEL_HEVC_MAIN_6_2, 4278190080, 35651584, 240000000 },
        };

        uint64_t samples = size.v.width * size.v.height;
        uint64_t samplesPerSec = samples * frameRate.v.value;

        // Check if the supplied level meets the MB / bitrate requirements. If
        // not, update the level with the lowest level meeting the requirements.

        bool found = false;
        // By default needsUpdate = false in case the supplied level does meet
        // the requirements.
        bool needsUpdate = false;
        if (!me.F(me.v.level).supportsAtAll(me.v.level)) {
            needsUpdate = true;
        }
        for (const LevelLimits &limit : kLimits) {
            if (samples <= limit.samples && samplesPerSec <= limit.samplesPerSec &&
                    bitrate.v.value <= limit.bitrate) {
                // This is the lowest level that meets the requirements, and if
                // we haven't seen the supplied level yet, that means we don't
                // need the update.
                if (needsUpdate) {
                    c2_info("Given level %x does not cover current configuration: "
                        "adjusting to %x", me.v.level, limit.level);
                    me.set().level = limit.level;
                }
                found = true;
                break;
            }
            if (me.v.level == limit.level) {
                // We break out of the loop when the lowest feasible level is
                // found. The fact that we're here means that our level doesn't
                // meet the requirement and needs to be updated.
                needsUpdate = true;
            }
        }
        if (!found || me.v.level > LEVEL_HEVC_MAIN_6_2) {
            // We set to the highest supported level.
            me.set().level = LEVEL_HEVC_MAIN_6_2;
        }
        return C2R::Ok();
    }

    static C2R DefaultProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        (void)me;
        (void)size;
        (void)frameRate;
        (void)bitrate;
        return C2R::Ok();
    }

    static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me) {
        (void)mayBlock;
        if (me.v.range > C2Color::RANGE_OTHER) {
            me.set().range = C2Color::RANGE_OTHER;
        }
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
            me.set().primaries = C2Color::PRIMARIES_OTHER;
        }
        if (me.v.transfer > C2Color::TRANSFER_OTHER) {
            me.set().transfer = C2Color::TRANSFER_OTHER;
        }
        if (me.v.matrix > C2Color::MATRIX_OTHER) {
            me.set().matrix = C2Color::MATRIX_OTHER;
        }
        return C2R::Ok();
    }

    static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                                       const C2P<C2StreamColorAspectsInfo::input> &coded) {
        (void)mayBlock;
        me.set().range = coded.v.range;
        me.set().primaries = coded.v.primaries;
        me.set().transfer = coded.v.transfer;
        me.set().matrix = coded.v.matrix;
        return C2R::Ok();
    }

    static C2R LayeringSetter(
            bool mayBlock, C2P<C2StreamTemporalLayeringTuning::output>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R PrependHeaderModeSetter(
            bool mayBlock, C2P<C2PrependHeaderModeSetting>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MinQualitySetter(
            bool mayBlock, C2P<C2EncodingQualityLevel>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R InputScalarSetter(
            bool mayBlock, C2P<C2StreamEncInputScalar::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R SESettingsSetter(
            bool mayBlock, C2P<C2StreamEncSEModeSetting::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R RoiRegionCfgSetter(
            bool mayBlock, C2P<C2StreamEncRoiRegionCfg::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R RoiRegion2CfgSetter(
            bool mayBlock, C2P<C2StreamEncRoiRegion2Cfg::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R RoiRegion3CfgSetter(
            bool mayBlock, C2P<C2StreamEncRoiRegion3Cfg::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R RoiRegion4CfgSetter(
            bool mayBlock, C2P<C2StreamEncRoiRegion4Cfg::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R PreProcessSetter(
            bool mayBlock, C2P<C2StreamEncPreProcess::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R SuperProcessSetter(
            bool mayBlock, C2P<C2StreamEncSuperProcess::input>& me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MProfileLevelSetter(
            bool mayBlock, C2P<C2MProfileLevel::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MSliceSpaceSetter(
            bool mayBlock, C2P<C2SliceSpacing::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MNumLTRFrmsSetter(
            bool mayBlock, C2P<C2NumLTRFrms::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MSarSizeSetter(
            bool mayBlock, C2P<C2SarSize::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MInputQueueCtlSetter(
            bool mayBlock, C2P<C2InputQueuCtl::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MLtrMarkFrmSetter(
            bool mayBlock, C2P<C2LtrCtlMark::input> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MLtrUseFrmSetter(
            bool mayBlock, C2P<C2LtrCtlUse::input> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R MTriggerTimeSetter(
            bool mayBlock, C2P<C2TriggerTime::input> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    uint32_t getProfile_l(MppCodingType type) const {
        uint32_t cProfile = mProfileLevel->profile;
        uint32_t mProfile = mMlvecParams->profileLevel->profile;

        if (type == MPP_VIDEO_CodingAVC) {
            if (mProfile > 0) {
                return C2RKCodecMapper::getMppH264Profile(mProfile, false);
            } else {
                return C2RKCodecMapper::getMppH264Profile(cProfile, true);
            }
        } else if (type == MPP_VIDEO_CodingHEVC) {
            return C2RKCodecMapper::getMppH265Profile(cProfile);
        } else {
            return 0;
        }
    }

    uint32_t getLevel_l(MppCodingType type) const {
        uint32_t cLevel = mProfileLevel->level;
        uint32_t mLevel = mMlvecParams->profileLevel->level;

        if (type == MPP_VIDEO_CodingAVC) {
            if (mLevel) {
                return C2RKCodecMapper::getMppH264Level(mLevel, false);
            } else {
                return C2RKCodecMapper::getMppH264Level(cLevel, true);
            }
        }  else if (type == MPP_VIDEO_CodingHEVC) {
            return C2RKCodecMapper::getMppH265Level(cLevel);
        } else {
            return 0;
        }
    }

    uint32_t getBitrateMode_l() const {
        int32_t cMode = mBitrateMode->value;
        int32_t mMode = mMlvecParams->rateControl->value;

        if (mMode >= 0) {
            return C2RKCodecMapper::getMppBitrateMode(mMode, false);
        } else {
            return C2RKCodecMapper::getMppBitrateMode(cMode, true);
        }
    }

    bool getIsDisableSEI() const {
        if (mDisableSEI && mDisableSEI->value > 0) {
            return true;
        }
        return false;
    }

#define SET_ROI_REGION(inCfg, regions) \
    if (inCfg && inCfg->width > 0 && inCfg->height > 0) { \
        RoiRegionCfg region; \
        region.x = inCfg->left; \
        region.y = inCfg->right; \
        region.w = inCfg->width; \
        region.h = inCfg->height; \
        region.force_intra = inCfg->forceIntra; \
        region.qp_mode = inCfg->qpMode; \
        region.qp_val = inCfg->qpVal; \
        inCfg->width = -1; \
        inCfg->height = -1; \
        regions.push(region); \
    } \

   Vector<RoiRegionCfg> getRoiRegionCfg() {
        Vector<RoiRegionCfg> regions;

        SET_ROI_REGION(mRoiRegionCfg,  regions)
        SET_ROI_REGION(mRoiRegion2Cfg, regions)
        SET_ROI_REGION(mRoiRegion3Cfg, regions)
        SET_ROI_REGION(mRoiRegion4Cfg, regions)

        return regions;
    }

    // unsafe getters
    std::shared_ptr<C2StreamPictureSizeInfo::input> getSize_l() const
    { return mSize; }
    std::shared_ptr<C2StreamIntraRefreshTuning::output> getIntraRefresh_l() const
    { return mIntraRefresh; }
    std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate_l() const
    { return mFrameRate; }
    std::shared_ptr<C2StreamBitrateInfo::output> getBitrate_l() const
    { return mBitrate; }
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> getRequestSync_l() const
    { return mRequestSync; }
    std::shared_ptr<C2StreamGopTuning::output> getGop_l() const
    { return mGop; }
    std::shared_ptr<C2StreamRotationInfo::output> getRotation_l() const
    { return mRotation; }
    std::shared_ptr<C2StreamPictureQuantizationTuning::output> getPictureQuantization_l() const
    { return mPictureQuantization; }
    std::shared_ptr<C2StreamColorAspectsInfo::output> getCodedColorAspects_l() const
    { return mCodedColorAspects; }
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> getTemporalLayers_l() const
    { return mLayering; }
    std::shared_ptr<C2PrependHeaderModeSetting> getPrependHeaderMode_l() const
    { return mPrependHeaderMode; }
    std::shared_ptr<C2EncodingQualityLevel> getQualityLevel_l() const
    { return mMinQuality; }
    std::shared_ptr<C2StreamEncSceneModeInfo::input> getSceneMode_l() const
    { return mSceneMode; }
    std::shared_ptr<C2StreamEncSliceSizeInfo::input> getSliceSize_l() const
    { return mSliceSize; }
    std::shared_ptr<C2StreamEncInputScalar::input> getInputScalar_l() const
    { return mInputScalar; }
    std::shared_ptr<C2StreamEncPreProcess::input> getPreProcess_l() const
    { return mPreProcess; }
    std::shared_ptr<C2StreamEncSuperProcess::input> getSuperProcess_l() const
    { return mSuperProcess; }
    std::shared_ptr<C2StreamEncSEModeSetting::input> getSuperEncodingSettings_l() const
    { return mSESettings; }
    std::shared_ptr<MlvecParams> getMlvecParams_l() const
    { return mMlvecParams; }

private:
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
    std::shared_ptr<C2StreamGopTuning::output> mGop;
    std::shared_ptr<C2StreamRotationInfo::output> mRotation;
    std::shared_ptr<C2StreamPictureQuantizationTuning::output> mPictureQuantization;
    std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> mLayering;
    std::shared_ptr<C2PrependHeaderModeSetting> mPrependHeaderMode;
    std::shared_ptr<C2EncodingQualityLevel> mMinQuality;

    /* extend parameter definition */
    std::shared_ptr<C2StreamEncSceneModeInfo::input> mSceneMode;
    std::shared_ptr<C2StreamEncSliceSizeInfo::input> mSliceSize;
    std::shared_ptr<C2StreamEncInputScalar::input> mInputScalar;
    std::shared_ptr<C2StreamEncSEModeSetting::input> mSESettings;
    std::shared_ptr<C2StreamEncDisableSEI::input> mDisableSEI;
    std::shared_ptr<C2StreamEncRoiRegionCfg::input> mRoiRegionCfg;
    std::shared_ptr<C2StreamEncRoiRegion2Cfg::input> mRoiRegion2Cfg;
    std::shared_ptr<C2StreamEncRoiRegion3Cfg::input> mRoiRegion3Cfg;
    std::shared_ptr<C2StreamEncRoiRegion4Cfg::input> mRoiRegion4Cfg;
    std::shared_ptr<C2StreamEncPreProcess::input> mPreProcess;
    std::shared_ptr<C2StreamEncSuperProcess::input> mSuperProcess;
    std::shared_ptr<MlvecParams> mMlvecParams;
};

status_t postAndAwaitResponse(const sp<AMessage> &msg) {
    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err != OK) {
        return err;
    }
    if (!response->findInt32("err", &err)) {
        err = OK;
    }

    return err;
}

void postReplyWithError(const sp<AMessage> &msg, int32_t err) {
    sp<AReplyToken> replyID;
    msg->senderAwaitsResponse(&replyID);

    sp<AMessage> response = new AMessage;
    response->setInt32("err", err);
    response->postReply(replyID);
}

void C2RKMpiEnc::WorkHandler::setComponent(C2RKMpiEnc *thiz) {
    mThiz = thiz;
}

void C2RKMpiEnc::WorkHandler::startWork() {
    mRunning = true;
}

void C2RKMpiEnc::WorkHandler::stopWork() {
    mRunning = false;

    sp<AMessage> msg = new AMessage(WorkHandler::kWhatStop, this);
    postAndAwaitResponse(msg);
}

void C2RKMpiEnc::WorkHandler::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatDrainWork: {
            if (mRunning && mThiz) {
                int32_t err = mThiz->onDrainWork();
                if (err == C2_CORRUPTED) {
                    mRunning = false;
                    c2_err("got error, quit work looper");
                }
            } else {
                c2_trace("Ignore process message as we're not running");
            }
        } break;
        case kWhatStop: {
            mRunning = false;

            /* post response */
            postReplyWithError(msg, C2_OK);
        } break;
        default: {
            c2_err("Unrecognized msg: %d", msg->what());
        } break;
    }
}

class C2RKSessionCallbackImpl : public C2RKSessionCallback {
public:
    explicit C2RKSessionCallbackImpl(C2RKMpiEnc *thiz) : mThiz(thiz) {}
    ~C2RKSessionCallbackImpl() override = default;

    void onError(const char *error) override {
        c2_err("got session error %s", error);
    }

    void onResultReady(ImageBuffer *srcImage, void *result) override {
        mThiz->onDetectResultReady(srcImage, result);
    }

private:
    C2RKMpiEnc *mThiz;
};

C2RKMpiEnc::C2RKMpiEnc(
        const char *name,
        const char *mime,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mName(name),
      mMime(mime),
      mIntf(intfImpl),
      mDmaMem(nullptr),
      mMlvec(nullptr),
      mDump(new C2RKDump),
      mRoiCtx(nullptr),
      mMppCtx(nullptr),
      mMppMpi(nullptr),
      mMdInfo(nullptr),
      mGroup(nullptr),
      mEncCfg(nullptr),
      mCodingType(MPP_VIDEO_CodingUnused),
      mInputMppFmt(MPP_FMT_YUV420SP),
      mChipType(C2RKChipCapDef::get()->getChipType()),
      mStarted(false),
      mInputScalar(false),
      mSpsPpsHeaderReceived(false),
      mSawInputEOS(false),
      mOutputEOS(false),
      mSignalledError(false),
      mHorStride(0),
      mVerStride(0),
      mCurLayerCount(0),
      mInputCount(0),
      mRknnSession(nullptr),
      mProfile(0) {
    c2_info("[%s] version %s", name, C2_COMPONENT_FULL_VERSION);
    mCodingType = (MppCodingType)GetMppCodingFromComponentName(name);
    if (mCodingType == MPP_VIDEO_CodingUnused) {
        c2_err("failed to get coding from name %s", name);
    }
}

C2RKMpiEnc::~C2RKMpiEnc() {
    onRelease();

    C2RKMemTrace::get()->removeVideoNode(this);
    C2RKMemTrace::get()->dumpAllNode();
}

c2_status_t C2RKMpiEnc::setupAndStartLooper() {
    status_t err = OK;
    if (mLooper == nullptr) {
        status_t err = OK;
        mLooper = new ALooper;
        mHandler = new WorkHandler;
        mHandler->setComponent(this);

        mLooper->setName("C2EncLooper");
        err = mLooper->start();
        if (err == OK) {
            mLooper->registerHandler(mHandler);
        }
    }
    return (c2_status_t)err;
}

c2_status_t C2RKMpiEnc::stopAndReleaseLooper() {
    if (mLooper != nullptr) {
        if (mHandler != nullptr) {
            mHandler->stopWork();
            mLooper->unregisterHandler(mHandler->id());
            mHandler.clear();
        }
        mLooper->stop();
        mLooper.clear();
    }
    return C2_OK;
}

c2_status_t C2RKMpiEnc::onInit() {
    c2_log_func_enter();

    C2RKMemTrace::C2NodeInfo node {
        .client = this,
        .name   = mName,
        .mime   = mMime,
        .type   = C2RKMemTrace::C2_TRACE_ENCODER,
        .width  = mIntf->getSize_l()->width,
        .height = mIntf->getSize_l()->height,
        .frameRate = mIntf->getFrameRate_l()->value
    };
    if (!C2RKMemTrace::get()->tryAddVideoNode(node)) {
        C2RKMemTrace::get()->dumpAllNode();
        return C2_NO_MEMORY;
    }

    if (C2RKPropsDef::getEncAsncOutputMode()) {
        c2_info("use async output mode");
        c2_status_t err = setupAndStartLooper();
        if (err != C2_OK) {
            c2_err("failed to start looper, fallback sync output");
        }
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::onStop() {
    c2_log_func_enter();
    return C2_OK;
}

void C2RKMpiEnc::onReset() {
    c2_log_func_enter();
}

void C2RKMpiEnc::onRelease() {
    if (!mStarted)
        return;

    c2_log_func_enter();

    /* set flushing state to discard all work output */
    setFlushingState();

    if (mMppMpi) {
        mMppMpi->reset(mMppCtx);
    }

    if (mHandler) {
        stopAndReleaseLooper();
    }

    if (mRknnSession) {
        delete mRknnSession;
        mRknnSession = nullptr;
    }

    if (mBlockPool) {
        mBlockPool.reset();
    }

    if (mMdInfo) {
        mpp_buffer_put(mMdInfo);
        mMdInfo = nullptr;
    }

    if (mGroup != nullptr) {
        mpp_buffer_group_put(mGroup);
        mGroup = nullptr;
    }

    if (mEncCfg) {
        mpp_enc_cfg_deinit(mEncCfg);
        mEncCfg = nullptr;
    }

    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    if (mDmaMem != nullptr) {
        GraphicBufferAllocator::get().free((buffer_handle_t)mDmaMem->handler);
        free(mDmaMem);
        mDmaMem = nullptr;
    }

    if (mMlvec != nullptr) {
        delete mMlvec;
        mMlvec = nullptr;
    }

    if (mRoiCtx != nullptr) {
        mpp_enc_roi_deinit(mRoiCtx);
        mRoiCtx = nullptr;
    }

    if (mDump != nullptr) {
        delete mDump;
        mDump = nullptr;
    }

    stopFlushingState();

    mStarted = false;
    mInputScalar = false;
    mSpsPpsHeaderReceived = false;
    mSawInputEOS = false;
    mOutputEOS = false;
    mSignalledError = false;
}

c2_status_t C2RKMpiEnc::onFlush_sm() {
    c2_log_func_enter();
    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupBaseCodec() {
    /* default stride */
    mHorStride = C2_ALIGN(mSize->width, 16);
    if (mCodingType == MPP_VIDEO_CodingVP8) {
        mVerStride = C2_ALIGN(mSize->height, 16);
    } else {
        mVerStride = C2_ALIGN(mSize->height, 8);
    }

    c2_info("setupBaseCodec: coding %s w %d h %d hor %d ver %d",
            toStr_Coding(mCodingType), mSize->width, mSize->height, mHorStride, mVerStride);

    mpp_enc_cfg_set_s32(mEncCfg, "codec:type", mCodingType);
    mpp_enc_cfg_set_s32(mEncCfg, "vp8:disable_ivf", 1);

    mpp_enc_cfg_set_s32(mEncCfg, "prep:width", mSize->width);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:height", mSize->height);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:ver_stride", mVerStride);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:format", MPP_FMT_YUV420SP);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupInputScalar() {
    IntfImpl::Lock lock = mIntf->lock();
    std::shared_ptr<C2StreamEncInputScalar::input> c2Scalar
            = mIntf->getInputScalar_l();

    if (c2Scalar && c2Scalar->width > 0 && c2Scalar->height > 0 &&
        c2Scalar->width != mSize->width && c2Scalar->height != mSize->height) {
        c2_info("setupInputScalar: get request [%d %d] -> [%d %d]",
                mSize->width, mSize->height, c2Scalar->width, c2Scalar->height);
        mSize->width  = c2Scalar->width;
        mSize->height = c2Scalar->height;
        mHorStride = C2_ALIGN(mSize->width, 16);
        if (mCodingType == MPP_VIDEO_CodingVP8) {
            mVerStride = C2_ALIGN(mSize->height, 16);
        } else {
            mVerStride = C2_ALIGN(mSize->height, 8);
        }

        mpp_enc_cfg_set_s32(mEncCfg, "prep:width", mSize->width);
        mpp_enc_cfg_set_s32(mEncCfg, "prep:height", mSize->height);
        mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride);
        mpp_enc_cfg_set_s32(mEncCfg, "prep:ver_stride", mVerStride);

        mInputScalar  = true;
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupPreProcess() {
    IntfImpl::Lock lock = mIntf->lock();
    std::shared_ptr<C2StreamRotationInfo::output> c2Rotation
            = mIntf->getRotation_l();
    std::shared_ptr<C2StreamEncPreProcess::input> c2PreProcess
            = mIntf->getPreProcess_l();

    int32_t degrees = c2Rotation->value;
    int32_t mirror = c2PreProcess->mirror;
    int32_t flip = c2PreProcess->flip;

    if (degrees > 0) {
        c2_info("setupPreProcess: rotation degrees %d", degrees);

        switch (degrees) {
        case 90:
            mpp_enc_cfg_set_s32(mEncCfg, "prep:rotation", MPP_ENC_ROT_90);
            break;
        case 180:
            mpp_enc_cfg_set_s32(mEncCfg, "prep:rotation", MPP_ENC_ROT_180);
            break;
        case 270:
            mpp_enc_cfg_set_s32(mEncCfg, "prep:rotation", MPP_ENC_ROT_270);
            break;
        default:
            c2_warn("We only support 0,90,180,270 degree rotation");
            break;
        }
    }

    if (mirror > 0) {
        c2_info("setupPreProcess: mirroring");
        mpp_enc_cfg_set_s32(mEncCfg, "prep:mirroring", 1);
    }
    if (flip > 0) {
        c2_info("setupPreProcess: flip");
        mpp_enc_cfg_set_s32(mEncCfg, "prep:flip", 1);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupSuperProcess() {
    IntfImpl::Lock lock = mIntf->lock();
    std::shared_ptr<C2StreamEncSuperProcess::input> c2SuperProcess
            = mIntf->getSuperProcess_l();

    /*
     * Large frame process of encoder
     *
     * Mode: 0 - close default
     *       1 - drop large frame
     *       2 - reenc large frame
     * iThd: threshold of large frame of I frame, unit kbps.
     * pThd: threshold of large frame of P frame, unit kbps.
     * maxReencTime: valid when mode is 2, the maximum times of reenc.
     */
    int32_t mode = c2SuperProcess->mode;
    int32_t iThd = c2SuperProcess->iThd;
    int32_t pThd = c2SuperProcess->pThd;
    int32_t reencTimes = c2SuperProcess->reencTimes;

    if (mode > 0 && iThd > 0 && pThd > 0) {
        mpp_enc_cfg_set_s32(mEncCfg, "rc:super_mode",  mode);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:super_i_thd", iThd);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:super_p_thd", pThd);

        if (reencTimes > 0) {
            mpp_enc_cfg_set_s32(mEncCfg, "rc:max_reenc_times", reencTimes);
        }

        c2_info("setupSuperProcess, mode %d iThd %d pThd %d reencTimes %d",
                mode, iThd, pThd, reencTimes);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupSceneMode() {
    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamEncSceneModeInfo::input> c2Mode = mIntf->getSceneMode_l();

    c2_info("setupSceneMode: scene-mode %d", c2Mode->value);

    /*
     * scene-mode of encoder, this feature only support on rk3588
     *   - 0: deault none ipc mode
     *   - 1: ipc mode
     */
    mpp_enc_cfg_set_s32(mEncCfg, "tune:scene_mode", c2Mode->value);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupSliceSize() {
    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamEncSliceSizeInfo::input> c2Size = mIntf->getSliceSize_l();

    if (c2Size->value > 0) {
        c2_info("setupSliceSize: slice-size %d", c2Size->value);
        mpp_enc_cfg_set_s32(mEncCfg, "split:mode", MPP_ENC_SPLIT_BY_BYTE);
        mpp_enc_cfg_set_s32(mEncCfg, "split:arg", c2Size->value);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupFrameRate() {
    float frameRate = 0.0f;
    uint32_t idrInterval = 0;
    int32_t gop = 0;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamGopTuning::output> c2Gop = mIntf->getGop_l();
    std::shared_ptr<C2StreamFrameRateInfo::output> c2FrameRate
            = mIntf->getFrameRate_l();

    idrInterval = mIntf->getSyncFramePeriod_l();
    frameRate = c2FrameRate->value;

    if (frameRate == 1) {
        // set default frameRate 30
        frameRate = 30;
    }

    if (c2Gop && c2Gop->flexCount() > 0) {
        uint32_t syncInterval = 30;
        uint32_t iInterval = 0;
        uint32_t maxBframes = 0;

        ParseGop(*c2Gop, &syncInterval, &iInterval, &maxBframes);
        if (syncInterval > 0) {
            c2_info("updating IDR interval: %d -> %d", idrInterval, syncInterval);
            idrInterval = syncInterval;
        }
    }

    c2_info("setupFrameRate: framerate %.2f gop %u", frameRate, idrInterval);

    gop = (idrInterval < 0xFFFFFF) ? idrInterval : 0;
    if (gop == 0) {
        // disable IDR encoding when fps changed
        mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_chg_no_idr", 1);
    }

    mpp_enc_cfg_set_s32(mEncCfg, "rc:gop", gop);

    /* fix input / output frame rate */
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_num", frameRate);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_num", frameRate);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_denorm", 1);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupBitRate() {
    uint32_t bitrate = 0;
    uint32_t bitrateMode = 0;
    uint32_t bpsTarget = 0, bpsMax = 0, bpsMin = 0;

    /* valid bps range from 1K~200M */
    static uint32_t gMinEncBps = 1024 + 1;
    static uint32_t gMaxEncBps = 200 * 1024 * 1024 - 1;

    IntfImpl::Lock lock = mIntf->lock();

    bitrate = mIntf->getBitrate_l()->value;
    bitrateMode = mIntf->getBitrateMode_l();

    switch (bitrateMode) {
    case MPP_ENC_RC_MODE_CBR: {
        /* CBR mode has narrow bound */
        bpsMax = bitrate * 17 / 16;
        bpsMin = bitrate * 15 / 16;
    } break;
    case MPP_ENC_RC_MODE_VBR: {
        /* VBR mode has wide bound */
        bpsMax = bitrate * 17 / 16;
        bpsMin = bitrate * 1 / 16;
    } break;
    case MPP_ENC_RC_MODE_FIXQP:
    default: {
        /* default use CBR mode */
        bpsMax = bitrate * 17 / 16;
        bpsMin = bitrate * 15 / 16;
    } break;
    }

    bpsTarget = std::clamp(bitrate, gMinEncBps, gMaxEncBps);
    bpsMax = std::clamp(bpsMax, bpsTarget, gMaxEncBps);
    bpsMin = std::clamp(bpsMin, gMinEncBps, bpsTarget);

    c2_info("setupBitRate: mode %s bps %d range [%d:%d:%d]",
            toStr_BitrateMode(bitrateMode), bitrate, bpsMin, bpsTarget, bpsMax);

    mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_target", bpsTarget);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bpsMax);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bpsMin);

    if (!mRknnSession) {
        mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", bitrateMode);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupProfileParams() {
    uint32_t profile, level;

    IntfImpl::Lock lock = mIntf->lock();

    profile = mIntf->getProfile_l(mCodingType);
    level = mIntf->getLevel_l(mCodingType);

    c2_info("setupProfileParams: profile %s level %s",
            toStr_Profile(profile, mCodingType), toStr_Level(level, mCodingType));

    switch (mCodingType) {
    case MPP_VIDEO_CodingAVC : {
        mpp_enc_cfg_set_s32(mEncCfg, "h264:profile", profile);
        mpp_enc_cfg_set_s32(mEncCfg, "h264:level", level);
        if (profile >= MPP_H264_HIGH) {
            mpp_enc_cfg_set_s32(mEncCfg, "h264:cabac_en", 1);
            mpp_enc_cfg_set_s32(mEncCfg, "h264:cabac_idc", 0);
            mpp_enc_cfg_set_s32(mEncCfg, "h264:trans8x8", 1);
        }
    } break;
    case MPP_VIDEO_CodingHEVC : {
        mpp_enc_cfg_set_s32(mEncCfg, "h265:profile", profile);
        mpp_enc_cfg_set_s32(mEncCfg, "h265:level", level);
    } break;
    default : {
        c2_err("setupProfileParams: unsupport coding type %d", mCodingType);
    } break;
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupQp() {
    int32_t defaultIMin = 0, defaultIMax = 0;
    int32_t defaultPMin = 0, defaultPMax = 0;
    int32_t qpInit = -1;

    if (mCodingType == MPP_VIDEO_CodingVP8) {
        defaultIMin = defaultPMin = 0;
        defaultIMax = defaultPMax = 127;
    } else {
        /* the quality of h264/265 range from 1~51 */
        defaultIMin = defaultPMin = 1;
        defaultIMax = defaultPMax = 51;
    }

    int32_t iMin = defaultIMin, iMax = defaultIMax;
    int32_t pMin = defaultPMin, pMax = defaultPMax;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamPictureQuantizationTuning::output> qp =
            mIntf->getPictureQuantization_l();

    if (!qp->flexCount()) {
        int32_t rcMode = mIntf->getBitrateMode_l();
        if (rcMode == MPP_ENC_RC_MODE_FIXQP) {
            /* use const qp for p-frame in FIXQP mode */
            c2_info("setupQp: raise qp quality in fixQpMode");
            pMax = pMin = 10;
        } else if (rcMode == MPP_ENC_RC_MODE_VBR) {
            std::shared_ptr<C2EncodingQualityLevel> minQuality = mIntf->getQualityLevel_l();
            // Encoding quality level signaling, indicate that the codec is to apply
            // a minimum quality bar.
            // "S_HANDHELD" corresponds to VMAF=70.
            if (minQuality->value == C2PlatformConfig::encoding_quality_level_t::S_HANDHELD) {
                c2_info("setupQp: minquality request, force fqp range VMAF=70");
                iMin = pMin = 1;
                if (mCodingType == MPP_VIDEO_CodingVP8) {
                    iMax = pMax = 90;
                } else {
                    iMax = pMax = 35;
                }
            }
        }
        // better quality at low resolutions
        if (mSize->width * mSize->height <= (320 * 240)) {
            iMin = pMin = 1;
            iMax = pMax = 40;
        }
    }

    for (size_t i = 0; i < qp->flexCount(); ++i) {
        const C2PictureQuantizationStruct &layer = qp->m.values[i];

        if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
            iMax = layer.max;
            iMin = layer.min;
            c2_info("PictureQuanlitySetter: iMin %d iMax %d", iMin, iMax);
        } else if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
            pMax = layer.max;
            pMin = layer.min;
            c2_info("PictureQuanlitySetter: pMin %d pMax %d", pMin, pMax);
        }
    }

    iMax = std::clamp(iMax, defaultIMin, defaultIMax);
    iMin = std::clamp(iMin, defaultIMin, defaultIMax);
    pMax = std::clamp(pMax, defaultPMin, defaultPMax);
    pMin = std::clamp(pMin, defaultPMin, defaultPMax);

    c2_info("setupQp: qpInit %d i %d-%d p %d-%d", qpInit, iMin, iMax, pMin, pMax);

    switch (mCodingType) {
    case MPP_VIDEO_CodingAVC:
        mpp_enc_cfg_set_s32(mEncCfg, "h264:cb_qp_offset", 0);
        mpp_enc_cfg_set_s32(mEncCfg, "h264:cr_qp_offset", 0);
        [[fallthrough]];
    case MPP_VIDEO_CodingHEVC: {
        /*
         * disable mb_rc for vepu, this cfg does not apply to rkvenc.
         * since the vepu has pool performance, mb_rc will cause mosaic.
         */
        // mpp_enc_cfg_set_s32(mEncCfg, "hw:mb_rc_disable", 1);

        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min", pMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max", pMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min_i", iMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max_i", iMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_init", qpInit);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_ip", 2);
    } break;
    case MPP_VIDEO_CodingVP8: {
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min", pMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max", pMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min_i", iMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max_i", iMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_init", qpInit);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_ip", 6);
    } break;
    default: {
        c2_err("setupQp: unsupport coding type %d", mCodingType);
        break;
    }
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupVuiParams() {
    ColorAspects sfAspects;
    int32_t primaries, transfer, matrixCoeffs;
    bool range;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamColorAspectsInfo::output> colorAspects
            = mIntf->getCodedColorAspects_l();

    if (!C2Mapper::map(colorAspects->primaries, &sfAspects.mPrimaries)) {
        sfAspects.mPrimaries = android::ColorAspects::PrimariesUnspecified;
    }
    if (!C2Mapper::map(colorAspects->range, &sfAspects.mRange)) {
        sfAspects.mRange = android::ColorAspects::RangeUnspecified;
    }
    if (!C2Mapper::map(colorAspects->matrix, &sfAspects.mMatrixCoeffs)) {
        sfAspects.mMatrixCoeffs = android::ColorAspects::MatrixUnspecified;
    }
    if (!C2Mapper::map(colorAspects->transfer, &sfAspects.mTransfer)) {
        sfAspects.mTransfer = android::ColorAspects::TransferUnspecified;
    }

    ColorUtils::convertCodecColorAspectsToIsoAspects(
            sfAspects, &primaries, &transfer,
            &matrixCoeffs, &range);

    c2_info("setupVuiParams: (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s))",
            sfAspects.mRange, asString(sfAspects.mRange),
            sfAspects.mPrimaries, asString(sfAspects.mPrimaries),
            sfAspects.mMatrixCoeffs, asString(sfAspects.mMatrixCoeffs),
            sfAspects.mTransfer, asString(sfAspects.mTransfer));

    mpp_enc_cfg_set_s32(mEncCfg, "prep:range", range ? 2 : 0);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:colorprim", primaries);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:colortrc", transfer);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:colorspace", matrixCoeffs);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupTemporalLayers() {
    int32_t layerCount = 0;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamTemporalLayeringTuning::output> layering =
            mIntf->getTemporalLayers_l();

    layerCount = layering->m.layerCount;
    if (layerCount == 0 || layerCount == 1) {
        return C2_OK;
    }

    if (layerCount < 2 || layerCount > 4) {
        c2_warn("only support tsvc layer 2 ~ 4(%d); ignored.", layerCount);
        return C2_OK;
    }

    /*
     * NOTE:
     * 1. not support set bLayerCount and bitrateRatios yet.
     *    - layering->m.bLayerCount
     *    - layering->m.bitrateRatios
     * 2. only support tsvc layer 2 ~ 4.
     */

    MPP_RET err = MPP_OK;
    MppEncRefCfg ref;
    MppEncRefLtFrmCfg ltRef[4];
    MppEncRefStFrmCfg stRef[16];
    RK_S32 ltCnt = 0;
    RK_S32 stCnt = 0;

    memset(&ltRef, 0, sizeof(ltRef));
    memset(&stRef, 0, sizeof(stRef));

    mpp_enc_ref_cfg_init(&ref);

    c2_info("setupTemporalLayers: layers %d", layerCount);

    switch (layerCount) {
    case 4: {
        // tsvc4
        //      /-> P1      /-> P3        /-> P5      /-> P7
        //     /           /             /           /
        //    //--------> P2            //--------> P6
        //   //                        //
        //  ///---------------------> P4
        // ///
        // P0 ------------------------------------------------> P8
        ltCnt = 1;

        /* set 8 frame lt-ref gap */
        ltRef[0].lt_idx        = 0;
        ltRef[0].temporal_id   = 0;
        ltRef[0].ref_mode      = REF_TO_PREV_LT_REF;
        ltRef[0].lt_gap        = 8;
        ltRef[0].lt_delay      = 0;

        stCnt = 9;
        /* set tsvc4 st-ref struct */
        /* st 0 layer 0 - ref */
        stRef[0].is_non_ref    = 0;
        stRef[0].temporal_id   = 0;
        stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[0].ref_arg       = 0;
        stRef[0].repeat        = 0;
        /* st 1 layer 3 - non-ref */
        stRef[1].is_non_ref    = 1;
        stRef[1].temporal_id   = 3;
        stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[1].ref_arg       = 0;
        stRef[1].repeat        = 0;
        /* st 2 layer 2 - ref */
        stRef[2].is_non_ref    = 0;
        stRef[2].temporal_id   = 2;
        stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[2].ref_arg       = 0;
        stRef[2].repeat        = 0;
        /* st 3 layer 3 - non-ref */
        stRef[3].is_non_ref    = 1;
        stRef[3].temporal_id   = 3;
        stRef[3].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[3].ref_arg       = 0;
        stRef[3].repeat        = 0;
        /* st 4 layer 1 - ref */
        stRef[4].is_non_ref    = 0;
        stRef[4].temporal_id   = 1;
        stRef[4].ref_mode      = REF_TO_PREV_LT_REF;
        stRef[4].ref_arg       = 0;
        stRef[4].repeat        = 0;
        /* st 5 layer 3 - non-ref */
        stRef[5].is_non_ref    = 1;
        stRef[5].temporal_id   = 3;
        stRef[5].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[5].ref_arg       = 0;
        stRef[5].repeat        = 0;
        /* st 6 layer 2 - ref */
        stRef[6].is_non_ref    = 0;
        stRef[6].temporal_id   = 2;
        stRef[6].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[6].ref_arg       = 0;
        stRef[6].repeat        = 0;
        /* st 7 layer 3 - non-ref */
        stRef[7].is_non_ref    = 1;
        stRef[7].temporal_id   = 3;
        stRef[7].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[7].ref_arg       = 0;
        stRef[7].repeat        = 0;
        /* st 8 layer 0 - ref */
        stRef[8].is_non_ref    = 0;
        stRef[8].temporal_id   = 0;
        stRef[8].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[8].ref_arg       = 0;
        stRef[8].repeat        = 0;
    } break;
    case 3: {
        // tsvc3
        //     /-> P1      /-> P3
        //    /           /
        //   //--------> P2
        //  //
        // P0/---------------------> P4
        ltCnt = 0;

        stCnt = 5;
        /* set tsvc4 st-ref struct */
        /* st 0 layer 0 - ref */
        stRef[0].is_non_ref    = 0;
        stRef[0].temporal_id   = 0;
        stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[0].ref_arg       = 0;
        stRef[0].repeat        = 0;
        /* st 1 layer 2 - non-ref */
        stRef[1].is_non_ref    = 1;
        stRef[1].temporal_id   = 2;
        stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[1].ref_arg       = 0;
        stRef[1].repeat        = 0;
        /* st 2 layer 1 - ref */
        stRef[2].is_non_ref    = 0;
        stRef[2].temporal_id   = 1;
        stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[2].ref_arg       = 0;
        stRef[2].repeat        = 0;
        /* st 3 layer 2 - non-ref */
        stRef[3].is_non_ref    = 1;
        stRef[3].temporal_id   = 2;
        stRef[3].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[3].ref_arg       = 0;
        stRef[3].repeat        = 0;
        /* st 4 layer 0 - ref */
        stRef[4].is_non_ref    = 0;
        stRef[4].temporal_id   = 0;
        stRef[4].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[4].ref_arg       = 0;
        stRef[4].repeat        = 0;
    } break;
    case 2: {
        // tsvc2
        //   /-> P1
        //  /
        // P0--------> P2
        ltCnt = 0;

        stCnt = 3;
        /* set tsvc4 st-ref struct */
        /* st 0 layer 0 - ref */
        stRef[0].is_non_ref    = 0;
        stRef[0].temporal_id   = 0;
        stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[0].ref_arg       = 0;
        stRef[0].repeat        = 0;
        /* st 1 layer 2 - non-ref */
        stRef[1].is_non_ref    = 1;
        stRef[1].temporal_id   = 1;
        stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[1].ref_arg       = 0;
        stRef[1].repeat        = 0;
        /* st 2 layer 1 - ref */
        stRef[2].is_non_ref    = 0;
        stRef[2].temporal_id   = 0;
        stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[2].ref_arg       = 0;
        stRef[2].repeat        = 0;
    } break;
    default : {
    } break;
    }

    if (ltCnt || stCnt) {
        mpp_enc_ref_cfg_set_cfg_cnt(ref, ltCnt, stCnt);

        if (ltCnt)
            mpp_enc_ref_cfg_add_lt_cfg(ref, ltCnt, ltRef);

        if (stCnt)
            mpp_enc_ref_cfg_add_st_cfg(ref, stCnt, stRef);

        /* check and get dpb size */
        mpp_enc_ref_cfg_check(ref);
    }

    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_REF_CFG, ref);
    if (err != MPP_OK) {
        c2_err("setupTemporalLayers: failed to set ref cfg, err %d", err);
        return C2_CORRUPTED;
    }

    mCurLayerCount = layerCount;

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupPrependHeaderSetting() {
    MPP_RET err = MPP_OK;
    MppEncHeaderMode mode = MPP_ENC_HEADER_MODE_DEFAULT;
    std::shared_ptr<C2PrependHeaderModeSetting> prepend;

    IntfImpl::Lock lock = mIntf->lock();

    prepend = mIntf->getPrependHeaderMode_l();

    if (prepend->value == C2Config::PREPEND_HEADER_TO_ALL_SYNC) {
        c2_info("setupPrependHeaderSetting: prepend sps pps to idr frames.");
        mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    }

    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_HEADER_MODE, &mode);
    if (err != MPP_OK) {
        c2_err("setupPrependHeaderSetting: failed to set mode, err %d", err);
        return C2_CORRUPTED;
    } else if (mode == MPP_ENC_HEADER_MODE_EACH_IDR) {
        // disable csd to avoid duplicated sps/pps in stream header
        mSpsPpsHeaderReceived = true;
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupSuperModeIfNeeded() {
    IntfImpl::Lock lock = mIntf->lock();
    std::shared_ptr<C2StreamEncSEModeSetting::input> settings
            = mIntf->getSuperEncodingSettings_l();

    int32_t superMode = settings->mode;
    // only valid in super mode v3.0
    // bgDeltaQp: delta qp of background
    // fgDeltaQp: delta qp of foreground
    // mapMinQp:  the min qp of that can be set
    // mapMaxQp:  the max qp of that can be set
    int32_t bgDeltaQp = settings->bgDeltaQp;
    int32_t fgDeltaQp = settings->fgDeltaQp;
    int32_t mapMinQp  = settings->mapMinQp;
    int32_t mapMaxQp  = settings->mapMaxQp;

    if (superMode <= 0 || superMode >= C2_SUPER_MODE_BUTT) {
        superMode = property_get_int32("codec2_enc_super_mode", 0);
        if (superMode) {
            c2_info("config super mode, property %d", superMode);
        } else {
            return C2_OK;
        }
    }

    if (mChipType != RK_CHIP_3588 && mChipType != RK_CHIP_3576) {
        c2_warn("only RK3576/RK3588 support super encoding mode");
        return C2_OK;
    }

    static int32_t aqThdSmart[16] = {
        0,  0,  0,  0,  3,  3,  5,  5,
        8,  8,  8, 15, 15, 20, 25, 28
    };

    static int32_t aqStepSmart[16] = {
        -8, -7, -6, -5, -4, -3, -2, -1,
        0,  1,  2,  3,  4,  6,  8, 10
    };

    bool isV3Mode = (superMode == C2_SUPER_MODE_V3_QUALITY_FIRST ||
                     superMode == C2_SUPER_MODE_V3_COMPRESS_FIRST);
    bool compressFirst = (superMode == C2_SUPER_MODE_V1_COMPRESS_FIRST) ||
                         (superMode == C2_SUPER_MODE_V3_COMPRESS_FIRST);

    if (isV3Mode && !mRknnSession) {
        mRknnSession = new C2RKYolov5Session();
        bool isHEVC = (mCodingType == MPP_VIDEO_CodingHEVC);
        if (!mRknnSession->createSession(
                    std::make_shared<C2RKSessionCallbackImpl>(this), isHEVC)) {
            c2_err("failed to create rknn session, fallback..");
            delete mRknnSession;
            mRknnSession = nullptr;
            return C2_NO_INIT;
        }
        if (!mRknnSession->isMaskResultType()) {
            return C2_OK;
        }
    }

    RcApiBrief rcApiBrief;
    MPP_RET err = mMppMpi->control(mMppCtx, MPP_ENC_GET_RC_API_CURRENT, &rcApiBrief);
    if (err != MPP_OK) {
        c2_warn("setupSuperMode: failed to get rcApi, err %d", err);
        return C2_OK;
    }

    rcApiBrief.name = "smart";
    rcApiBrief.type = mCodingType;
    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_RC_API_CURRENT, &rcApiBrief);
    if (err != MPP_OK) {
        c2_warn("setupSuperMode: failed to set rcApi, err %d", err);
        return C2_OK;
    }

    mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", isV3Mode ? 5 : 4);
    mpp_enc_cfg_set_u32(mEncCfg, "rc:max_reenc_times", 0);
    mpp_enc_cfg_set_u32(mEncCfg, "rc:super_mode", 0);
    mpp_enc_cfg_set_s32(mEncCfg, "hw:qbias_i", 200);
    mpp_enc_cfg_set_s32(mEncCfg, "hw:qbias_p", 100);

    mpp_enc_cfg_set_s32(mEncCfg, "tune:deblur_en",   1);
    mpp_enc_cfg_set_s32(mEncCfg, "tune:deblur_str",  3);
    mpp_enc_cfg_set_s32(mEncCfg, "tune:lgt_chg_lvl", 0);

    mpp_enc_cfg_set_st(mEncCfg,  "hw:aq_thrd_i", aqThdSmart);
    mpp_enc_cfg_set_st(mEncCfg,  "hw:aq_thrd_p", aqThdSmart);
    mpp_enc_cfg_set_st(mEncCfg,  "hw:aq_step_i", aqStepSmart);
    mpp_enc_cfg_set_st(mEncCfg,  "hw:aq_step_p", aqStepSmart);
    // default ipc mode
    mpp_enc_cfg_set_s32(mEncCfg, "tune:scene_mode", 1);

    mpp_enc_cfg_set_s32(mEncCfg, "rc:fqp_min_i", 10);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fqp_min_p", 10);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fqp_max_p", 42);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fqp_max_i", 42);

    if (isV3Mode) {
        if (bgDeltaQp == 0) {
            bgDeltaQp = property_get_int32("codec2_enc_super_bg_delta_qp", 0);
        }
        if (fgDeltaQp == 0) {
            fgDeltaQp = property_get_int32("codec2_enc_super_fg_delta_qp", 0);
        }
        if (mapMinQp == 0) {
            mapMinQp = property_get_int32("codec2_enc_super_map_min_qp", 0);
        }
        if (mapMaxQp == 0) {
            mapMaxQp = property_get_int32("codec2_enc_super_map_max_qp", 0);
        }

        // set default config settings
        if (bgDeltaQp == 0)  bgDeltaQp = -8;
        if (fgDeltaQp == 0)  fgDeltaQp = 6;
        if (mapMinQp == 0)   mapMinQp  = 10;
        if (mapMaxQp == 0)   mapMaxQp  = 42;

        c2_info("setupSuperMode: bgDeltaQp %d fgDeltaQp %d mapMinQp %d mapMaxQp %d",
                bgDeltaQp, fgDeltaQp, mapMinQp, mapMaxQp);

        // 0:balance  1:quality_first  2:bitrate_first 3:external_se_mode
        mpp_enc_cfg_set_s32(mEncCfg, "tune:se_mode", 3);

        mpp_enc_cfg_set_s32(mEncCfg, "tune:bg_delta_qp_i",  bgDeltaQp);
        mpp_enc_cfg_set_s32(mEncCfg, "tune:bg_delta_qp_p",  bgDeltaQp);
        mpp_enc_cfg_set_s32(mEncCfg, "tune:fg_delta_qp_i",  fgDeltaQp);
        mpp_enc_cfg_set_s32(mEncCfg, "tune:fg_delta_qp_p",  fgDeltaQp);

        mpp_enc_cfg_set_s32(mEncCfg, "tune:bmap_qpmin_i",   mapMinQp);
        mpp_enc_cfg_set_s32(mEncCfg, "tune:bmap_qpmin_p",   mapMinQp);
        mpp_enc_cfg_set_s32(mEncCfg, "tune:bmap_qpmax_i",   mapMaxQp);
        mpp_enc_cfg_set_s32(mEncCfg, "tune:bmap_qpmax_p",   mapMaxQp);
    }  else {
        // smart v1 mode
        if (compressFirst) {
            uint32_t bitrate = mIntf->getBitrate_l()->value;

            mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_target", bitrate * 13 / 10);
            mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max",    bitrate * 13 / 10);
            mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min",    bitrate * 13 / 10 / 2);
        }
    }

    c2_info("setupSuperMode: setup super mode %d", superMode);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupMlvecIfNeeded() {
    int32_t layerCount = 0;
    int32_t spacing = 0;
    int32_t numLTRFrms = 0;
    int32_t inputCtlMode = 0;
    uint32_t sarWidth = 0, sarHeight = 0;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<MlvecParams> params = mIntf->getMlvecParams_l();
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> layering =
            mIntf->getTemporalLayers_l();

    layerCount = layering->m.layerCount;

    spacing      = params->sliceSpacing->spacing;
    numLTRFrms   = params->numLTRFrms->num;
    sarWidth     = params->sarSize->width;
    sarHeight    = params->sarSize->height;
    inputCtlMode = params->inputQueueCtl->enable;

    /* enable mlvec */
    if (spacing > 0 || numLTRFrms > 0 || sarWidth > 0 ||
        sarHeight > 0 || inputCtlMode > 0) {
        C2RKMlvecLegacy::MStaticCfg stCfg;

        if (numLTRFrms > MLVEC_MAX_LTR_FRAMES_COUNT) {
            c2_warn("not support LTRFrames num %d(max %d), quit mlvec mode",
                    numLTRFrms, MLVEC_MAX_LTR_FRAMES_COUNT);
            return C2_CANNOT_DO;
        }

        if (sarWidth > mSize->width || sarHeight > mSize->height) {
            c2_warn("not support sarSize %dx%d, picture size %dx%d, quit mlvec mode",
                    sarWidth, sarHeight, mSize->width, mSize->height);
            return C2_CANNOT_DO;
        }

        c2_info("setupMlvec: layerCount %d spacing %d numLTRFrms %d",
                layerCount, spacing, numLTRFrms);
        c2_info("setupMlvec: w %d h %d sarWidth %d sarHeight %d",
                mSize->width, mSize->height, sarWidth, sarHeight);
        c2_info("setupMlvec: inputCtlMode %d", inputCtlMode);

        mMlvec = new C2RKMlvecLegacy(mMppCtx, mMppMpi, mEncCfg);

        memset(&stCfg, 0, sizeof(stCfg));

        stCfg.magic = ((int32_t)'M') << 24;
        stCfg.magic |= ((int32_t)'0') << 16;
        stCfg.width  = mSize->width;
        stCfg.height = mSize->height;
        stCfg.sarWidth  = sarWidth;
        stCfg.sarHeight = sarHeight;
        stCfg.maxTid = layerCount;
        stCfg.ltrFrames = numLTRFrms;
        stCfg.addPrefix = (layerCount >= 1) ? 1 : 0;
        stCfg.sliceMbs = spacing;

        if (!mMlvec->setupStaticConfig(&stCfg)) {
            c2_err("failed to setup mlvec static config");
        } else {
            mCurLayerCount = layerCount;
        }

        // mlvec need pic_order_cnt_type equal to 2
        mpp_enc_cfg_set_s32(mEncCfg, "h264:poc_type", 2);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupEncCfg() {
    MPP_RET err = MPP_OK;

    err = mpp_enc_cfg_init(&mEncCfg);
    if (err != MPP_OK) {
        c2_err("failed to get enc_cfg, err %d", err);
        return C2_CORRUPTED;
    }

    err = mMppMpi->control(mMppCtx, MPP_ENC_GET_CFG, mEncCfg);
    if (err != MPP_OK) {
        c2_err("failed to get codec cfg, err %d", err);
        return C2_CORRUPTED;
    }

    /* Video control Set Base Codec */
    setupBaseCodec();

    /* Video control Ser input scaler */
    setupInputScalar();

    /* Video control PreProcess, rotation\mirror\flip */
    setupPreProcess();

    /* Video Large Frame Process, drop or reenc */
    setupSuperProcess();

    /* Video control Set Scene Mode */
    setupSceneMode();

    /* Video control Set Slice Size */
    setupSliceSize();

    /* Video control Set FrameRates and gop */
    setupFrameRate();

    /* Video control Set Bitrate */
    setupBitRate();

    /* Video control Set Profile params */
    setupProfileParams();

    /* Video control Set QP */
    setupQp();

    /* Video control Set VUI params */
    setupVuiParams();

    /* Video control Set Temporal Layers */
    setupTemporalLayers();

    /* Video control Set Prepend Header Setting */
    setupPrependHeaderSetting();

    /* Video control Set Super Encoding Mode */
    setupSuperModeIfNeeded();

    /* Video control Set MLVEC encoder */
    setupMlvecIfNeeded();

    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
    if (err != MPP_OK) {
        c2_err("failed to setup codec cfg, ret %d", err);
        return C2_CORRUPTED;
    } else {
        /* Video control Set SEI config */
        IntfImpl::Lock lock = mIntf->lock();
        MppEncSeiMode seiMode = MPP_ENC_SEI_MODE_ONE_FRAME;
        if (mIntf->getIsDisableSEI()) {
            c2_info("disable sei info output");
            seiMode = MPP_ENC_SEI_MODE_DISABLE;
        }
        // FIXME: MLVEC not support HEVC SEI parser currently
        if (mMlvec && mCodingType == MPP_VIDEO_CodingHEVC) {
            seiMode = MPP_ENC_SEI_MODE_DISABLE;
        }
        mMppMpi->control(mMppCtx, MPP_ENC_SET_SEI_CFG, &seiMode);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::initEncoder() {
    MPP_RET err = MPP_OK;

    c2_log_func_enter();

    {
        IntfImpl::Lock lock = mIntf->lock();
        mSize = mIntf->getSize_l();
        mBitrate = mIntf->getBitrate_l();
        mFrameRate = mIntf->getFrameRate_l();
        mProfile = mIntf->getProfile_l(mCodingType);
    }

    /*
     * NOTE: We need temporary buffer to store rga nv12 output for some rgba input,
     * since mpp can't process rgba input properly. in addition to this, alloc buffer
     * within 4G in view of rga efficiency.
     */
    buffer_handle_t bufferHandle;
    uint32_t stride = 0;

    uint64_t usage = (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);

    // allocate buffer within 4G to avoid rga2 error.
    if (mChipType == RK_CHIP_3588 || mChipType == RK_CHIP_356X) {
        usage = RK_GRALLOC_USAGE_WITHIN_4G;
    }

    status_t status = GraphicBufferAllocator::get().allocate(
            C2_ALIGN(mSize->width, 16), C2_ALIGN(mSize->height, 16),
            0x15 /* NV12 */, 1u /* layer count */,
            usage, &bufferHandle, &stride, "C2RKMpiEnc");
    if (status) {
        c2_err("failed transaction: allocate");
        goto error;
    }

    mDmaMem = (MyDmaBuffer_t *)malloc(sizeof(MyDmaBuffer_t));
    mDmaMem->fd = C2RKGrallocOps::get()->getShareFd(bufferHandle);
    mDmaMem->size = C2RKGrallocOps::get()->getAllocationSize(bufferHandle);
    mDmaMem->handler = (void *)bufferHandle;

    c2_info("alloc temporary DmaMem fd %d size %d", mDmaMem->fd, mDmaMem->size);

    // create mpp and init mpp
    err = mpp_create(&mMppCtx, &mMppMpi);
    if (err != MPP_OK) {
        c2_err("failed to mpp_create, err %d", err);
        goto error;
    }

    {
        // Update the block timeout settings for output.
        MppPollType timeout = MPP_POLL_BLOCK;
        err = mMppMpi->control(mMppCtx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
        if (err != MPP_OK) {
            c2_err("failed to set output timeout %d, err %d", timeout, err);
            goto error;
        }
    }

    err = mpp_init(mMppCtx, MPP_CTX_ENC, mCodingType);
    if (err != MPP_OK) {
        c2_err("failed to mpp_init, err %d", err);
        goto error;
    }

    if (setupEncCfg() != C2_OK) {
        c2_err("failed to set config");
        goto error;
    }

    err = mpp_buffer_group_get_internal(&mGroup, MPP_BUFFER_TYPE_ION);
    if (err != MPP_OK) {
        c2_err("failed to get mpp buffer group, err %d", err);
        goto error;
    }

    err = mpp_buffer_get(mGroup, &mMdInfo, mSize->width * mSize->height);
    if (err != MPP_OK) {
        c2_err("failed to get motion info buffer, err %d", err);
        goto error;
    }

    mDump->initDump(mSize->width, mSize->height, true);

    return C2_OK;

error:
    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    return C2_CORRUPTED;
}

void C2RKMpiEnc::fillEmptyWork(const std::unique_ptr<C2Work>& work) {
    uint32_t flags = 0;

    c2_trace_func_called();

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        c2_info("Signalling EOS");
    }
    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2RKMpiEnc::finishWork(
        const std::unique_ptr<C2Work> &work,
        OutWorkEntry entry) {
    c2_status_t ret = C2_OK;
    uint64_t frmIndex = 0;
    MppPacket packet = nullptr;
    std::shared_ptr<C2LinearBlock> block;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };

    frmIndex = entry.frameIndex;
    packet   = entry.outPacket;

    void   *data = mpp_packet_get_data(packet);
    size_t  len  = mpp_packet_get_length(packet);
    size_t  size = mpp_packet_get_size(packet);

    ret = mBlockPool->fetchLinearBlock(size, usage, &block);
    if (ret != C2_OK) {
        c2_err("failed to fetch block for output, ret 0x%x", ret);
        mSignalledError = true;
        return;
    }

    C2WriteView wView = block->map().get();
    if (C2_OK != wView.error()) {
        c2_err("write view map failed with status 0x%x", wView.error());
        mSignalledError = true;
        return;
    }

    // copy mpp output to c2 output
    memcpy(wView.data(), data, len);

    RK_S32 isIntra = 0;
    std::shared_ptr<C2Buffer> buffer = createLinearBuffer(block, 0, len);
    MppMeta meta = mpp_packet_get_meta(packet);
    mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &isIntra);
    if (isIntra) {
        c2_info("IDR frame produced");
        buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
                0u /* stream id */, C2Config::SYNC_FRAME));
    }

    mpp_packet_deinit(&packet);

    auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };

    if (work && c2_cntr64_t(frmIndex) == work->input.ordinal.frameIndex) {
        fillWork(work);
        if (mOutputEOS) {
            work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        }
    } else {
        // TODO: wait pendding work ready, getpacket return before process over?
        int32_t retry = 0;
        static const int32_t kMaxPendingRetryTime = 20;
        while (!isPendingFlushing() && !isPendingWorkExist(frmIndex)) {
            usleep(2 * 1000);
            if ((retry++) > kMaxPendingRetryTime) {
                c2_err("failed to wait work index %lld pendding", frmIndex);
                mSignalledError = true;
                break;
            }
        }
        finish(frmIndex, fillWork);
    }
}

c2_status_t C2RKMpiEnc::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    (void)drainMode;
    (void)pool;
    return C2_OK;
}

c2_status_t C2RKMpiEnc::onDrainWork(const std::unique_ptr<C2Work> &work) {
    if (mSignalledError) return C2_BAD_STATE;

    OutWorkEntry entry;
    memset(&entry, 0, sizeof(entry));

    c2_status_t err = getoutpacket(&entry);
    if (err == C2_OK) {
        finishWork(work, entry);
    } else if (err == C2_CORRUPTED) {
        c2_err("signalling error");
        mSignalledError = true;
    }

    return err;
}

void C2RKMpiEnc::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err = C2_OK;

    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    // Initialize encoder if not already initialized
    if (!mStarted) {
        err = initEncoder();
        if (err != C2_OK) {
            work->result = C2_BAD_VALUE;
            c2_info("failed to initialize, signalled Error");
            return;
        }
        // start output looper
        if (mHandler) {
            mHandler->startWork();
        }
        mBlockPool = pool;
        mStarted = true;
    }

    if (mSignalledError) {
        c2_info("Signalled Error");
        work->result = C2_BAD_VALUE;
        work->workletsProcessed = 1u;
        return;
    }

    uint32_t flags = work->input.flags;
    uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();
    uint64_t timestamp = work->input.ordinal.timestamp.peekll();

    std::shared_ptr<const C2GraphicView> view;
    std::shared_ptr<C2Buffer> inputBuffer = nullptr;
    if (!work->input.buffers.empty()) {
        inputBuffer = work->input.buffers[0];
        view = std::make_shared<const C2GraphicView>(
                inputBuffer->data().graphicBlocks().front().map().get());
        if (view->error() != C2_OK) {
            c2_err("graphic view map err = %d", view->error());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            work->workletsProcessed = 1u;
            return;
        }
    } else {
        c2_warn("ignore empty input with frameIndex %lld", frameIndex);
        fillEmptyWork(work);
        return;
    }

    c2_trace("process one work timestamp %llu frameindex %llu, flags %x",
             timestamp, frameIndex, flags);

    mSawInputEOS = (flags & C2FrameData::FLAG_END_OF_STREAM);

    if (!mSpsPpsHeaderReceived) {
        MppPacket hdrPkt   = nullptr;
        void     *hdrBuf   = nullptr;
        void     *data     = nullptr;
        uint32_t  hdrSize  = 1024;
        uint32_t  dataSize = 0;

        hdrBuf = malloc(hdrSize * sizeof(uint8_t));
        if (hdrBuf)
            mpp_packet_init(&hdrPkt, hdrBuf, hdrSize);

        if (hdrPkt) {
            mMppMpi->control(mMppCtx, MPP_ENC_GET_HDR_SYNC, hdrPkt);
            data = mpp_packet_get_data(hdrPkt);
            dataSize = mpp_packet_get_length(hdrPkt);
        }

        if (data) {
            std::unique_ptr<C2StreamInitDataInfo::output> csd =
                    C2StreamInitDataInfo::output::AllocUnique(dataSize, 0u);

            memcpy(csd->m.value, data, dataSize);
            work->worklets.front()->output.configUpdate.push_back(std::move(csd));

            /* dump output data if neccessary */
            mDump->recordFile(ROLE_OUTPUT, data, dataSize);

            mSpsPpsHeaderReceived = true;
        }

        if (hdrPkt) {
            mpp_packet_deinit(&hdrPkt);
            hdrPkt = nullptr;
        }
        if (hdrBuf) {
            free(hdrBuf);
            hdrBuf = nullptr;
        }

        if (work->input.buffers.empty()) {
            work->workletsProcessed = 1u;
            return;
        }
    }

    // handle common dynamic config change
    handleCommonDynamicCfg();

    MyDmaBuffer_t inDmaBuf;
    memset(&inDmaBuf, 0, sizeof(MyDmaBuffer_t));

    err = getInBufferFromWork(work, &inDmaBuf);
    if (err != C2_OK) {
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    // In smart v3 mode, handle yolov5 rknn object detection.
    // not set workletsProcessed to indicates that the current work incomplete.
    // and will finish this work later in sesssion callback.
    if (mRknnSession) {
        err = handleRknnDetection(work, inDmaBuf);
        if (err == C2_OK) {
            return;
        }
    }

    /* send frame to mpp */
    err = sendframe(inDmaBuf, frameIndex, flags);
    if (C2_OK != err) {
        c2_err("failed to enqueue frame, err %d", err);
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    // In async output mode, not set workletsProcessed to indicates that the
    // current work is not completed. find this work by frameIndex and finish
    // it later in output looper.
    if (mHandler) {
        sp<AMessage> msg = new AMessage(WorkHandler::kWhatDrainWork, mHandler);
        msg->post();
    } else {
        err = onDrainWork(work);
        if (err != C2_OK) {
            fillEmptyWork(work);
        }

        if (!mSawInputEOS && work->input.buffers.empty()) {
            fillEmptyWork(work);
        }
    }
}

c2_status_t C2RKMpiEnc::handleCommonDynamicCfg() {
    bool change = false;

    IntfImpl::Lock lock = mIntf->lock();
    std::shared_ptr<C2StreamPictureSizeInfo::input> size = mIntf->getSize_l();
    std::shared_ptr<C2StreamBitrateInfo::output> bitrate = mIntf->getBitrate_l();
    std::shared_ptr<C2StreamFrameRateInfo::output> frameRate = mIntf->getFrameRate_l();
    uint32_t profile = mIntf->getProfile_l(mCodingType);
    lock.unlock();

    // handle dynamic size config.
    if (size != mSize) {
        c2_info("new size request, w %d h %d", size->width, size->height);
        mSize = size;
        setupBaseCodec();
        change = true;
    }

    // handle dynamic bitrate config.
    if (bitrate != mBitrate) {
        c2_info("new bitrate request, value %d", bitrate->value);
        mBitrate = bitrate;
        setupBitRate();
        change = true;
    }

    // handle dynamic frameRate config.
    if (frameRate != mFrameRate) {
        c2_info("new frameRate request, value %.2f", frameRate->value);
        mFrameRate = frameRate;
        setupFrameRate();
        change = true;
    }

    // handle dynamic profile config.
    if (profile != mProfile) {
        c2_info("new profile request, value %s", toStr_Profile(profile, mCodingType));
        mProfile = profile;
        setupProfileParams();
        change = true;
    }

    if (change) {
        MPP_RET err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
        if (err != MPP_OK) {
            c2_err("failed to setup dynamic config, ret %d", err);
        }
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::handleRequestSyncFrame() {
    int32_t layerPos = 0;

    // TODO Is there a better way to count frame layer?
    if (mCurLayerCount >= 2) {
        layerPos = mInputCount % (2 << (mCurLayerCount - 2));
    }

    // only handle IDR request at layer 0
    if (layerPos == 0) {
        IntfImpl::Lock lock = mIntf->lock();
        std::shared_ptr<C2StreamRequestSyncFrameTuning::output> requestSync;
        requestSync = mIntf->getRequestSync_l();
        lock.unlock();

        // we can handle IDR immediately
        if (requestSync->value) {
            c2_info("got sync request");
            // unset request
            C2StreamRequestSyncFrameTuning::output clearSync(0u, C2_FALSE);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            mIntf->config({ &clearSync }, C2_MAY_BLOCK, &failures);
            // force set IDR frame
            mMppMpi->control(mMppCtx, MPP_ENC_SET_IDR_FRAME, nullptr);
        }
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::handleMlvecDynamicCfg(MppMeta meta) {
    int32_t layerCount = 0;
    int32_t layerPos = 0;

    if (!mMlvec) {
        return C2_OK;
    }

    IntfImpl::Lock lock = mIntf->lock();

    C2RKMlvecLegacy::MDynamicCfg cfg;
    std::shared_ptr<MlvecParams> params = mIntf->getMlvecParams_l();
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> layering =
            mIntf->getTemporalLayers_l();

    layerCount = layering->m.layerCount;

    memset(&cfg, 0, sizeof(cfg));

    /* count layer position */
    if (layerCount >= 2) {
        layerPos = mInputCount % (2 << (layerCount - 2));
        c2_trace("layer %d/%d frameNum %d", layerPos, layerCount, mInputCount);
    }

    if (layerPos == 0) {
        if (mCurLayerCount != layerCount) {
            c2_info("temporalLayers change, %d to %d", mCurLayerCount, layerCount);
            mMlvec->setupMaxTid(layerCount);
            mCurLayerCount = layerCount;
        }

        if (params->ltrMarkFrmCtl->markFrame >= 0) {
            c2_trace("ltrMarkFrm change, value %d", params->ltrMarkFrmCtl->markFrame);
            cfg.updated |= MLVEC_ENC_MARK_LTR_UPDATED;
            cfg.markLtr = params->ltrMarkFrmCtl->markFrame;
            params->ltrMarkFrmCtl->markFrame = -1;
        }

        if (params->ltrUseFrmCtl->useFrame >= 0) {
            c2_trace("ltrUseFrm change, value %d", params->ltrUseFrmCtl->useFrame);
            cfg.updated |= MLVEC_ENC_USE_LTR_UPDATED;
            cfg.useLtr = params->ltrUseFrmCtl->useFrame;
            params->ltrUseFrmCtl->useFrame = -1;
        }
    }

    if (params->frameQPCtl->value >= 0) {
        c2_trace("frameQP change, value %d", params->frameQPCtl->value);
        cfg.updated |= MLVEC_ENC_FRAME_QP_UPDATED;
        cfg.frameQP = params->frameQPCtl->value;
        params->frameQPCtl->value = -1;
    }

    if (params->baseLayerPid->value >= 0) {
        c2_trace("baseLayerPid change, value %d", params->baseLayerPid->value);
        cfg.updated |= MLVEC_ENC_BASE_PID_UPDATED;
        cfg.baseLayerPid = params->baseLayerPid->value;
        params->baseLayerPid->value = -1;
    }

    if (params->sliceSpacing->spacing >= 0) {
        c2_trace("sliceSpacing change, value %d", params->sliceSpacing->spacing);
        cfg.updated |= MLVEC_ENC_SLICE_MBS_UPDATED;
        cfg.sliceMbs = params->sliceSpacing->spacing;
        params->sliceSpacing->spacing = -1;
    }

    if (cfg.updated) {
        mMlvec->setupDynamicConfig(&cfg, meta);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::handleRoiRegionRequest(
        MppMeta meta, Vector<RoiRegionCfg> regions) {
    if (regions.size() == 0) return C2_OK;

    if (!mRoiCtx) {
        if (mpp_enc_roi_init(&mRoiCtx, mSize->width, mSize->height, mCodingType)) {
            c2_err("failed to init roi context");
            return C2_CORRUPTED;
        }
        c2_info("setup roi done, ctx %p", mRoiCtx);
    }

    for (int i = 0; i < regions.size(); i++) {
        RoiRegionCfg *region = &regions.editItemAt(i);
        if (region->x > mSize->width || region->y > mSize->height ||
            region->w > mSize->width || region->h > mSize->height ||
            (region->x + region->w) > mSize->width ||
            (region->y + region->h) > mSize->height) {
            c2_err("size limit [%d,%d] qpVal in range 1~51", mSize->width, mSize->height);
            c2_err("got invalid roi region, rect [%d,%d,%d,%d] intra %d mode %d qp %d",
                    region->x, region->y, region->w, region->h,
                    region->force_intra, region->qp_mode, region->qp_val);
        } else {
            mpp_enc_roi_add_region(mRoiCtx, region);
            c2_trace("setup roi region[%d] rect [%d,%d,%d,%d] intra %d mode %d qp %d",
                     i, region->x, region->y, region->w, region->h,
                     region->force_intra, region->qp_mode, region->qp_val);
        }
    }

    // send roi info by metadata
    mpp_enc_roi_setup_meta(mRoiCtx, meta);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::onDetectResultReady(ImageBuffer *srcImage, void *result) {
    if (!srcImage) {
        c2_trace("ignore empty detection image");
        return C2_OK;
    }

    if (isPendingFlushing()) {
        c2_trace("ignore frame output since pending flush");
        return C2_OK;
    }

    MyDmaBuffer_t inDmaBuf;
    memset(&inDmaBuf, 0, sizeof(MyDmaBuffer_t));

    inDmaBuf.fd   = srcImage->fd;
    inDmaBuf.size = srcImage->size;
    inDmaBuf.npuMaps = result;

    /* send frame to mpp */
    c2_status_t err = sendframe(inDmaBuf, srcImage->pts, srcImage->flags);
    if (err != C2_OK) {
        c2_err("failed to enqueue frame, err %d", err);
        mSignalledError = true;
        return C2_CORRUPTED;
    }

    /* get and drain output work */
    err = onDrainWork();

    return err;
}

c2_status_t C2RKMpiEnc::handleRknnDetection(
        const std::unique_ptr<C2Work> &work, MyDmaBuffer_t dbuffer) {
    if (!mRknnSession) return C2_CORRUPTED;

    // ignore empty input buffer
    if (dbuffer.fd <= 0) return C2_CORRUPTED;

    uint32_t flags = work->input.flags;
    uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();

    ImageBuffer srcImage;
    memset(&srcImage, 0, sizeof(ImageBuffer));

    srcImage.width   = mSize->width;
    srcImage.height  = mSize->height;
    srcImage.hstride = mHorStride;
    srcImage.vstride = mVerStride;
    srcImage.fd      = dbuffer.fd;
    srcImage.size    = dbuffer.size;
    srcImage.pts     = frameIndex;
    srcImage.flags   = flags;

    if (mInputMppFmt == MPP_FMT_RGBA8888) {
        srcImage.format = IMAGE_FORMAT_RGBA8888;
    } else {
        srcImage.format = IMAGE_FORMAT_YUV420SP_NV12;
    }

    if (!mRknnSession->startDetect(&srcImage)) {
        c2_err("failed to start detection");
        return C2_CORRUPTED;
    }

    return C2_OK;
}

// Note: Check if the input can be received by mpp driver directly
bool C2RKMpiEnc::needRgaConvert(uint32_t width, uint32_t height, MppFrameFormat fmt) {
    bool needsRga = true;

    if (mInputScalar) {
        needsRga = true;
        goto cleanUp;
    }

    if (fmt == MPP_FMT_RGBA8888) {
        if (!C2RKChipCapDef::get()->hasRkVenc()) {
            needsRga = true;
            goto cleanUp;
        }
    }

    if (mChipType == RK_CHIP_3588 ||
        mChipType == RK_CHIP_3562 ||
        mChipType == RK_CHIP_3576 ||
        mChipType == RK_CHIP_3528 ) {
        needsRga = (mCodingType == MPP_VIDEO_CodingVP8);
    }

    if (needsRga && C2_IS_ALIGNED(width, 16) && C2_IS_ALIGNED(height, 16)) {
        needsRga = false;
    }

cleanUp:
    if (mInputCount == 0) {
        c2_info("check: hor %d ver %d fmt %s %s extra convert",
                width, height, toStr_Format(fmt), needsRga ? "need" : "no need");
    }
    return needsRga;
}

int32_t C2RKMpiEnc::getRgaColorSpaceMode() {
    int32_t mode = RGA_COLOR_SPACE_DEFAULT;
    int32_t setRange = 0, setStandard = 0, setTransfer = 0;

    IntfImpl::Lock lock = mIntf->lock();

    ColorAspects sfAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> colorAspects
            = mIntf->getCodedColorAspects_l();

    if (!C2Mapper::map(colorAspects->primaries, &sfAspects.mPrimaries)) {
        sfAspects.mPrimaries = android::ColorAspects::PrimariesUnspecified;
    }
    if (!C2Mapper::map(colorAspects->range, &sfAspects.mRange)) {
        sfAspects.mRange = android::ColorAspects::RangeUnspecified;
    }
    if (!C2Mapper::map(colorAspects->matrix, &sfAspects.mMatrixCoeffs)) {
        sfAspects.mMatrixCoeffs = android::ColorAspects::MatrixUnspecified;
    }
    if (!C2Mapper::map(colorAspects->transfer, &sfAspects.mTransfer)) {
        sfAspects.mTransfer = android::ColorAspects::TransferUnspecified;
    }

    // aspects are normally communicated in ColorAspects
    ColorUtils::convertCodecColorAspectsToPlatformAspects(
            sfAspects, &setRange, &setStandard, &setTransfer);

    if (setStandard == ColorUtils::kColorStandardBT709) {
        mode = RGA_RGB_TO_YUV_BT709_LIMIT;
    } else if (setStandard == ColorUtils::kColorStandardBT601_625 ||
               setStandard == ColorUtils::kColorStandardBT601_525) {
        if (setRange == ColorUtils::kColorRangeFull)
            mode = RGA_RGB_TO_YUV_BT601_FULL;
        else
            mode = RGA_RGB_TO_YUV_BT601_LIMIT;
    }

    return mode;
}

c2_status_t C2RKMpiEnc::getInBufferFromWork(
        const std::unique_ptr<C2Work> &work, MyDmaBuffer_t *outBuffer) {
    c2_status_t ret = C2_OK;
    uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();
    bool configChanged = false;

    std::shared_ptr<const C2GraphicView> view;
    std::shared_ptr<C2Buffer> inputBuffer = nullptr;

    /* dump frame time consuming if neccessary */
    mDump->recordFrameTime(frameIndex);

    inputBuffer = work->input.buffers[0];
    view = std::make_shared<const C2GraphicView>(
            inputBuffer->data().graphicBlocks().front().map().get());
    const C2GraphicView* const input = view.get();
    const C2PlanarLayout& layout = input->layout();
    const C2Handle *c2Handle = inputBuffer->data().graphicBlocks().front().handle();

    uint32_t bqSlot, width, height, format, stride, generation;
    uint64_t usage, bqId;

    android::_UnwrapNativeCodec2GrallocMetadata(
            c2Handle, &width, &height, &format, &usage,
            &stride, &generation, &bqId, &bqSlot);

    // Fix error for wifidisplay when stride is 0
    if (stride == 0) {
        std::vector<ui::PlaneLayout> layouts;
        buffer_handle_t bufferHandle;
        native_handle_t *grallocHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

        GraphicBufferMapper &gm(GraphicBufferMapper::get());
        gm.importBuffer(const_cast<native_handle_t *>(grallocHandle),
                        width, height, 1, format, usage,
                        stride, &bufferHandle);
        gm.getPlaneLayouts(const_cast<native_handle_t *>(bufferHandle), &layouts);
        if (layouts[0].sampleIncrementInBits != 0) {
            stride = layouts[0].strideInBytes * 8 / layouts[0].sampleIncrementInBits;
        } else {
            c2_err("layouts[0].sampleIncrementInBits = 0");
            stride = mHorStride;
        }
        gm.freeBuffer(bufferHandle);
        native_handle_delete(grallocHandle);
    }

    c2_trace("in buffer attr. w %d h %d stride %d layout 0x%x frameIndex %lld",
             width, height, stride, layout.type, frameIndex);

    switch (layout.type) {
    case C2PlanarLayout::TYPE_RGB:
        [[fallthrough]];
    case C2PlanarLayout::TYPE_RGBA: {
        uint32_t fd = c2Handle->data[0];

        /* dump input data if neccessary */
        mDump->recordFile(ROLE_INPUT,
                (void*)input->data()[0], stride, height, MPP_FMT_RGBA8888);

        if (!needRgaConvert(stride, height, MPP_FMT_RGBA8888)) {
            if (mHorStride != stride || mVerStride != height) {
                // setup encoder using new stride config
                c2_info("cfg stride change from [%d:%d] -> [%d %d]",
                        mHorStride, mVerStride, stride, height);
                mHorStride = stride;
                mVerStride = height;
                configChanged = true;
            }

            if (mInputMppFmt != MPP_FMT_RGBA8888) {
                c2_info("update use rgba input format");
                mInputMppFmt = MPP_FMT_RGBA8888;
                configChanged = true;
            }

            outBuffer->fd = fd;
            outBuffer->size = mHorStride * mVerStride * 4;
        } else {
            RgaInfo srcInfo, dstInfo;
            // get RGA color space mode for rgba->yuv conversion
            int32_t colorSpaceMode = getRgaColorSpaceMode();

            C2RKRgaDef::SetRgaInfo(
                    &srcInfo, fd, HAL_PIXEL_FORMAT_RGBA_8888,
                    width, height, stride, height);
            C2RKRgaDef::SetRgaInfo(
                    &dstInfo, mDmaMem->fd, HAL_PIXEL_FORMAT_YCrCb_NV12,
                    mSize->width, mSize->height, mHorStride, mVerStride);
            if (!C2RKRgaDef::DoBlit(srcInfo, dstInfo, colorSpaceMode)) {
                c2_err("failed to RgaConver(RGBA->NV12)");
                ret = C2_CORRUPTED;
            }

            outBuffer->fd = mDmaMem->fd;
            outBuffer->size = mHorStride * mVerStride * 3 / 2;
        }
    } break;
    case C2PlanarLayout::TYPE_YUV: {
        uint32_t fd = c2Handle->data[0];

        /* dump input data if neccessary */
        mDump->recordFile(ROLE_INPUT,
                (void*)input->data()[0], stride, height, MPP_FMT_YUV420SP);

        if (mInputMppFmt != MPP_FMT_YUV420SP) {
            c2_info("update use yuv input format");
            mInputMppFmt = MPP_FMT_YUV420SP;
            configChanged = true;
        }

        if (!needRgaConvert(stride, height, MPP_FMT_YUV420SP)) {
            if (mHorStride != stride || mVerStride != height) {
                // setup encoder using new stride config
                c2_info("cfg stride change from [%d:%d] -> [%d %d]",
                        mHorStride, mVerStride, stride, height);
                mHorStride = stride;
                mVerStride = height;
                configChanged = true;
            }
            outBuffer->fd = fd;
            outBuffer->size = mHorStride * mVerStride * 3 / 2;
        } else {
            RgaInfo srcInfo, dstInfo;

            C2RKRgaDef::SetRgaInfo(
                    &srcInfo, fd, HAL_PIXEL_FORMAT_YCrCb_NV12,
                    width, height, stride, height);
            C2RKRgaDef::SetRgaInfo(
                    &dstInfo, mDmaMem->fd, HAL_PIXEL_FORMAT_YCrCb_NV12,
                    mSize->width, mSize->height, mHorStride, mVerStride);
            if (!C2RKRgaDef::DoBlit(srcInfo, dstInfo)) {
                c2_err("failed to RgaCrop(NV12->NV12)");
                ret = C2_CORRUPTED;
            }

            outBuffer->fd = mDmaMem->fd;
            outBuffer->size = mHorStride * mVerStride * 3 / 2;
        }
    } break;
    default:
        c2_err("Unrecognized plane type: %d", layout.type);
        ret = C2_BAD_VALUE;
    }

    if (configChanged) {
        if (mInputMppFmt == MPP_FMT_RGBA8888) {
            mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride * 4);
        } else {
            mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride);
        }
        mpp_enc_cfg_set_s32(mEncCfg, "prep:ver_stride", mVerStride);
        mpp_enc_cfg_set_s32(mEncCfg, "prep:format", mInputMppFmt);

        MPP_RET err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
        if (err != MPP_OK) {
            c2_err("failed to setup new mpp config.");
            ret = C2_CORRUPTED;
        }
    }

    return ret;
}

c2_status_t C2RKMpiEnc::sendframe(
        MyDmaBuffer_t dBuffer, uint64_t pts, uint32_t flags) {
    c2_status_t ret = C2_OK;
    MPP_RET err = MPP_OK;
    MppFrame frame = nullptr;
    MppMeta meta = nullptr;

    mpp_frame_init(&frame);

    meta = mpp_frame_get_meta(frame);

    if (flags & C2FrameData::FLAG_END_OF_STREAM) {
        c2_info("send input eos");
        mpp_frame_set_eos(frame, 1);
    }

    c2_trace("send frame fd %d size %d pts %lld", dBuffer.fd, dBuffer.size, pts);

    if (dBuffer.fd > 0) {
        MppBuffer buffer = nullptr;
        MppBufferInfo commit;

        memset(&commit, 0, sizeof(commit));

        commit.type = MPP_BUFFER_TYPE_ION;
        commit.fd = dBuffer.fd;
        commit.size = dBuffer.size;

        err = mpp_buffer_import(&buffer, &commit);
        if (err) {
            c2_err("failed to import input buffer");
            ret = C2_NOT_FOUND;
            goto error;
        }
        mpp_frame_set_buffer(frame, buffer);
        mpp_buffer_put(buffer);
        buffer = nullptr;
    } else {
        mpp_frame_set_buffer(frame, nullptr);
    }

    mpp_frame_set_width(frame, mSize->width);
    mpp_frame_set_height(frame, mSize->height);
    mpp_frame_set_ver_stride(frame, mVerStride);
    mpp_frame_set_pts(frame, pts);
    mpp_frame_set_fmt(frame, mInputMppFmt);

    switch(mInputMppFmt) {
    case MPP_FMT_RGBA8888:
        mpp_frame_set_hor_stride(frame, mHorStride * 4);
        break;
    case MPP_FMT_YUV420P:
    case MPP_FMT_YUV420SP:
        mpp_frame_set_hor_stride(frame, mHorStride);
        break;
    default:
         break;
    }

    mpp_meta_set_buffer(meta, KEY_MOTION_INFO, mMdInfo);

    /* handle dynamic configurations from teams mlvec */
    if (mMlvec) {
        handleMlvecDynamicCfg(meta);
    }

    /* handle IDR request */
    handleRequestSyncFrame();

    /* handle ROI region setup from user */
    {
        IntfImpl::Lock lock = mIntf->lock();
        Vector<RoiRegionCfg> regions = mIntf->getRoiRegionCfg();
        if (regions.size() > 0) {
            handleRoiRegionRequest(meta, regions);
        }
    }

    /* set npu detection maps */
    if (dBuffer.npuMaps) {
        /*
         * rknn detect session with two types of output:
         *
         * 1. proto mask, it requires a period of post-processing, but with more
         *    precise rate control, the sesk mask is process by mpp encoder.
         * 2. roi rect arrays, task less time and the quality of ROI regions
         *    is controlled outside. We enhance quality by reduce relative QP of
         *    ROI regions. it is a relatively rough control.
         */
        if (mRknnSession->isMaskResultType()) {
            err = mpp_meta_set_ptr(meta, KEY_NPU_OBJ_FLAG, dBuffer.npuMaps);
            if (err != MPP_OK) {
                c2_warn("failed to set rknn object, err %d", err);
            }
        } else {
            DetectRegions *dRegions = reinterpret_cast<DetectRegions*>(dBuffer.npuMaps);

            Vector<RoiRegionCfg> regions;
            int regionCount = std::clamp(dRegions->count, 0, MPP_MAX_ROI_REGION_COUNT);

            for (int i = 0; i < regionCount; i++) {
                RoiRegionCfg region;
                region.x = (dRegions->rects[i].left) & (~0x01);
                region.y = (dRegions->rects[i].top) & (~0x01);
                region.w = (dRegions->rects[i].right - dRegions->rects[i].left) & (~0x01);
                region.h = (dRegions->rects[i].bottom - dRegions->rects[i].top) & (~0x01);
                region.force_intra = 0;
                region.qp_mode = 0;
                region.qp_val = -10;

                regions.push(region);
            }
            handleRoiRegionRequest(meta, regions);
        }
    }

    err = mMppMpi->encode_put_frame(mMppCtx, frame);
    if (err != MPP_OK) {
        ret = C2_NOT_FOUND;
        goto error;
    }

    /* dump show input process fps if neccessary */
    mDump->showDebugFps(ROLE_INPUT);

    mInputCount++;

error:
    if (frame) {
        mpp_frame_deinit(&frame);
    }

    return ret;
}

c2_status_t C2RKMpiEnc::getoutpacket(OutWorkEntry *entry) {
    MPP_RET err = MPP_OK;
    MppPacket packet = nullptr;

    err = mMppMpi->encode_get_packet(mMppCtx, &packet);
    if (err != MPP_OK || packet == nullptr) {
        return C2_NOT_FOUND;
    } else {
        int64_t  pts = mpp_packet_get_pts(packet);
        size_t   len = mpp_packet_get_length(packet);
        uint32_t eos = mpp_packet_get_eos(packet);
        void   *data = mpp_packet_get_data(packet);

        c2_trace("get outpacket pts %lld size %d eos %d", pts, len, eos);

        /* dump output data if neccessary */
        mDump->recordFile(ROLE_OUTPUT, data, len);

        /* dump show input process fps and time consuming if neccessary */
        mDump->showDebugFps(ROLE_OUTPUT);
        mDump->showFrameTiming(pts);

        if (eos) {
            c2_info("get output eos");
            mOutputEOS = true;
            if (pts == 0 || !len) {
                c2_info("eos with empty pkt");
                return C2_CORRUPTED;
            }
        }

        if (!len) {
            c2_warn("ignore empty output with pts %lld", pts);
            mpp_packet_deinit(&packet);
            return C2_NOT_FOUND;
        }

        entry->frameIndex = pts;
        entry->outPacket  = packet;

        return C2_OK;
    }
}

class C2RKMpiEncFactory : public C2ComponentFactory {
public:
    explicit C2RKMpiEncFactory(std::string name)
            : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
                  GetCodec2RKComponentStore()->getParamReflector())),
              mComponentName(name) {
        C2RKComponentEntry *entry = GetRKComponentEntry(name);
        if (entry != nullptr) {
            mKind = entry->kind;
            mMime = entry->mime;
            mDomain = C2Component::DOMAIN_VIDEO;
        } else {
            c2_err("failed to get component entry from name %s", name.c_str());
        }
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2RKMpiEnc(
                        mComponentName.c_str(),
                        mMime.c_str(),
                        id,
                        std::make_shared<C2RKMpiEnc::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new C2RKInterface<C2RKMpiEnc::IntfImpl>(
                        mComponentName.c_str(),
                        id,
                        std::make_shared<C2RKMpiEnc::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual ~C2RKMpiEncFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
    std::string mComponentName;
    std::string mMime;
    C2Component::kind_t mKind;
    C2Component::domain_t mDomain;
};

C2ComponentFactory* CreateRKMpiEncFactory(std::string componentName) {
    return new ::android::C2RKMpiEncFactory(componentName);
}

} // namespace android
