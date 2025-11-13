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
#define ROCKCHIP_LOG_TAG    "C2RKMpiDec"

#include <C2PlatformSupport.h>
#include <C2AllocatorGralloc.h>
#include <Codec2Mapper.h>
#include <media/stagefright/foundation/ALookup.h>

#include "C2RKMpiDec.h"
#include "C2RKPlatformSupport.h"
#include "C2RKLog.h"
#include "C2RKMediaUtils.h"
#include "C2RKRgaDef.h"
#include "C2RKChipCapDef.h"
#include "C2RKColorAspects.h"
#include "C2RKNaluParser.h"
#include "C2RKVdecExtendFeature.h"
#include "C2RKCodecMapper.h"
#include "C2RKMlvecLegacy.h"
#include "C2RKExtendParameters.h"
#include "C2RKGraphicBufferMapper.h"
#include "C2RKDumpStateService.h"
#include "C2RKTunneledSession.h"
#include "C2RKPropsDef.h"
#include "C2RKVersion.h"

namespace android {

/* max support video resolution */
constexpr uint64_t kCpuReadWriteUsage = (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);

constexpr uint32_t kMaxVideoWidth = 8192;
constexpr uint32_t kMaxVideoHeight = 4320;

constexpr size_t kRenderSmoothnessFactor = 4;
constexpr size_t kMinInputBufferSize = 2 * 1024 * 1024;

struct MlvecParams {
    std::shared_ptr<C2DriverVersion::output> driverInfo;
    std::shared_ptr<C2LowLatencyMode::output> lowLatencyMode;
};

class C2RKMpiDec::IntfImpl : public C2RKInterface<void>::BaseParams {
public:
    explicit IntfImpl(
            const std::shared_ptr<C2ReflectorHelper> &helper,
            C2String name,
            C2Component::kind_t kind,
            C2Component::domain_t domain,
            C2String mediaType)
        : C2RKInterface<void>::BaseParams(helper, name, kind, domain, mediaType) {
        mMlvecParams = std::make_shared<MlvecParams>();

        addParameter(
                DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::output(0))
                .withFields({C2F(mActualOutputDelay, value).inRange(0, C2_MAX_REF_FRAME_COUNT)})
                .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
                .build());

        // input picture frame size
        addParameter(
                DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
                .withFields({
                    C2F(mSize, width).inRange(2, kMaxVideoWidth, 1),
                    C2F(mSize, height).inRange(2, kMaxVideoWidth, 1),
                })
                .withSetter(SizeSetter)
                .build());

        addParameter(
                DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
                .withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
                .withFields({
                    C2F(mSize, width).inRange(2, kMaxVideoWidth, 1),
                    C2F(mSize, height).inRange(2, kMaxVideoWidth, 1),
                })
                .withSetter(MaxPictureSizeSetter, mSize)
                .build());

        addParameter(
                DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
                .withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
                // TODO: More restriction?
                .withFields({C2F(mFrameRate, value).greaterThan(0.)})
                .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBlockSize, C2_PARAMKEY_BLOCK_SIZE)
                .withDefault(new C2StreamBlockSizeInfo::output(0u, 320, 240))
                .withFields({
                    C2F(mBlockSize, width).inRange(2, kMaxVideoWidth, 2),
                    C2F(mBlockSize, height).inRange(2, kMaxVideoWidth, 2),
                })
                .withSetter(BlockSizeSetter)
                .build());

        std::vector<uint32_t> pixelFormats = {HAL_PIXEL_FORMAT_YCBCR_420_888};
        if (C2RKMediaUtils::isP010Allowed()) {
            pixelFormats.push_back(HAL_PIXEL_FORMAT_YCBCR_P010);
        }

        // TODO: support more formats?
        addParameter(
                DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                .withDefault(new C2StreamPixelFormatInfo::output(
                                    0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
                .withFields({C2F(mPixelFormat, value).oneOf(pixelFormats)})
                .withSetter((Setter<decltype(*mPixelFormat)>::StrictValueWithNoDeps))
                .build());

        // profile and level
        if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC) {
            std::vector<uint32_t> avcProfiles ={
                                C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                                C2Config::PROFILE_AVC_BASELINE,
                                C2Config::PROFILE_AVC_MAIN,
                                C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
                                C2Config::PROFILE_AVC_PROGRESSIVE_HIGH,
                                C2Config::PROFILE_AVC_HIGH};
            if (C2RKChipCapDef::get()->is10bitSupport(MPP_VIDEO_CodingAVC)) {
                avcProfiles.push_back(C2Config::PROFILE_AVC_HIGH_10);
                avcProfiles.push_back(C2Config::PROFILE_AVC_PROGRESSIVE_HIGH_10);
            }
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_AVC_BASELINE, C2Config::LEVEL_AVC_5_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf(avcProfiles),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
                                C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
                                C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                                C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2,
                                C2Config::LEVEL_AVC_4, C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                                C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1, C2Config::LEVEL_AVC_5_2,
                                C2Config::LEVEL_AVC_6})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_HEVC) {
            std::vector<uint32_t> hevcProfiles ={C2Config::PROFILE_HEVC_MAIN};
            if (C2RKChipCapDef::get()->is10bitSupport(MPP_VIDEO_CodingHEVC)) {
                hevcProfiles.push_back(C2Config::PROFILE_HEVC_MAIN_10);
            }
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_HEVC_MAIN, C2Config::LEVEL_HEVC_MAIN_5_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf(hevcProfiles),
                        C2F(mProfileLevel, level).oneOf({
                               C2Config::LEVEL_HEVC_MAIN_1,
                               C2Config::LEVEL_HEVC_MAIN_2, C2Config::LEVEL_HEVC_MAIN_2_1,
                               C2Config::LEVEL_HEVC_MAIN_3, C2Config::LEVEL_HEVC_MAIN_3_1,
                               C2Config::LEVEL_HEVC_MAIN_4, C2Config::LEVEL_HEVC_MAIN_4_1,
                               C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1,
                               C2Config::LEVEL_HEVC_MAIN_5_2, C2Config::LEVEL_HEVC_MAIN_6,
                               C2Config::LEVEL_HEVC_MAIN_6_1, C2Config::LEVEL_HEVC_MAIN_6_2,
                               C2Config::LEVEL_HEVC_HIGH_4, C2Config::LEVEL_HEVC_HIGH_4_1,
                               C2Config::LEVEL_HEVC_HIGH_5, C2Config::LEVEL_HEVC_HIGH_5_1,
                               C2Config::LEVEL_HEVC_HIGH_5_2, C2Config::LEVEL_HEVC_HIGH_6})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_MP2V_SIMPLE, C2Config::LEVEL_MP2V_HIGH))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_MP2V_SIMPLE,
                                C2Config::PROFILE_MP2V_MAIN}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_MP2V_LOW,
                                C2Config::LEVEL_MP2V_MAIN,
                                C2Config::LEVEL_MP2V_HIGH_1440,
                                C2Config::LEVEL_MP2V_HIGH})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_MPEG4) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_MP4V_SIMPLE, C2Config::LEVEL_MP4V_3))
                   .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_MP4V_SIMPLE}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_MP4V_0,
                                C2Config::LEVEL_MP4V_0B,
                                C2Config::LEVEL_MP4V_1,
                                C2Config::LEVEL_MP4V_2,
                                C2Config::LEVEL_MP4V_3})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_H263) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_H263_BASELINE, C2Config::LEVEL_H263_30))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_H263_BASELINE,
                                C2Config::PROFILE_H263_ISWV2}),
                       C2F(mProfileLevel, level).oneOf({
                               C2Config::LEVEL_H263_10,
                               C2Config::LEVEL_H263_20,
                               C2Config::LEVEL_H263_30,
                               C2Config::LEVEL_H263_40,
                               C2Config::LEVEL_H263_45})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_VP9) {
            std::vector<uint32_t> vp9Profiles ={C2Config::PROFILE_VP9_0};
            if (C2RKChipCapDef::get()->is10bitSupport(MPP_VIDEO_CodingVP9)) {
                vp9Profiles.push_back(C2Config::PROFILE_VP9_2);
            }
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_5))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf(vp9Profiles),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_VP9_1,
                                C2Config::LEVEL_VP9_1_1,
                                C2Config::LEVEL_VP9_2,
                                C2Config::LEVEL_VP9_2_1,
                                C2Config::LEVEL_VP9_3,
                                C2Config::LEVEL_VP9_3_1,
                                C2Config::LEVEL_VP9_4,
                                C2Config::LEVEL_VP9_4_1,
                                C2Config::LEVEL_VP9_5,
                                C2Config::LEVEL_VP9_5_1,
                                C2Config::LEVEL_VP9_5_2,
                                C2Config::LEVEL_VP9_6})
                     })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_AV1) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_AV1_0, C2Config::LEVEL_AV1_6_3))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_AV1_0,
                                C2Config::PROFILE_AV1_1}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_AV1_2, C2Config::LEVEL_AV1_2_1, C2Config::LEVEL_AV1_2_2,
                                C2Config::LEVEL_AV1_2_3, C2Config::LEVEL_AV1_3, C2Config::LEVEL_AV1_3_1,
                                C2Config::LEVEL_AV1_3_2, C2Config::LEVEL_AV1_3_3, C2Config::LEVEL_AV1_4,
                                C2Config::LEVEL_AV1_4_1, C2Config::LEVEL_AV1_4_2, C2Config::LEVEL_AV1_4_3,
                                C2Config::LEVEL_AV1_5, C2Config::LEVEL_AV1_5_1, C2Config::LEVEL_AV1_5_2,
                                C2Config::LEVEL_AV1_5_3, C2Config::LEVEL_AV1_6})
                     })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        }

        // max input buffer size
        addParameter(
                DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kMinInputBufferSize))
                .withFields({
                    C2F(mMaxInputSize, value).any(),
                })
                .calculatedAs(MaxInputSizeSetter, mMaxSize)
                .build());

        // ColorInfo
        C2ChromaOffsetStruct locations[1] = { C2ChromaOffsetStruct::ITU_YUV_420_0() };
        std::shared_ptr<C2StreamColorInfo::output> defaultColorInfo =
            C2StreamColorInfo::output::AllocShared(
                    1u, 0u, 8u /* bitDepth */, C2Color::YUV_420);
        memcpy(defaultColorInfo->m.locations, locations, sizeof(locations));

        defaultColorInfo =
            C2StreamColorInfo::output::AllocShared(
                   { C2ChromaOffsetStruct::ITU_YUV_420_0() },
                   0u, 8u /* bitDepth */, C2Color::YUV_420);
        helper->addStructDescriptors<C2ChromaOffsetStruct>();

        addParameter(
                DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
                .withConstValue(defaultColorInfo)
                .build());

        // colorAspects
        addParameter(
                DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsTuning::output(
                        0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                        C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({
                    C2F(mDefaultColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                    C2F(mDefaultColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                    C2F(mDefaultColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                    C2F(mDefaultColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
                })
                .withSetter(DefaultColorAspectsSetter)
                .build());

        // vui colorAspects
        if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC ||
            mediaType == MEDIA_MIMETYPE_VIDEO_HEVC ||
            mediaType == MEDIA_MIMETYPE_VIDEO_AV1 ||
            mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2) {
            addParameter(
                    DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
                    .withDefault(new C2StreamColorAspectsInfo::input(
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
                    .withSetter(CodedColorAspectsSetter)
                    .build());

           addParameter(
                    DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                    .withDefault(new C2StreamColorAspectsInfo::output(
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
                    .withSetter(ColorAspectsSetter, mDefaultColorAspects, mCodedColorAspects)
                    .build());
        }

        /* tunneled video playback */
        addParameter(
                DefineParam(mTunneledPlayback, C2_PARAMKEY_TUNNELED_RENDER)
                .withDefault(C2PortTunneledModeTuning::output::AllocUnique(
                        0, C2PortTunneledModeTuning::Struct::NONE,
                        C2PortTunneledModeTuning::Struct::REALTIME, 0))
                .withFields({
                    C2F(mTunneledPlayback, m.mode).oneOf({
                            C2PortTunneledModeTuning::Struct::NONE,
                            C2PortTunneledModeTuning::Struct::SIDEBAND}),
                    C2F(mTunneledPlayback, m.syncType).oneOf({
                            C2PortTunneledModeTuning::Struct::REALTIME,
                            C2PortTunneledModeTuning::Struct::AUDIO_HW_SYNC,
                            C2PortTunneledModeTuning::Struct::HW_AV_SYNC}),
                    C2F(mTunneledPlayback, m.syncId).any()
                })
                .withSetter(TunneledPlaybackSetter)
                .build());

        addParameter(
                DefineParam(mTunneledSideband, C2_PARAMKEY_OUTPUT_TUNNEL_HANDLE)
                .withDefault(decltype(mTunneledSideband)::element_type::AllocShared(256))
                .withFields({C2F(mTunneledSideband, m.values).any()})
                .withSetter(TunneledSidebandSetter, mTunneledPlayback)
                .build());

        addParameter(
                DefineParam(mLowLatency, C2_PARAMKEY_LOW_LATENCY_MODE)
                .withDefault(new C2GlobalLowLatencyModeTuning(false))
                .withFields({C2F(mLowLatency, value)})
                .withSetter(Setter<decltype(*mLowLatency)>::NonStrictValueWithNoDeps)
                .build());

        /* extend parameter definition */
        addParameter(
                DefineParam(mDisableDpbCheck, C2_PARAMKEY_DEC_DISABLE_DPB_CHECK)
                .withDefault(new C2StreamDecDisableDpbCheck::input(0))
                .withFields({C2F(mDisableDpbCheck, value).any()})
                .withSetter(Setter<decltype(mDisableDpbCheck)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mDisableErrorMark, C2_PARAMKEY_DEC_DISABLE_ERROR_MARK)
                .withDefault(new C2StreamDecDisableErrorMark::input(0))
                .withFields({C2F(mDisableErrorMark, value).any()})
                .withSetter(Setter<decltype(mDisableErrorMark)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mLowMemoryMode, C2_PARAMKEY_DEC_LOW_MEMORY_MODE)
                .withDefault(new C2StreamDecLowMemoryMode::input(0))
                .withFields({C2F(mLowMemoryMode, value).any()})
                .withSetter(Setter<decltype(mLowMemoryMode)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mFbcDisable, C2_PARAMKEY_DEC_FBC_DISABLE)
                .withDefault(new C2StreamDecFbcDisable::input(0))
                .withFields({C2F(mFbcDisable, value).any()})
                .withSetter(Setter<decltype(mFbcDisable)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mOutputCropEnable, C2_PARAMKEY_DEC_OUTPUT_CROP)
                .withDefault(new C2StreamDecOutputCropEnable::input(0))
                .withFields({C2F(mOutputCropEnable, value).any()})
                .withSetter(Setter<decltype(mOutputCropEnable)::element_type>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMlvecParams->driverInfo, C2_PARAMKEY_MLVEC_DEC_DRI_VERSION)
                .withConstValue(new C2DriverVersion::output(MLVEC_DRIVER_VERSION))
                .build());

        addParameter(
                DefineParam(mMlvecParams->lowLatencyMode, C2_PARAMKEY_MLVEC_DEC_LOW_LATENCY_MODE)
                .withDefault(new C2LowLatencyMode::output(0))
                .withFields({
                    C2F(mMlvecParams->lowLatencyMode, enable).any(),
                })
                .withSetter(MLowLatenctyModeSetter)
                .build());
    }

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                          C2P<C2StreamPictureSizeInfo::output> &me) {
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
        if (me.set().width * me.set().height > kMaxVideoWidth * kMaxVideoHeight) {
            c2_warn("max support video resolution %dx%d, cur %dx%d",
                    kMaxVideoWidth, kMaxVideoHeight, me.set().width, me.set().height);
        }
        return res;
    }

    static C2R MaxPictureSizeSetter(bool mayBlock, C2P<C2StreamMaxPictureSizeTuning::output> &me,
                                    const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        // TODO: get max width/height from the size's field helpers vs. hardcoding
        me.set().width = c2_min(c2_max(me.v.width, size.v.width), kMaxVideoWidth);
        me.set().height = c2_min(c2_max(me.v.height, size.v.height), kMaxVideoWidth);
        if (me.set().width * me.set().height > kMaxVideoWidth * kMaxVideoHeight) {
            c2_warn("max support video resolution %dx%d, cur %dx%d",
                    kMaxVideoWidth, kMaxVideoHeight, me.set().width, me.set().height);
        }
        return C2R::Ok();
    }

    static C2R BlockSizeSetter(bool mayBlock, const C2P<C2StreamBlockSizeInfo::output> &oldMe,
                          C2P<C2StreamBlockSizeInfo::output> &me) {
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

    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input> &me,
                                  const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        (void)size;
        (void)me;  // TODO: validate
        return C2R::Ok();
    }

    static C2R MaxInputSizeSetter(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input> &me,
                                const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize) {
        (void)mayBlock;
        if (C2RKPropsDef::getInputBufferSize() > 0) {
            me.set().value = C2RKPropsDef::getInputBufferSize();
        } else {
            // assume compression ratio of 2
            me.set().value = c2_max((((maxSize.v.width + 63) / 64)
                * ((maxSize.v.height + 63) / 64) * 3072), kMinInputBufferSize);
        }
        return C2R::Ok();
    }


    static C2R DefaultColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsTuning::output> &me) {
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

    static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me) {
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

    static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                                  const C2P<C2StreamColorAspectsTuning::output> &def,
                                  const C2P<C2StreamColorAspectsInfo::input> &coded) {
        (void)mayBlock;
        // take default values for all unspecified fields, and coded values for specified ones
        me.set().range = coded.v.range == RANGE_UNSPECIFIED ? def.v.range : coded.v.range;
        me.set().primaries = coded.v.primaries == PRIMARIES_UNSPECIFIED
                ? def.v.primaries : coded.v.primaries;
        me.set().transfer = coded.v.transfer == TRANSFER_UNSPECIFIED
                ? def.v.transfer : coded.v.transfer;
        me.set().matrix = coded.v.matrix == MATRIX_UNSPECIFIED ? def.v.matrix : coded.v.matrix;
        return C2R::Ok();
    }

    static C2R TunneledPlaybackSetter(
        bool mayBlock, C2P<C2PortTunneledModeTuning::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    static C2R TunneledSidebandSetter(
        bool mayBlock, C2P<C2PortTunnelHandleTuning::output> &me,
        const C2P<C2PortTunneledModeTuning::output> &tunneledMode) {
        (void)mayBlock;
        (void)me;
        if (tunneledMode.v.m.mode != C2PortTunneledModeTuning::Struct::SIDEBAND) {
            return C2R::BadState();
        }
        return C2R::Ok();
    }

    static C2R MLowLatenctyModeSetter(
        bool mayBlock, C2P<C2LowLatencyMode::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    std::shared_ptr<C2StreamPictureSizeInfo::output> getSize_l() {
        return mSize;
    }

    std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate_l() {
        return mFrameRate;
    }

    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects_l() {
        return mColorAspects;
    }

    std::shared_ptr<C2StreamColorAspectsTuning::output> getDefaultColorAspects_l() {
        return mDefaultColorAspects;
    }

    std::shared_ptr<C2GlobalLowLatencyModeTuning> getLowLatency_l() {
        return mLowLatency;
    }

    std::shared_ptr<C2StreamProfileLevelInfo::input> getProfileLevel_l() {
        return mProfileLevel;
    }

    std::shared_ptr<C2StreamPixelFormatInfo::output> getPixelFormat_l() const {
        return mPixelFormat;
    }

    bool getIsDisableDpbCheck() const {
        if (mDisableDpbCheck && mDisableDpbCheck->value > 0) {
            return true;
        }
        return false;
    }

    bool getIsDisableErrorMark() const {
        if (mDisableErrorMark && mDisableErrorMark->value > 0) {
            return true;
        }
        return false;
    }

    bool getIsLowMemoryMode() const {
        if (mLowMemoryMode && mLowMemoryMode->value > 0) {
            return true;
        }
        return (C2RKPropsDef::getLowMemoryMode() > 0);
    }

    bool getFbcDisable() const {
        if (mFbcDisable && mFbcDisable->value > 0) {
            return true;
        }
        return false;
    }

    bool getOutputCropEnable() const {
        if (mOutputCropEnable && mOutputCropEnable->value > 0) {
            return true;
        }
        return false;
    }

    bool getIsLowLatencyMode() {
        if (mLowLatency && mLowLatency->value > 0) {
            return true;
        }
        if (mMlvecParams->lowLatencyMode
                && mMlvecParams->lowLatencyMode->enable != 0) {
            return true;
        }
        return false;
    }

    bool getIs10bit() {
        uint32_t profile = mProfileLevel ? mProfileLevel->profile : 0;
        if (profile == PROFILE_AVC_HIGH_10 ||
            profile == PROFILE_HEVC_MAIN_10 ||
            profile == PROFILE_VP9_2) {
            return true;
        }
        if (mDefaultColorAspects->transfer == 6 /* SMPTEST2084 */) {
            return true;
        }
        return false;
    }

    bool getIsTunnelMode() {
        if (mTunneledPlayback && mTunneledPlayback->m.mode
                == C2PortTunneledModeTuning::Struct::SIDEBAND) {
            return true;
        }
        return false;
    }

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamBlockSizeInfo::output> mBlockSize;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2GlobalLowLatencyModeTuning> mLowLatency;
    std::shared_ptr<C2PortTunneledModeTuning::output> mTunneledPlayback;
    std::shared_ptr<C2PortTunnelHandleTuning::output> mTunneledSideband;

    /* extend parameter definition */
    std::shared_ptr<C2StreamDecDisableDpbCheck::input> mDisableDpbCheck;
    std::shared_ptr<C2StreamDecDisableErrorMark::input> mDisableErrorMark;
    std::shared_ptr<C2StreamDecLowMemoryMode::input> mLowMemoryMode;
    std::shared_ptr<C2StreamDecFbcDisable::input> mFbcDisable;
    std::shared_ptr<C2StreamDecOutputCropEnable::input> mOutputCropEnable;
    std::shared_ptr<MlvecParams> mMlvecParams;
};

static int32_t frameReadyCb(void *ctx, void *mppCtx, int32_t cmd, void *frame) {
    (void)mppCtx;
    (void)cmd;
    (void)frame;
    C2RKMpiDec *decoder = reinterpret_cast<C2RKMpiDec *>(ctx);
    decoder->postFrameReady();
    return 0;
}

static int64_t _toDts(int64_t frameIndex) {
    return (++frameIndex);
}

static int64_t _toFrameIndex(int64_t dts) {
    return (--dts);
}

void C2RKMpiDec::WorkHandler::flushAllMessages() {
    mRunning = false;

    sp<AMessage> msg = new AMessage(WorkHandler::kWhatFlushMessage, this);
    sp<AMessage> response;
    msg->postAndAwaitResponse(&response);

    mRunning = true;
}

void C2RKMpiDec::WorkHandler::stop() {
    flushAllMessages();

    mRunning = false;
}

void C2RKMpiDec::WorkHandler::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatFrameReady: {
            C2RKMpiDec *thiz = nullptr;
            if (mRunning && msg->findPointer("thiz", (void **)(&thiz)) && thiz) {
                thiz->drainWork();
            }
        } break;
        case kWhatFlushMessage: {
            sp<AReplyToken> replyID;
            msg->senderAwaitsResponse(&replyID);

            sp<AMessage> response = new AMessage;
            response->postReply(replyID);
        } break;
        default: {
            c2_err("Unrecognized msg: %d", msg->what());
        } break;
    }
}

bool C2RKMpiDec::OutBuffer::ownedByDecoder() {
    return mOwnedByDecoder;
}

void C2RKMpiDec::OutBuffer::submitToDecoder() {
    if (!mOwnedByDecoder) {
        mpp_buffer_put(mMppBuffer);
        mOwnedByDecoder = true;
    } else {
        c2_warn("submitToDecoder - invalid operation "
                "(the index %d is already owned by decoder)", mBufferId);
    }
}

void C2RKMpiDec::OutBuffer::setInusedByClient() {
    if (mOwnedByDecoder) {
        mpp_buffer_inc_ref(mMppBuffer);
        mOwnedByDecoder = false;
    } else {
        c2_warn("setInusedByClient - invalid operation "
                "(the index %d is not owned by decoder)", mBufferId);
    }
}

class C2DecNodeInfoListener : public C2NodeInfoListener {
public:
    explicit C2DecNodeInfoListener(C2RKMpiDec *thiz) : mThiz(thiz) {}
    ~C2DecNodeInfoListener() override = default;

    void onNodeSummaryRequest(std::string &summary) override {
        mThiz->onNodeSummaryRequest(summary);
    }

private:
    C2RKMpiDec *mThiz;
};

C2RKMpiDec::C2RKMpiDec(
        const char *name,
        const char *mime,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mName(name),
      mMime(mime),
      mIntf(intfImpl),
      mTunneledSession(nullptr),
      mDumpService(C2RKDumpStateService::get()),
      mLooper(nullptr),
      mHandler(nullptr),
      mMppCtx(nullptr),
      mMppMpi(nullptr),
      mCodingType(MPP_VIDEO_CodingUnused),
      mColorFormat(MPP_FMT_YUV420SP),
      mBufferGroup(nullptr),
      mWidth(0),
      mHeight(0),
      mHorStride(0),
      mVerStride(0),
      mLeftCorner(0),
      mTopCorner(0),
      mNumOutputSlots(0),
      mSlotsToReduce(0),
      mPixelFormat(0),
      mScaleMode(0),
      mFdPerf(-1),
      mStarted(false),
      mFlushed(true),
      mInputEOS(false),
      mOutputEOS(false),
      mSignalledError(false),
      mGraphicSourceMode(false),
      mHdrMetaEnabled(false),
      mTunneled(false),
      mBufferMode(false),
      mUseRgaBlit(true),
      mStandardWorkFlow(true) {
    c2_info("[%s] version %s", name, C2_COMPONENT_FULL_VERSION);
    mCodingType = (MppCodingType)GetMppCodingFromComponentName(name);
    if (mCodingType == MPP_VIDEO_CodingUnused) {
        c2_err("failed to get coding from name %s", name);
    }
}

C2RKMpiDec::~C2RKMpiDec() {
    onRelease();

    mDumpService->removeNode(this);
    mDumpService->dumpNodesSummary();
}

// Implementation of virtual function from C2NodeInfoListener
void C2RKMpiDec::onNodeSummaryRequest(std::string &summary) {
    const size_t SIZE = 256;
    char buffer[SIZE];
    int64_t inputFrames = 0;
    int64_t outputFrames = 0;
    int64_t errorFrames = 0;
    ColorAspects sfAspects;

    ColorUtils::convertIsoColorAspectsToCodecAspects(
            mBitstreamColorAspects.primaries, mBitstreamColorAspects.transfer,
            mBitstreamColorAspects.coeffs, mBitstreamColorAspects.fullRange, sfAspects);

    snprintf(buffer, sizeof(buffer),
        "| Component   : %s\n"
        "| Media Format: %s, %.1f fps, %s%s\n"
        "| Resolution  : %dx%d (Stride %dx%d)\n",
        mName, mMime, mIntf->getFrameRate_l()->value,
        MPP_FRAME_FMT_IS_YUV_10BIT(mColorFormat) ? "10-Bit" : "8-Bit",
        MPP_FRAME_FMT_IS_FBC(mColorFormat) ? ", FBC" : "",
        mWidth, mHeight, mHorStride, mVerStride);

    summary.append(buffer);

    snprintf(buffer, sizeof(buffer),
        "| Color Info  : Range=%d(%s)\n"
        "|               Primaries=%d(%s)\n"
        "|               Matrix=%d(%s)\n"
        "|               Transfer=%d(%s)\n",
        sfAspects.mRange, asString(sfAspects.mRange),
        sfAspects.mPrimaries, asString(sfAspects.mPrimaries),
        sfAspects.mMatrixCoeffs, asString(sfAspects.mMatrixCoeffs),
        sfAspects.mTransfer, asString(sfAspects.mTransfer));

    summary.append(buffer);

    if (!mBuffers.empty()) {
        size_t sizeOwnedByDecoder = std::count_if(
            mBuffers.begin(),  mBuffers.end(),
            [](const auto& value) {
                return value.second->ownedByDecoder();
            });

        snprintf(buffer, sizeof(buffer),
            "|\n|--------------Buffer Allocation State-------------|\n"
            "| Count       : %zu (%zu in decoder)\n"
            "| Size        : %d bytes each\n"
            "| Usage       : 0x%llx\n"
            "| Format      : 0x%x\n"
            "| Mode        : %s\n",
            mBuffers.size(), sizeOwnedByDecoder,
            mBuffers.begin()->second->getSize(),
            (long long)mAllocParams.usage, mAllocParams.format,
            mBufferMode ? "BufferMode" : "SurfaceMode");

        summary.append(buffer);
    }

    if (mDumpService->getNodePortFrameCount(
            this, &inputFrames, &outputFrames, &errorFrames) && inputFrames > 0) {
        int32_t diff = inputFrames - outputFrames - errorFrames;
        int32_t threshold = mNumOutputSlots - mSlotsToReduce + kRenderSmoothnessFactor;
        std::string errorFramesDesc = "";

        if (errorFrames > 0) {
            snprintf(buffer, sizeof(buffer), ", %lld Error", (long long)errorFrames);
            errorFramesDesc.append(buffer);
        }

        snprintf(buffer, sizeof(buffer),
            "|\n|--------------Pipeline Runtime State--------------|\n"
            "| Input packet: %lld Totals, %lld Decoded%s\n"
            "| Threshold   : %d (Slots %d Smoothness %d)\n"
            "| State       : %s\n",
            (long long)inputFrames, (long long)outputFrames, errorFramesDesc.c_str(),
            threshold, (mNumOutputSlots - mSlotsToReduce), (int)kRenderSmoothnessFactor,
            (diff >= threshold) ? "Pipeline-Full" : "Normal");

        summary.append(buffer);
    }
}

c2_status_t C2RKMpiDec::onInit() {
    c2_log_func_enter();

    std::shared_ptr<C2NodeInfo> nodeInfo =
            std::make_shared<C2NodeInfo>(
                this,   // nodeId
                mIntf->getSize_l()->width,  // width
                mIntf->getSize_l()->height, // height
                false,  // isEncoder
                mIntf->getFrameRate_l()->value // frameRate
            );

    nodeInfo->setListener(std::make_shared<C2DecNodeInfoListener>(this));

    if (!mDumpService->addNode(nodeInfo)) {
        mDumpService->dumpNodesSummary();
        return C2_NO_MEMORY;
    }

    c2_status_t err = setupAndStartLooper();
    if (err != C2_OK) {
        c2_err("failed to start looper therad");
        return err;
    }

    err = configOutputDelay();
    if (err != C2_OK) {
        c2_err("failed to config output delay");
    }

    return err;
}

c2_status_t C2RKMpiDec::onStop() {
    c2_log_func_enter();
    if (!mFlushed) {
        return onFlush_sm();
    }

    return C2_OK;
}

void C2RKMpiDec::onReset() {
    c2_log_func_enter();
    onStop();
}

void C2RKMpiDec::onRelease() {
    if (!mStarted)
        return;

    c2_log_func_enter();

    /* set flushing state to discard all work output */
    setFlushingState();

    if (!mFlushed) {
        onFlush_sm();
    }

    stopAndReleaseLooper();

    releaseAllBuffers();

    if (mBlockPool) {
        mBlockPool.reset();
    }

    if (mBufferGroup != nullptr) {
        mpp_buffer_group_put(mBufferGroup);
        mBufferGroup = nullptr;
    }

    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    if (mTunneled) {
        mTunneledSession->disconnect();
    }

    stopFlushingState();
    setMppPerformance(false);

    mStarted = false;
}

c2_status_t C2RKMpiDec::onFlush_sm() {
    if (!mFlushed) {
        c2_log_func_enter();
        mInputEOS = false;
        mOutputEOS = false;
        mSignalledError = false;

        if (mMppMpi) {
            mMppMpi->reset(mMppCtx);
        }

        if (mHandler) {
            mHandler->flushAllMessages();
        }

        // reset dump statistics
        mDumpService->resetNode(this);

        mFlushed = true;
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::setupAndStartLooper() {
    status_t err = OK;

    if (mLooper == nullptr) {
        mLooper = new ALooper;
        mHandler = new WorkHandler;

        mLooper->setName("C2DecLooper");
        err = mLooper->start(false, false, ANDROID_PRIORITY_VIDEO);
        if (err == OK) {
            mLooper->registerHandler(mHandler);
        }
    }
    return (c2_status_t)err;
}

void C2RKMpiDec::stopAndReleaseLooper() {
    if (mLooper != nullptr) {
        if (mHandler != nullptr) {
            mLooper->unregisterHandler(mHandler->id());
            mHandler.clear();
        }
        mLooper->stop();
        mLooper.clear();
    }
}

int32_t C2RKMpiDec::getFbcOutputMode(const std::unique_ptr<C2Work> &work) {
    int32_t fbcMode = C2RKChipCapDef::get()->getFbcOutputMode(mCodingType);

    if (!fbcMode || mGraphicSourceMode || mBufferMode) {
        return 0;
    }

    {
        IntfImpl::Lock lock = mIntf->lock();
        if (mIntf->getFbcDisable() ||
                mDumpService->hasFeatures(C2_FEATURE_DEC_DISABLE_FBC)) {
            c2_info("got disable fbc request");
            return 0;
        }
    }

    if (fbcMode == C2_COMPRESS_AFBC_16x16) {
        if (MPP_FRAME_FMT_IS_YUV_10BIT(mColorFormat)) {
            c2_trace("10bit video source, perfer afbc output mode");
            return fbcMode;
        }

        // do extra detection from spspps to search bitInfo in this case
        if (work != nullptr && work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
            if (!work->input.buffers.empty()) {
                C2ReadView rView = mDummyReadView;
                rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
                int32_t depth = C2RKNaluParser::detectBitDepth(
                        const_cast<uint8_t *>(rView.data()), rView.capacity(), mCodingType);
                if (depth == 10) {
                    c2_trace("10bit video profile detached, prefer afbc output mode");
                    return fbcMode;
                }
            }
        }
    } else if ((mColorFormat & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV422SP_10BIT ||
               (mColorFormat & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV444SP_10BIT) {
        c2_info("10bit video source, perfer rfbc output mode");
        return fbcMode;
    }

    uint32_t minStride = C2RKChipCapDef::get()->getFbcMinStride(fbcMode);

    if (mWidth <= minStride && mHeight <= minStride) {
        c2_info("within fbc min stirde %d, disable fbc otuput mode", minStride);
        return 0;
    }

    return fbcMode;
}

c2_status_t C2RKMpiDec::getSurfaceFeatures(const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err = C2_OK;
    std::shared_ptr<C2GraphicBlock> block;

    // alloc a temporary graphicBuffer to get surface features.
    err = pool->fetchGraphicBlock(
            176, 144, HAL_PIXEL_FORMAT_YCrCb_NV12,
            {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE}, &block);
    if (err != C2_OK) {
        c2_err("failed to fetchGraphicBlock, err %d", err);
        return err;
    }

    buffer_handle_t handle = nullptr;
    auto c2Handle = block->handle();

    if (C2RKGraphicBufferMapper::get()->importBuffer(c2Handle, &handle) != OK) {
        c2_err("failed to import feature buffer");
        return C2_CORRUPTED;
    }

    uint64_t usage = C2RKGraphicBufferMapper::get()->getUsage(handle);
    if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
        mGraphicSourceMode = true;
        updateFbcModeIfNeeded();
    }

    /* check use scale mode */

    uint32_t scaleMode = C2RKChipCapDef::get()->getScaleMode();

    if (!scaleMode || mBufferMode || C2RKPropsDef::getScaleDisable()) {
        goto cleanUp;
    }

    switch (scaleMode) {
        case C2_SCALE_MODE_META:
            checkUseScaleMeta(handle);
        break;
        case C2_SCALE_MODE_DOWN_SCALE:
            checkUseScaleDown(handle);
        break;
        default:
        break;
    }

cleanUp:
    C2RKGraphicBufferMapper::get()->freeBuffer(handle);
    return err;
}

c2_status_t C2RKMpiDec::checkUseScaleMeta(buffer_handle_t handle) {
    MPP_RET ret = MPP_OK;
    MppDecCfg cfg = nullptr;
    int needScale = 0;
    uint32_t scaleMode = C2_SCALE_MODE_META;

    needScale = C2RKVdecExtendFeature::checkNeedScale(handle);
    if (needScale < 0) {
        return C2_OK;
    } else if (needScale == 0) {
        scaleMode = C2_SCALE_MODE_NONE;
    }

    if (mScaleMode == scaleMode) {
        return C2_OK;
    }

    mpp_dec_cfg_init(&cfg);

    mMppMpi->control(mMppCtx, MPP_DEC_GET_CFG, cfg);
    mpp_dec_cfg_set_u32(cfg, "base:enable_thumbnail", scaleMode);
    ret = mMppMpi->control(mMppCtx, MPP_DEC_SET_CFG, cfg);
    if (ret != MPP_OK) {
        c2_warn("failed to set scale mode %d", scaleMode);
        goto cleanUp;
    }

    c2_info("enable scale meta dec");
    mScaleMode = scaleMode;

cleanUp:
    if (cfg != nullptr) {
        mpp_dec_cfg_deinit(cfg);
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::checkUseScaleDown(buffer_handle_t handle) {
    MPP_RET err = MPP_OK;
    MppDecCfg cfg = nullptr;
    (void)handle;

    // enable scale dec only in 8k
    if (mWidth <= 4096 && mHeight <= 4096) {
        goto cleanUp;
    }

    mpp_dec_cfg_init(&cfg);

    mMppMpi->control(mMppCtx, MPP_DEC_GET_CFG, cfg);
    mpp_dec_cfg_set_u32(cfg, "base:enable_thumbnail", C2_SCALE_MODE_DOWN_SCALE);
    err = mMppMpi->control(mMppCtx, MPP_DEC_SET_CFG, cfg);
    if (err != MPP_OK) {
        c2_warn("failed to set scale down mode");
        goto cleanUp;
    }

    mScaleMode = C2_SCALE_MODE_DOWN_SCALE;

    c2_info("enable scale down mode");

cleanUp:
    if (cfg != nullptr) {
        mpp_dec_cfg_deinit(cfg);
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::configOutputDelay(const std::unique_ptr<C2Work> &work) {
    c2_status_t err = C2_OK;
    uint32_t width = 0, height = 0, level = 0;
    uint32_t dpbBasedRefCnt, protocolRefCnt = 0;
    uint32_t numOutputSlots = 0;

    bool lowMemoryMode = false;

    {
        IntfImpl::Lock lock = mIntf->lock();
        width  = mIntf->getSize_l()->width;
        height = mIntf->getSize_l()->height;
        level  = mIntf->getProfileLevel_l() ? mIntf->getProfileLevel_l()->level : 0;
        lowMemoryMode |= mIntf->getIsLowMemoryMode();
        lowMemoryMode |= mDumpService->hasFeatures(C2_FEATURE_DEC_LOW_MEMORY_MODE);
        if (lowMemoryMode) {
            c2_info("in low memory mode, reduce output ref count");
        }
    }

    numOutputSlots = dpbBasedRefCnt =
            C2RKMediaUtils::calculateVideoRefCount(mCodingType, width, height, level);

    if (work != nullptr) {
        C2ReadView rView = mDummyReadView;
        if (!work->input.buffers.empty()) {
            rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
            protocolRefCnt = C2RKNaluParser::detectMaxRefCount(
                    const_cast<uint8_t *>(rView.data()), rView.capacity(), mCodingType);
            if (lowMemoryMode && protocolRefCnt > 0 && protocolRefCnt < dpbBasedRefCnt) {
                numOutputSlots = protocolRefCnt;
            } else {
                numOutputSlots = C2_MAX(dpbBasedRefCnt, protocolRefCnt);
            }
        }
    }

    // TODO: Codec2 sfplugin only accepts growing output slots
    if (numOutputSlots > mNumOutputSlots) {
        uint32_t slotsToReduce = 0;
        std::vector<std::unique_ptr<C2SettingResult>> failures;

        c2_info("Codec(%s %dx%d) requires %d output slots based on %s",
                toStr_Coding(mCodingType), width, height, numOutputSlots,
                (protocolRefCnt) ? "protocol" : "levelInfo");

        /*
         * The kSmoothnessFactor on the framework is 4, and the ccodec_rendering-deep is 3.
         * Under low memory conditions, the reported delayRef is reduced by 4, which is
         * equivalent to occupying 3 buffer blocks of the framework.
         */
        if (lowMemoryMode) {
            slotsToReduce = C2_MIN(numOutputSlots, kRenderSmoothnessFactor - 1);
        }

        C2PortActualDelayTuning::output delay(numOutputSlots - slotsToReduce);
        err = mIntf->config({&delay}, C2_MAY_BLOCK, &failures);
        if (err != C2_OK) {
            c2_err("failed to config delay tuning, err %d", err);
        } else {
            if (work != nullptr) {
                work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(delay));
            }

            mSlotsToReduce = slotsToReduce;
            mNumOutputSlots = numOutputSlots;
        }
    }

    return err;
}

c2_status_t C2RKMpiDec::configTunneledPlayback(const std::unique_ptr<C2Work> &work) {
    if (!mTunneledSession) {
        mTunneledSession = std::make_shared<C2RKTunneledSession>();
    }

    c2_status_t err = C2_OK;
    TunnelParams params;

    params.left   = mLeftCorner;
    params.top    = mTopCorner;
    params.right  = mWidth;
    params.bottom = mHeight;
    params.width  = mHorStride;
    params.height = mVerStride;
    params.format = C2RKMediaUtils::getHalPixerFormat(mColorFormat);
    params.usage  = 0;
    params.dataSpace = 0;
    params.compressMode = MPP_FRAME_FMT_IS_FBC(mColorFormat) ? 1 : 0;

    if (!mTunneledSession->configure(params)) {
        c2_err("failed to congigure tunneled session");
        return C2_CORRUPTED;
    }

    c2_info("configuring TUNNELED video playback.");

    int32_t handles[256];
    std::unique_ptr<C2PortTunnelHandleTuning::output> tunnelHandle =
            C2PortTunnelHandleTuning::output::AllocUnique(handles);

    void *sideband = mTunneledSession->getTunnelSideband();
    memcpy(tunnelHandle->m.values, sideband, sizeof(SidebandHandler));

    // 1. When codec2 plugin start update stream sideband to native window, the
    //    decoder has not yet received format information.
    // 2. rebuild sideband configUpdate to update sideband here, so extra patch
    //    in frameworks is needed to handle extend configUpdate.
    // 3. TODO: Is there any way to without patch in frameworks ?
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    err = mIntf->config({ tunnelHandle.get() }, C2_MAY_BLOCK, &failures);
    if (err == C2_OK) {
        C2StreamTunnelStartRender::output tunnel(0, true);
        work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(tunnel));

        // enable fast out in tunnel mode
        uint32_t fastOut = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_IMMEDIATE_OUT, &fastOut);
    } else {
        c2_err("failed to config tunnel handle");
    }

    return err;
}

c2_status_t C2RKMpiDec::updateDecoderArgs(const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err  = C2_OK;
    bool needsUpdate = false;

    {
        IntfImpl::Lock lock = mIntf->lock();
        int32_t width       = mIntf->getSize_l()->width;
        int32_t height      = mIntf->getSize_l()->height;
        int32_t pixelFormat = mIntf->getPixelFormat_l()->value;
        int32_t colorFormat = mIntf->getIs10bit() ? MPP_FMT_YUV420SP_10BIT : MPP_FMT_YUV420SP;
        bool    tunneled    = mIntf->getIsTunnelMode();
        bool    bufferMode  = (pool->getLocalId() <= C2BlockPool::PLATFORM_START);

        // needs mpp frame update, initial setup in initDecoder()
        needsUpdate = (mWidth != width) || (mHeight != height);

        // av1 support convert to user-set format internally
        if (mCodingType == MPP_VIDEO_CodingAV1
                && pixelFormat == HAL_PIXEL_FORMAT_YCBCR_P010) {
            colorFormat = MPP_FMT_YUV420SP_10BIT;
        }

        // In surfaceTexture case, it is hopes that the component output result
        // without stride since they don't want to deal with crop.
        if (mIntf->getOutputCropEnable() ||
                mDumpService->hasFeatures(C2_FEATURE_DEC_EXCLUDE_PADDING)) {
            c2_info("get request for output crop");
            bufferMode = true;
        }

        if (mDumpService->hasFeatures(C2_FEATURE_DEC_INTERNAL_BUFFER_GROUP)) {
            c2_info("get request for use internal buffer group");
            bufferMode = true;
        }

        // since P010 format is different from the decoder's compact 10-bit output
        // format,  switch to output buffer mode and do an extra copy operation to
        // convert to P010 format.
        if (colorFormat == MPP_FMT_YUV420SP_10BIT) {
            if (pixelFormat == HAL_PIXEL_FORMAT_YCBCR_P010) {
                c2_info("got p010 format request, use output buffer mode");
                bufferMode = true;
            }
            if (width * height <= 176 * 144) {
                bufferMode = true;
            }
        }

        mBufferMode  = bufferMode;
        mBlockPool   = pool;
        mWidth       = width;
        mHeight      = height;
        mPixelFormat = pixelFormat;
        mTunneled    = tunneled;
        mColorFormat = (mStarted) ? mColorFormat : (MppFrameFormat)colorFormat;
    }

    if (mStarted && needsUpdate) {
        err = updateMppFrameInfo(getFbcOutputMode());
        if (err == C2_OK) {
            // update alloc params once args updated
            err = updateAllocParams();
        }
    }

    return err;
}

c2_status_t C2RKMpiDec::updateAllocParams() {
    int32_t allocWidth  = 0;
    int32_t allocHeight = 0;
    int32_t allocFormat = 0;
    int64_t allocUsage  = RK_GRALLOC_USAGE_SPECIFY_STRIDE;

    int32_t videoWidth  = mWidth;
    int32_t videoHeight = mHeight;
    int32_t frameWidth  = mHorStride;
    int32_t frameHeight = mVerStride;
    int32_t colorFormat = mColorFormat;

    // In down-scaling mode, update surface info using down-scaling config
    if (mScaleMode == C2_SCALE_MODE_DOWN_SCALE) {
        MppFrame frame  = nullptr;

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, mWidth);
        mpp_frame_set_height(frame, mHeight);
        mpp_frame_set_hor_stride(frame, mHorStride);
        mpp_frame_set_ver_stride(frame, mVerStride);
        mpp_frame_set_fmt(frame, mColorFormat);

        mMppMpi->control(mMppCtx, MPP_DEC_GET_THUMBNAIL_FRAME_INFO, (MppParam)frame);

        videoWidth  = mpp_frame_get_width(frame);
        videoHeight = mpp_frame_get_height(frame);
        frameWidth  = mpp_frame_get_hor_stride(frame);
        frameHeight = mpp_frame_get_ver_stride(frame);
        colorFormat = mpp_frame_get_fmt(frame);

        c2_info("using down-scaling alloc params: w %d h %d hor %d ver %d fmt %x",
                videoWidth, videoHeight, frameWidth, frameHeight, colorFormat);

        mpp_frame_deinit(&frame);
    }

    allocWidth  = frameWidth;
    allocHeight = frameHeight;
    allocFormat = C2RKMediaUtils::getHalPixerFormat(colorFormat);

    if (MPP_FRAME_FMT_IS_FBC(colorFormat)) {
        // NOTE: FBC case may have offset y on top and vertical stride
        // should aligned to 16.
        allocHeight = C2_ALIGN(frameHeight + mTopCorner, 16);

        // In fbc 10bit mode, surfaceCB treat width as pixel stride.
        if (allocFormat == HAL_PIXEL_FORMAT_YUV420_10BIT_I ||
            allocFormat == HAL_PIXEL_FORMAT_Y210 ||
            allocFormat == HAL_PIXEL_FORMAT_YUV420_10BIT_RFBC ||
            allocFormat == HAL_PIXEL_FORMAT_YUV422_10BIT_RFBC ||
            allocFormat == HAL_PIXEL_FORMAT_YUV444_10BIT_RFBC) {
            allocWidth = C2_ALIGN(videoWidth, 64);
        }
    } else {
        int32_t grallocVersion = C2RKGraphicBufferMapper::get()->getMapperVersion();

        // NOTE: private gralloc stride usage only support in 4.0.
        // Update use stride usage if we are able config available stride.
        if (!mGraphicSourceMode && grallocVersion >= 4) {
            uint64_t horUsage = 0, verUsage = 0;

            // 10bit video calculate stride base on (width * 10 / 8)
            if (MPP_FRAME_FMT_IS_YUV_10BIT(colorFormat)) {
                horUsage = C2RKMediaUtils::getStrideUsage(videoWidth * 10 / 8, frameWidth);
            } else {
                horUsage = C2RKMediaUtils::getStrideUsage(videoWidth, frameWidth);
            }
            verUsage = C2RKMediaUtils::getHStrideUsage(videoHeight, frameHeight);

            if (horUsage > 0 && verUsage > 0) {
                allocWidth  = videoWidth;
                allocHeight = videoHeight;
                allocUsage &= ~RK_GRALLOC_USAGE_SPECIFY_STRIDE;
                allocUsage |= (horUsage | verUsage);
                c2_info("update use stride usage 0x%llx", allocUsage);
            }
        } else if (mCodingType == MPP_VIDEO_CodingVP9 && grallocVersion < 4) {
            allocWidth = C2_ALIGN_ODD(videoWidth, 256);
        }
    }

    {
        IntfImpl::Lock lock = mIntf->lock();
        std::shared_ptr<C2StreamColorAspectsTuning::output> colorAspects
                = mIntf->getDefaultColorAspects_l();

        switch (colorAspects->primaries) {
            case C2Color::PRIMARIES_BT601_525:
                allocUsage |= MALI_GRALLOC_USAGE_YUV_COLOR_SPACE_BT601;
                break;
            case C2Color::PRIMARIES_BT709:
                allocUsage |= MALI_GRALLOC_USAGE_YUV_COLOR_SPACE_BT709;
                break;
            default:
                break;
        }
        switch (colorAspects->range) {
            case C2Color::RANGE_FULL:
                allocUsage |= MALI_GRALLOC_USAGE_RANGE_WIDE;
                break;
            default:
                allocUsage |= MALI_GRALLOC_USAGE_RANGE_NARROW;
                break;
        }
    }

    // only large than gralloc 4 can support int64 usage.
    // otherwise, gralloc 3 will check high 32bit is empty,
    // if not empty, will alloc buffer failed and return
    // error. So we need clear high 32 bit.
    if (C2RKGraphicBufferMapper::get()->getMapperVersion() < 4) {
        allocUsage &= 0xffffffff;
    }

#ifdef GRALLOC_USAGE_DYNAMIC_HDR
    if (mHdrMetaEnabled) {
        allocUsage |= GRALLOC_USAGE_DYNAMIC_HDR;
    }
#endif

    if (mScaleMode == C2_SCALE_MODE_META) {
        allocUsage |= GRALLOC_USAGE_RKVDEC_SCALING;
    }

    if (!mBufferMode) {
        // For 3288 and 3399, Setting buffer with cache can reduce the time
        // required for SurfaceFlinger_NV12-10bit to 16bit conversion
        if (C2RKChipCapDef::get()->getChipType() == RK_CHIP_3399 ||
            C2RKChipCapDef::get()->getChipType() == RK_CHIP_3288) {
            allocUsage |= kCpuReadWriteUsage;
        }
    }

    mAllocParams.width  = allocWidth;
    mAllocParams.height = allocHeight;
    mAllocParams.usage  = allocUsage;
    mAllocParams.format = allocFormat;

    mUseRgaBlit = true;

    c2_trace("update alloc attrs, size %dx%d usage 0x%llx format %d",
              allocWidth, allocHeight, allocUsage, allocFormat);

    return C2_OK;
}

c2_status_t C2RKMpiDec::updateMppFrameInfo(int32_t fbcMode) {
    if (!mMppCtx) {
        return C2_OK;
    }

    MPP_RET err = MPP_OK;
    MppFrame frame = nullptr;
    int32_t format = mColorFormat;
    int32_t leftCorner = 0, topCorner = 0;

    if (fbcMode) {
        format |= MPP_FRAME_FBC_AFBC_V2;
        /* fbc decode output has padding inside, set crop before display */
        C2RKChipCapDef::get()->getFbcOutputOffset(mCodingType, &leftCorner, &topCorner);
        c2_info("use fbc output mode, padding offset(%d, %d)", leftCorner, topCorner);
    } else {
        format &= ~MPP_FRAME_FBC_AFBC_V2;
    }

    mMppMpi->control(mMppCtx, MPP_DEC_SET_OUTPUT_FORMAT, (MppParam)&format);

    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, mWidth);
    mpp_frame_set_height(frame, mHeight);
    mpp_frame_set_fmt(frame, (MppFrameFormat)format);

    err = mMppMpi->control(mMppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);
    if (err != MPP_OK) {
        c2_err("failed to set frame info, err %d", err);
        mpp_frame_deinit(&frame);
        return C2_CORRUPTED;;
    }

    mHorStride   = mpp_frame_get_hor_stride(frame);
    mVerStride   = mpp_frame_get_ver_stride(frame);
    mColorFormat = mpp_frame_get_fmt(frame);
    mLeftCorner  = leftCorner;
    mTopCorner   = topCorner;

    mpp_frame_deinit(&frame);

    return C2_OK;
}

c2_status_t C2RKMpiDec::initDecoder(const std::unique_ptr<C2Work> &work) {
    c2_log_func_enter();

    MPP_RET err = mpp_create(&mMppCtx, &mMppMpi);
    if (err != MPP_OK) {
        c2_err("failed to mpp_create, ret %d", err);
        goto error;
    }

    {
        IntfImpl::Lock lock = mIntf->lock();

        uint32_t deinterlace = 1; // enable deinterlace, but not decting
        uint32_t splitMode = 0;
        uint32_t fastParse = C2RKChipCapDef::get()->getFastModeSupport(mCodingType);
        uint32_t fastPlay = 2; // 0: disable, 1: enable, 2: enable_once
        uint32_t fastOut = mIntf->getIsLowLatencyMode();
        uint32_t disableDpbCheck = mIntf->getIsDisableDpbCheck();
        uint32_t disableErrorMark = mIntf->getIsDisableErrorMark();

        // process feature requests configured via system lshal/dumpsys interface
        static const std::unordered_map<int32_t, std::function<void()>> featureHandlers = {
            { C2_FEATURE_DEC_DISABLE_DEINTERLACE, [&]() { deinterlace = 0; } },
            { C2_FEATURE_DEC_ENABLE_PARSER_SPLIT, [&]() { splitMode = 1; } },
            { C2_FEATURE_DEC_ENABLE_LOW_LATENCY,  [&]() { fastOut = 1; } },
            { C2_FEATURE_DEC_DISABLE_DPB_CHECK,   [&]() { disableDpbCheck = 1; } },
            { C2_FEATURE_DEC_DISABLE_ERROR_MARK,  [&]() { disableErrorMark = 1; } },
        };

        for (const auto& [flag, handler] : featureHandlers) {
            if (mDumpService->hasFeatures(flag)) handler();
        }

        // TODO: workaround: CTS-CodecDecoderTest
        // testFlushNative[15(c2.rk.mpeg2.decoder_video/mpeg2)
        if (mCodingType == MPP_VIDEO_CodingMPEG2) {
            deinterlace = 0;
            splitMode = 1;
        }

        mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_FAST_MODE, &fastParse);
        mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_FAST_PLAY, &fastPlay);

        if (!deinterlace) {
            mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &deinterlace);
            c2_info("disable deinterlace mode");
        }

        if (splitMode) {
            mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_SPLIT_MODE, &splitMode);
            c2_info("enable parser split mode");
        }

        if (fastOut) {
            mMppMpi->control(mMppCtx, MPP_DEC_SET_IMMEDIATE_OUT, &fastOut);
            c2_info("enable lowLatency fast-out mode");
        }

        if (disableDpbCheck) {
            mMppMpi->control(mMppCtx, MPP_DEC_SET_DISABLE_DPB_CHECK, &disableDpbCheck);
            c2_info("disable poc discontinuous check");
        }

        if (disableErrorMark) {
            mMppMpi->control(mMppCtx, MPP_DEC_SET_DISABLE_ERROR, &disableErrorMark);
            c2_info("disable error frame mark");
        }
    }

    err = mpp_init(mMppCtx, MPP_CTX_DEC, mCodingType);
    if (err != MPP_OK) {
        c2_err("failed to mpp_init, err %d", err);
        goto error;
    }

    /* update frame info to decoder */
    if (updateMppFrameInfo(getFbcOutputMode(work)) != C2_OK) {
        goto error;
    }

    if (!mDumpService->hasFeatures(C2_FEATURE_DEC_INTERNAL_BUFFER_GROUP)) {
        err = mpp_buffer_group_get_external(&mBufferGroup, MPP_BUFFER_TYPE_ION);
        if (err != MPP_OK) {
            c2_err("failed to get buffer_group, err %d", err);
            goto error;
        }

        err = mMppMpi->control(mMppCtx, MPP_DEC_SET_EXT_BUF_GROUP, mBufferGroup);
        if (err != MPP_OK) {
            c2_err("failed to set buffer group, err %d", err);
            goto error;
        }
    }

    {
        /* set output frame callback  */
        MppDecCfg cfg;
        mpp_dec_cfg_init(&cfg);
        mpp_dec_cfg_set_ptr(cfg, "cb:frm_rdy_cb", (void *)frameReadyCb);
        mpp_dec_cfg_set_ptr(cfg, "cb:frm_rdy_ctx", this);

        /* check HDR Vivid support */
        if (!mBufferMode && C2RKChipCapDef::get()->getHdrMetaCap()
                && !C2RKPropsDef::getHdrDisable()) {
            c2_info("enable hdr meta");
            mpp_dec_cfg_set_u32(cfg, "base:enable_hdr_meta", 1);
            mHdrMetaEnabled = true;
        }

        err = mMppMpi->control(mMppCtx, MPP_DEC_SET_CFG, cfg);
        if (err != MPP_OK) {
            c2_err("failed to set frame callback, err %d", err);
            goto error;
        }

        mpp_dec_cfg_deinit(cfg);
    }

    c2_info("init: w %d h %d coding %s", mWidth, mHeight, toStr_Coding(mCodingType));
    c2_info("init: hor %d ver %d color 0x%08x", mHorStride, mVerStride, mColorFormat);

    return C2_OK;

error:
    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    return C2_CORRUPTED;
}

void C2RKMpiDec::setMppPerformance(bool on) {
    int32_t width     = 1920;
    int32_t height    = 1080;
    int32_t byteHevc  = 0;
    int32_t byteColor = 8;

    width  = C2_MAX(width, mWidth);
    height = C2_MAX(height, mHeight);

    if (MPP_FRAME_FMT_IS_YUV_10BIT(mColorFormat)) {
        byteColor = 10;
    }
    if (mCodingType == MPP_VIDEO_CodingHEVC) {
        byteHevc = 1;
    }

    if (-1 == mFdPerf) {
        mFdPerf = open("/dev/video_state", O_WRONLY);
    }
    if (-1 == mFdPerf) {
        mFdPerf = open("/sys/class/devfreq/dmc/system_status", O_WRONLY);
        if (mFdPerf == -1) {
            c2_warn("failed to open /sys/class/devfreq/dmc/system_status");
        }
    }

    if (mFdPerf != -1) {
        char buffer[128] = { 0 };
        int32_t len = snprintf(buffer, sizeof(buffer),
                "%d,width=%d,height=%d,ishevc=%d,videoFramerate=%d,streamBitrate=%d",
                on, width, height, byteHevc, 0, byteColor);
        c2_info("config performance(%s, len=%d) of dmc driver", buffer, len);
        write(mFdPerf, buffer, len);
        if (!on) {
            close(mFdPerf);
            mFdPerf = -1;
        }
    }
}

void C2RKMpiDec::fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
    }
    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2RKMpiDec::finishWork(
        const std::unique_ptr<C2Work> &work, WorkEntry entry) {
    std::shared_ptr<C2Buffer> c2Buffer = nullptr;

    std::shared_ptr<C2GraphicBlock> block = entry.block;
    uint32_t flags = entry.flags;
    uint64_t timestamp = entry.timestamp;
    uint64_t frameIndex = entry.frameIndex;

    // stop work output in tunnel mode
    if (mTunneled) {
        return;
    }

    if (block) {
        c2Buffer = createGraphicBuffer(
                std::move(block), C2Rect(mWidth, mHeight).at(mLeftCorner, mTopCorner));

        if (mCodingType == MPP_VIDEO_CodingAVC ||
            mCodingType == MPP_VIDEO_CodingHEVC ||
            mCodingType == MPP_VIDEO_CodingAV1 ||
            mCodingType == MPP_VIDEO_CodingMPEG2) {
            IntfImpl::Lock lock = mIntf->lock();
            c2Buffer->setInfo(mIntf->getColorAspects_l());
        }
    }

    auto fillWork = [c2Buffer, flags, timestamp, this](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->worklets.front()->output.ordinal.timestamp = timestamp;

        if (c2Buffer) {
            work->worklets.front()->output.buffers.push_back(c2Buffer);
        }
        if (flags & WorkEntry::FLAGS_ERROR_FRAME) {
            c2_trace("finish empty error work, pts %lld", timestamp);
        }
        if (flags & WorkEntry::FLAGS_DROP_FRAME) {
            work->input.flags = C2FrameData::flags_t(
                    work->input.flags | C2FrameData::FLAG_DROP_FRAME);
            work->worklets.front()->output.flags = C2FrameData::flags_t(
                    work->worklets.front()->output.flags |C2FrameData::FLAG_DROP_FRAME);
        }
        if (flags & WorkEntry::FLAGS_INFO_CHANGE) {
            c2_info("update new size %dx%d config to framework.", mWidth, mHeight);
            C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
            C2PortActualDelayTuning::output delay(mNumOutputSlots - mSlotsToReduce);

            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(delay));

            if (mTunneled) {
                configTunneledPlayback(work);
             }
        }
        if (flags & WorkEntry::FLAGS_EOS) {
            c2_info("signalling eos");
            mOutputEOS = true;
            work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        }

        work->workletsProcessed = 1u;
        work->result = C2_OK;
    };

    if (work && (flags & WorkEntry::FLAGS_EOS)) {
        fillWork(work);
    } else {
        if (isPendingWorkExist(frameIndex)) {
            finish(frameIndex, fillWork);
        } else {
            // not present in the current peddingWorks, maybe interlaced video
            // source, sent through new work pipeline.
            std::unique_ptr<C2Work> work(new C2Work);
            work->worklets.clear();
            work->worklets.emplace_back(new C2Worklet);

            // work flags set to incomplete to ignore frame index check
            work->input.ordinal.frameIndex = OUTPUT_WORK_INDEX;
            work->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;

            finish(work, fillWork);
        }
    }
}

c2_status_t C2RKMpiDec::drainEOS(const std::unique_ptr<C2Work> &work) {
    if (mHandler) {
        mHandler->stop();
    }

    int64_t maxTimeUs = 2000000; /* 2s */
    int64_t startTimeUs = ALooper::GetNowUs();

    while (true) {
        if (C2_OK != drainWork(work)) {
            goto error;
        }

        if (mOutputEOS) {
            break;
        }

        if (ALooper::GetNowUs() - startTimeUs >= maxTimeUs) {
            c2_warn("failed to get output eos within 2 seconds");
            goto error;
        } else {
            usleep(1000);
        }
    }

    return C2_OK;

error:
    mSignalledError = true;
    work->workletsProcessed = 1u;
    work->result = C2_CORRUPTED;

    return C2_CORRUPTED;
}

c2_status_t C2RKMpiDec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    (void)drainMode;
    (void)pool;
    return C2_OK;
}

void C2RKMpiDec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err = C2_OK;

    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    // Initialize decoder if not already initialized
    if (!mStarted) {
        err = updateDecoderArgs(pool);
        if (err != C2_OK) {
            work->result = C2_BAD_VALUE;
            c2_info("failed to update args, signalled Error");
            return;
        }

        err = initDecoder(work);
        if (err != C2_OK) {
            work->result = C2_BAD_VALUE;
            c2_info("failed to initialize, signalled Error");
            return;
        }
        err = getSurfaceFeatures(pool);
        if (err == C2_OK) {
            c2_info("surface config: bufferMode %d graphicSource %d scaleMode %d",
                    mBufferMode, mGraphicSourceMode, mScaleMode);
        }
        if (mTunneled) {
            err = configTunneledPlayback(work);
            if (err != C2_OK) {
                work->result = C2_BAD_VALUE;
                c2_err("failed to configure tunneled playback, signalled Error");
                return;
            }
        }

        err = configOutputDelay(work);
        if (err != C2_OK) {
            c2_err("failed to config output delay");
            work->result = C2_BAD_VALUE;
            return;
        }

        // update alloc params once args updated
        updateAllocParams();

        // scene ddr frequency control
        setMppPerformance(true);

        mStarted = true;
    }

    if (mInputEOS || mSignalledError) {
        work->workletsProcessed = 1u;
        work->result = C2_CORRUPTED;
        return;
    }

    uint8_t *inData = nullptr;
    size_t inSize = 0u;
    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inData = const_cast<uint8_t *>(rView.data());
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            c2_err("failed to read rWiew, error %d", rView.error());
            work->result = rView.error();
            return;
        }
    }

    uint32_t flags = work->input.flags;
    uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();
    uint64_t timestamp = work->input.ordinal.timestamp.peekll();

    mInputEOS = ((flags & C2FrameData::FLAG_END_OF_STREAM) != 0);

    c2_trace("in buffer attr. size %zu timestamp %lld frameindex %lld, flags %x",
             inSize, timestamp, frameIndex, flags);

    if (mFlushed) {
        if (mBlockPool != pool) {
            err = updateDecoderArgs(pool);
            if (err != C2_OK) {
                mSignalledError = true;
                work->result = C2_CORRUPTED;
                return;
            }
        }

        err = ensureDecoderState();
        if (err != C2_OK) {
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
    }

    err = sendpacket(inData, inSize, timestamp, frameIndex, flags);
    if (err != C2_OK) {
        c2_err("failed to send packet, pts %lld", timestamp);
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        return;
    }

    if (mInputEOS) {
        drainEOS(work);
    } else if (!mStandardWorkFlow || (flags & C2FrameData::FLAG_CODEC_CONFIG)) {
        fillEmptyWork(work);
    }

    mFlushed = false;
}

void C2RKMpiDec::setDefaultCodecColorAspectsIfNeeded(ColorAspects &aspects) {
    typedef ColorAspects CA;
    CA::MatrixCoeffs matrix;

    static const ALookup<CA::Primaries, CA::MatrixCoeffs> sPMAspectMap = {
        {
            { CA::PrimariesUnspecified,   CA::MatrixUnspecified },
            { CA::PrimariesBT709_5,       CA::MatrixBT709_5 },
            { CA::PrimariesBT601_6_625,   CA::MatrixBT601_6 },
            { CA::PrimariesBT601_6_525,   CA::MatrixBT601_6 },
            { CA::PrimariesBT2020,        CA::MatrixBT2020 },
            { CA::PrimariesBT470_6M,      CA::MatrixBT470_6M },
        }
    };

    // dataspace supported lists: BT709 / BT601_6_625 / BT601_6_525 / BT2020.
    // so reset unsupport aspect here. For unassigned aspect, reassignment will
    // do later in frameworks.
    if (aspects.mMatrixCoeffs == CA::MatrixOther)
        aspects.mMatrixCoeffs = CA::MatrixUnspecified;
    if (!sPMAspectMap.map(aspects.mPrimaries, &matrix)) {
        c2_warn("reset unsupport primaries %s", asString(aspects.mPrimaries));
        aspects.mPrimaries = CA::PrimariesUnspecified;
    }

    if (aspects.mMatrixCoeffs == CA::MatrixUnspecified
            && aspects.mPrimaries != CA::PrimariesUnspecified) {
        sPMAspectMap.map(aspects.mPrimaries, &aspects.mMatrixCoeffs);
    } else if (aspects.mPrimaries == CA::PrimariesUnspecified
                   && aspects.mMatrixCoeffs != CA::MatrixUnspecified) {
        if (aspects.mMatrixCoeffs == CA::MatrixBT601_6) {
            if ((mWidth <= 720 && mHeight <= 480) || (mHeight <= 720 && mWidth <= 480)) {
                aspects.mPrimaries = CA::PrimariesBT601_6_525;
            } else {
                aspects.mPrimaries = CA::PrimariesBT601_6_625;
            }
        } else {
            if (!sPMAspectMap.map(aspects.mMatrixCoeffs, &aspects.mPrimaries)) {
                aspects.mMatrixCoeffs = CA::MatrixUnspecified;
            }
        }
    } else if (aspects.mPrimaries == CA::PrimariesBT601_6_625 ||
               aspects.mPrimaries == CA::PrimariesBT601_6_525) {
        // unadjusted standard is not allowed, update aspect to avoid get unsupport
        // StandardBT601_625_Unadjusted and StandardBT601_525_Unadjusted.
        if (aspects.mMatrixCoeffs == CA::MatrixBT709_5 ||
            aspects.mMatrixCoeffs == CA::MatrixSMPTE240M) {
            aspects.mMatrixCoeffs = CA::MatrixBT601_6;
        }
    }
}

void C2RKMpiDec::getVuiParams(MppFrame frame) {
    VuiColorAspects aspects;

    aspects.primaries = mpp_frame_get_color_primaries(frame);
    aspects.transfer  = mpp_frame_get_color_trc(frame);
    aspects.coeffs    = mpp_frame_get_colorspace(frame);
    if (mCodingType == MPP_VIDEO_CodingMPEG2) {
        aspects.fullRange = 0;
    } else {
        aspects.fullRange =
            (mpp_frame_get_color_range(frame) == MPP_FRAME_RANGE_JPEG);
    }

    // convert vui aspects to C2 values if changed
    if (!(aspects == mBitstreamColorAspects)) {
        mBitstreamColorAspects = aspects;
        ColorAspects sfAspects;
        C2StreamColorAspectsInfo::input codedAspects = { 0u };

        c2_info("Got vui color aspects, P(%d) T(%d) M(%d) R(%d)",
                aspects.primaries, aspects.transfer,
                aspects.coeffs, aspects.fullRange);

        ColorUtils::convertIsoColorAspectsToCodecAspects(
                aspects.primaries, aspects.transfer, aspects.coeffs,
                aspects.fullRange, sfAspects);

        setDefaultCodecColorAspectsIfNeeded(sfAspects);

        if (!C2Mapper::map(sfAspects.mPrimaries, &codedAspects.primaries)) {
            codedAspects.primaries = C2Color::PRIMARIES_UNSPECIFIED;
        }
        if (!C2Mapper::map(sfAspects.mRange, &codedAspects.range)) {
            codedAspects.range = C2Color::RANGE_UNSPECIFIED;
        }
        if (!C2Mapper::map(sfAspects.mMatrixCoeffs, &codedAspects.matrix)) {
            codedAspects.matrix = C2Color::MATRIX_UNSPECIFIED;
        }
        if (!C2Mapper::map(sfAspects.mTransfer, &codedAspects.transfer)) {
            codedAspects.transfer = C2Color::TRANSFER_UNSPECIFIED;
        }

        std::vector<std::unique_ptr<C2SettingResult>> failures;
        mIntf->config({&codedAspects}, C2_MAY_BLOCK, &failures);

        c2_info("set colorAspects (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s))",
                sfAspects.mRange, asString(sfAspects.mRange),
                sfAspects.mPrimaries, asString(sfAspects.mPrimaries),
                sfAspects.mMatrixCoeffs, asString(sfAspects.mMatrixCoeffs),
                sfAspects.mTransfer, asString(sfAspects.mTransfer));
    }
}

c2_status_t C2RKMpiDec::updateFbcModeIfNeeded() {
    c2_status_t err   = C2_OK;
    bool nowIsFbcMode = MPP_FRAME_FMT_IS_FBC(mColorFormat);
    bool dstIsFbcMode = (getFbcOutputMode() != 0);

    if (nowIsFbcMode != dstIsFbcMode) {
        c2_info("update use mpp %s output mode", dstIsFbcMode ? "fbc" : "non-fbc");
        err = updateMppFrameInfo(dstIsFbcMode);
    }

    return err;
}

c2_status_t C2RKMpiDec::importBufferToDecoder(std::shared_ptr<C2GraphicBlock> block) {
    auto c2Handle = block->handle();
    buffer_handle_t handle = nullptr;

    status_t err = C2RKGraphicBufferMapper::get()->importBuffer(c2Handle, &handle);
    if (err != OK) {
        return C2_CORRUPTED;
    }

    int32_t bufferFd = c2Handle->data[0];
    int32_t bufferId = C2RKGraphicBufferMapper::get()->getBufferId(handle);

    std::shared_ptr<OutBuffer> outBuffer = findOutBuffer(bufferId);
    if (outBuffer) {
        /* reuse this buffer */
        outBuffer->updateBlock(std::move(block));
        outBuffer->submitToDecoder();

        c2_trace("reuse this buffer, bufferId %d", bufferId);
    } else {
        /* register this buffer to decoder */
        MppBuffer mppBuffer;

        MppBufferInfo bufferInfo {
            .type  = MPP_BUFFER_TYPE_ION,
            .size  = (size_t)(C2RKGraphicBufferMapper::get()->getAllocationSize(handle)),
            .fd    = bufferFd,
            .index = bufferId,
            .ptr   = nullptr,
            .hnd   = nullptr
        };

        mpp_buffer_import_with_tag(
                mBufferGroup, &bufferInfo, &mppBuffer, "codec2", __FUNCTION__);

        std::shared_ptr<OutBuffer> newBuffer =
                std::make_shared<OutBuffer>(bufferId, bufferInfo.size, mppBuffer, block);

        // signal buffer available to decoder
        newBuffer->submitToDecoder();

        if (mTunneled) {
            mTunneledSession->newBuffer(
                    UnwrapNativeCodec2GrallocHandle(c2Handle), bufferId);

            // a set of buffer are pre-reserved to surface for smooth
            if (mTunneledSession->isReservedBuffer(bufferId)) {
                newBuffer->setInusedByClient();
            }
        }

        mBuffers.insert(std::make_pair(bufferId, std::move(newBuffer)));

        c2_trace("import this buffer, bufferId %d size %d listSize %d",
                 bufferId, bufferInfo.size, mBuffers.size());
    }

    /* check use scale meta when chip feature support it */
    if (C2RKChipCapDef::get()->getScaleMode() == C2_SCALE_MODE_META)
        checkUseScaleMeta(handle);

    C2RKGraphicBufferMapper::get()->freeBuffer(handle);

    return C2_OK;
}

c2_status_t C2RKMpiDec::ensureTunneledState() {
    c2_status_t err = C2_OK;
    std::shared_ptr<OutBuffer> outBuffer = nullptr;
    int32_t fetch = mTunneledSession->getNeedDequeueCnt();

    if (fetch <= 0) return err;

    c2_trace("required dequeue %d tunnel buffers", fetch);

    for (int32_t i = 0; i < fetch; i++) {
        int32_t bufferId = -1;
        if (mTunneledSession->dequeueBuffer(&bufferId)) {
            outBuffer = findOutBuffer(bufferId);
            if (outBuffer) {
                outBuffer->submitToDecoder();
            } else {
                c2_err("found unexpected buffer, index %d", bufferId);
                err = C2_CORRUPTED;
            }
        }
    }

    return err;
}

c2_status_t C2RKMpiDec::ensureDecoderState() {
    c2_status_t err = C2_OK;

    if (isPendingFlushing()) {
        c2_trace("NO NEED ensure, in pending flush");
        return err;
    }

    Mutex::Autolock autoLock(mBufferLock);

    if (mTunneled && !mBuffers.empty()) {
        return ensureTunneledState();
    }

    int32_t width  = mAllocParams.width;
    int32_t height = mAllocParams.height;
    int64_t usage  = mAllocParams.usage;
    int32_t format = mAllocParams.format;

    if (mBufferMode) {
        int32_t bWidth  = C2_ALIGN(mWidth, 2);
        int32_t bHeight = C2_ALIGN(mHeight, 2);
        int32_t bFormat = MPP_FRAME_FMT_IS_YUV_10BIT(mColorFormat) ? mPixelFormat : format;
        int64_t bUsage  = kCpuReadWriteUsage;

        // use cachable memory for higher cpu-copy performance
        usage |= kCpuReadWriteUsage;

        // allocate buffer within 4G to avoid rga2 error.
        if (C2RKChipCapDef::get()->hasRga2()) {
            bUsage |= RK_GRALLOC_USAGE_WITHIN_4G;
        }

        if (mOutBlock &&
                (mOutBlock->width() != bWidth || mOutBlock->height() != bHeight)) {
            mOutBlock.reset();
        }
        if (!mOutBlock) {
            err = mBlockPool->fetchGraphicBlock(bWidth, bHeight, bFormat,
                                                C2AndroidMemoryUsage::FromGrallocUsage(bUsage),
                                                &mOutBlock);
            if (err != C2_OK) {
                c2_err("failed to fetchGraphicBlock, err %d usage 0x%llx", err, bUsage);
                return err;
            }
        }
    }

    if (mBufferGroup) {
        size_t sizeOwnedByDecoder = std::count_if(
            mBuffers.begin(),  mBuffers.end(),
            [](const auto& value) {
                return value.second->ownedByDecoder();
            });

        std::shared_ptr<C2GraphicBlock> block;
        int32_t fetch = mNumOutputSlots - sizeOwnedByDecoder + 1;

        if (mTunneled) {
            fetch += mTunneledSession->getSmoothnessFactor();
        }

        int32_t i = 0;
        for (i = 0; i < fetch; i++) {
            err = mBlockPool->fetchGraphicBlock(width, height, format,
                                                C2AndroidMemoryUsage::FromGrallocUsage(usage),
                                                &block);
            if (err != C2_OK) {
                c2_err("failed to fetchGraphicBlock, err %d", err);
                break;
            }

            err = importBufferToDecoder(std::move(block));
            if (err != C2_OK) {
                c2_err("failed to commit buffer");
                break;
            }
        }

        if (err != C2_OK || fetch > 2) {
            c2_info("required (%dx%d) usage 0x%llx format 0x%x, fetch %d/%d",
                    width, height, usage, format, i, fetch);
        }
    }

    return err;
}

void C2RKMpiDec::postFrameReady() {
    if (mHandler) {
        sp<AMessage> msg = new AMessage(WorkHandler::kWhatFrameReady, mHandler);
        msg->setPointer("thiz", this);
        msg->post();
    }
}

c2_status_t C2RKMpiDec::drainWork(const std::unique_ptr<C2Work> &work) {
    if (mSignalledError) return C2_BAD_STATE;

    WorkEntry entry;
    memset(&entry, 0, sizeof(entry));

outframe:
    c2_status_t err = getoutframe(&entry);
    if (err == C2_OK) {
        finishWork(work, entry);

        err = ensureDecoderState();
        if (err != C2_OK) {
            goto error;
        }
    } else if (err == C2_NO_MEMORY) {
        err = ensureDecoderState();
        if (err == C2_OK) {
            // feekback config update to first output frame.
            entry.flags |= WorkEntry::FLAGS_INFO_CHANGE;
            goto outframe;
        } else {
            goto error;
        }
    } else if (err == C2_CORRUPTED) {
        goto error;
    }

    return C2_OK;

error:
    c2_err("signalling error");
    mSignalledError = true;

    return C2_CORRUPTED;
}

c2_status_t C2RKMpiDec::sendpacket(
        uint8_t *data, size_t size, uint64_t pts,
        uint64_t frameIndex, uint32_t flags) {
    c2_status_t ret = C2_OK;
    MppPacket packet = nullptr;

    mpp_packet_init(&packet, data, size);
    mpp_packet_set_pts(packet, pts);
    mpp_packet_set_pos(packet, data);
    mpp_packet_set_length(packet, size);
    // non-zero dts after decoding validates this method, so
    // we will never set dts to 0.
    // FIXME: better way to pass frameIndex.
    mpp_packet_set_dts(packet, _toDts(frameIndex));

    if (flags & C2FrameData::FLAG_END_OF_STREAM) {
        c2_info("send input eos");
        mpp_packet_set_eos(packet);
    }

    if (flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mpp_packet_set_extra_data(packet);
    } else if (flags & C2FrameData::FLAG_DROP_FRAME) {
        mDropFrames.push_back(pts);
    }

    /* dump frame time consuming if neccessary */
    mDumpService->recordFrameTime(this, pts);

    static uint32_t kMaxRetryCnt = 1000;
    uint32_t retry = 0;

    while (true) {
        MPP_RET err = mMppMpi->decode_put_packet(mMppCtx, packet);
        if (err == MPP_OK) {
            c2_trace("send packet pts %lld size %d", pts, size);
            /* record input packet buffer */
            bool skipStats = (flags & C2FrameData::FLAG_CODEC_CONFIG);
            mDumpService->recordFrame(this, data, size, skipStats);
            break;
        }

        if (mSignalledError || ((++retry) > kMaxRetryCnt)) {
            ret = C2_CORRUPTED;
            break;
        } else if (retry % 200 == 0) {
            /*
             * FIXME:
             * When player get paused, fetchGraphicBlock may get blocked since
             * surface fence paused. In this case, we are not able to output
             * frame and then the input process stucked also.
             *
             * To solve this issue, we attempt to send packet continuely when
             * fetchGraphicBlock get blocked. I still don't know is there a better
             * way to know player get paused?
             */
            if (mBufferLock.tryLock() == NO_ERROR) {
                c2_warn("try to resend packet, pts %lld", pts);
                mBufferLock.unlock();
            } else {
                retry = 0;
            }
        }

        usleep(3 * 1000);
    }

    mpp_packet_deinit(&packet);

    return ret;
}

c2_status_t C2RKMpiDec::getoutframe(WorkEntry *entry) {
    c2_status_t ret = C2_OK;
    MPP_RET     err = MPP_OK;
    MppFrame  frame = nullptr;

    err = mMppMpi->decode_get_frame(mMppCtx, &frame);
    if (err != MPP_OK || frame == nullptr) {
        return C2_NOT_FOUND;
    }

    int32_t width    = mpp_frame_get_width(frame);
    int32_t height   = mpp_frame_get_height(frame);
    int32_t hstride  = mpp_frame_get_hor_stride(frame);
    int32_t vstride  = mpp_frame_get_ver_stride(frame);
    int32_t error    = mpp_frame_get_errinfo(frame);
    int32_t discard  = mpp_frame_get_discard(frame);
    int32_t eos      = mpp_frame_get_eos(frame);
    int64_t pts      = mpp_frame_get_pts(frame);
    int64_t dts      = mpp_frame_get_dts(frame);
    int32_t mode     = mpp_frame_get_mode(frame);
    int64_t frameIdx = _toFrameIndex(dts);
    int32_t flags    = 0;
    int32_t bufferId = 0;

    MppFrameFormat format = mpp_frame_get_fmt(frame);
    MppBuffer mppBuffer = mpp_frame_get_buffer(frame);

    if (mStandardWorkFlow) {
        if ((mode & MPP_FRAME_FLAG_IEP_DEI_MASK) || (!eos && !dts)) {
            c2_info("fallback non-standard workflow");
            mStandardWorkFlow = false;
            flushPeddingWorks();
        }
    }

    if (mpp_frame_get_info_change(frame)) {
        c2_info("info-change with old dimensions(%dx%d) stride(%dx%d) fmt 0x%llx", \
                mWidth, mHeight, mHorStride, mVerStride, mColorFormat);
        c2_info("info-change with new dimensions(%dx%d) stride(%dx%d) fmt 0x%llx", \
                width, height, hstride, vstride, format);

        if (width > kMaxVideoWidth || height > kMaxVideoWidth) {
            c2_err("unsupport video size %dx%d, signalled Error.", width, height);
            ret = C2_CORRUPTED;
            goto cleanUp;
        }

        Mutex::Autolock autoLock(mBufferLock);

        releaseAllBuffers();

        mWidth = width;
        mHeight = height;
        mHorStride = hstride;
        mVerStride = vstride;
        mColorFormat = format;

        // support fbc mode change on info change stage
        updateFbcModeIfNeeded();

        // All buffer group config done. Set info change ready to let
        // decoder continue decoding.
        mMppMpi->control(mMppCtx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);

        C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        ret = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err("failed to set width and height");
            ret = C2_CORRUPTED;
            goto cleanUp;
        }

        ret = configOutputDelay();
        if (ret != C2_OK) {
            c2_err("failed to update output delay");
            ret = C2_CORRUPTED;
            goto cleanUp;
        }

        // update alloc params once args updated
        updateAllocParams();

        // update node params of service
        mDumpService->updateNode(this, mWidth, mHeight);

        ret = C2_NO_MEMORY;

        goto cleanUp;
    }

    if (eos) {
        c2_info("get output eos");
        flags |= WorkEntry::FLAGS_EOS;
        // ignore null frame with eos
        if (!mppBuffer) goto cleanUp;
    }

    if (isDropFrame(pts)) {
        c2_warn("skip drop frame with pts %lld", pts);
        flags |= WorkEntry::FLAGS_DROP_FRAME;
        /* record drop output frame */
        mDumpService->recordFrame(this, kDropFrame);
        goto cleanUp;
    }

    if (error || discard) {
        c2_warn("skip error frame with pts %lld", pts);
        flags |= WorkEntry::FLAGS_ERROR_FRAME;
        /* record error output frame */
        mDumpService->recordFrame(this, kErrorFrame);
        goto cleanUp;
    }

    if (isPendingFlushing()) {
        c2_trace("ignore frame output since pending flush");
        goto cleanUp;
    }

    bufferId = mpp_buffer_get_index(mppBuffer);

    c2_trace("get one frame [%d:%d] stride [%d:%d] pts %lld err %d bufferId %d",
             width, height, hstride, vstride, pts, error, bufferId);

    if (mBufferMode) {
        Mutex::Autolock autoLock(mBufferLock);

        auto c2Handle = mOutBlock->handle();
        int32_t srcFd = mpp_buffer_get_fd(mppBuffer);
        int32_t dstFd = c2Handle->data[0];

        int32_t srcFmt = MPP_FRAME_FMT_IS_YUV_10BIT(format) ?
                         HAL_PIXEL_FORMAT_YCrCb_NV12_10 :
                         HAL_PIXEL_FORMAT_YCrCb_NV12;
        int32_t dstFmt = mPixelFormat == HAL_PIXEL_FORMAT_YCBCR_P010 ?
                         HAL_PIXEL_FORMAT_YCBCR_P010 :
                         HAL_PIXEL_FORMAT_YCrCb_NV12;

        C2GraphicView dstView = mOutBlock->map().get();
        if (dstView.error()) {
            c2_err("unexpected map error %d", dstView.error());
            ret = C2_CORRUPTED;
            goto cleanUp;
        }

        int32_t dstStride = dstView.layout().planes[C2PlanarLayout::PLANE_Y].rowInc;
        int32_t dstVStride = C2_ALIGN(height, 2);

        if (mUseRgaBlit) {
            RgaInfo srcInfo, dstInfo;

            C2RKRgaDef::SetRgaInfo(
                    &srcInfo, srcFd, srcFmt, width, height, hstride, vstride);
            C2RKRgaDef::SetRgaInfo(
                    &dstInfo, dstFd, dstFmt, width, height, dstStride, dstVStride);
            if (!C2RKRgaDef::DoBlit(srcInfo, dstInfo)) {
                mUseRgaBlit = false;
                c2_warn("failed RGA blit, fallback software copy");
            }
        }

        // fallback software copy
        if (!mUseRgaBlit) {
            uint8_t *srcPtr = (uint8_t*)mpp_buffer_get_ptr(mppBuffer);
            uint8_t *dstPtr = (uint8_t*)(dstView.data()[C2PlanarLayout::PLANE_Y]);

            C2RKMediaUtils::translateToRequestFmt(
                    { srcPtr, srcFd, srcFmt, width, height, hstride, vstride },
                    { dstPtr, dstFd, dstFmt, width, height, dstStride, dstVStride },
                    true /* cache sync */);
        }

        entry->block = std::move(mOutBlock);
    } else {
        std::shared_ptr<OutBuffer> outBuffer = findOutBuffer(bufferId);
        if (!outBuffer) {
            ret = C2_CORRUPTED;
            c2_err("get outdated mppBuffer %p", mppBuffer);
            goto cleanUp;
        }

        // scale/hdr frame meta config
        configFrameMetaIfNeeded(frame, outBuffer->getBlock());

        // signal buffer occupied by client
        outBuffer->setInusedByClient();

        if (mTunneled) {
            mTunneledSession->renderBuffer(bufferId);
        }

        entry->block = outBuffer->takeBlock();
    }

    if (mCodingType == MPP_VIDEO_CodingAVC ||
        mCodingType == MPP_VIDEO_CodingHEVC ||
        mCodingType == MPP_VIDEO_CodingAV1 ||
        mCodingType == MPP_VIDEO_CodingMPEG2) {
        getVuiParams(frame);
    }

    {
        /* record output frame buffer */
        void *dumpData = nullptr;
        if (mDumpService->hasDebugFlags(C2_DUMP_RECORD_DECODE_OUTPUT)) {
            dumpData = mpp_buffer_get_ptr(mppBuffer);
        }
        mDumpService->recordFrame(this, dumpData, hstride, vstride, format);
        mDumpService->showFrameTiming(this, pts);
    }

cleanUp:
    entry->flags |= flags;
    entry->timestamp = pts;
    entry->frameIndex = frameIdx;

    if (frame) {
        mpp_frame_deinit(&frame);
        frame = nullptr;
    }

    return ret;
}

void C2RKMpiDec::releaseAllBuffers() {
    for (auto &pair : mBuffers) {
        if (!pair.second->ownedByDecoder()) {
            pair.second->submitToDecoder();
        }
    }
    mBuffers.clear();

    if (mBufferGroup) {
        mpp_buffer_group_clear(mBufferGroup);
    }
    if (mOutBlock) {
        mOutBlock.reset();
    }
    if (mTunneled) {
        mTunneledSession->reset();
    }
}

std::shared_ptr<C2RKMpiDec::OutBuffer> C2RKMpiDec::findOutBuffer(int32_t bufferId) {
    auto it = mBuffers.find(bufferId);
    if (it != mBuffers.end()) {
        return it->second;
    }
    return nullptr;
}

c2_status_t C2RKMpiDec::configFrameMetaIfNeeded(
        MppFrame frame, std::shared_ptr<C2GraphicBlock> block) {
    if (!mScaleMode && !mHdrMetaEnabled) {
        return C2_OK;
    }

    MppMeta meta = mpp_frame_get_meta(frame);

    int32_t scaleYOffset  = 0;
    int32_t scaleUVOffset = 0;
    int32_t hdrMetaOffset = 0;
    int32_t hdrMetaSize   = 0;

    if (mScaleMode && mpp_frame_get_thumbnail_en(frame)) {
        mpp_meta_get_s32(meta, KEY_DEC_TBN_Y_OFFSET,  &scaleYOffset);
        mpp_meta_get_s32(meta, KEY_DEC_TBN_UV_OFFSET, &scaleUVOffset);

        if (!scaleYOffset || !scaleUVOffset) {
            c2_err("unexpected scale offset meta");
            return C2_CORRUPTED;
        }
    }

    if (mHdrMetaEnabled && MPP_FRAME_FMT_IS_HDR(mpp_frame_get_fmt(frame))) {
        mpp_meta_get_s32(meta, KEY_HDR_META_OFFSET, &hdrMetaOffset);
        mpp_meta_get_s32(meta, KEY_HDR_META_SIZE,   &hdrMetaSize);

        if (!hdrMetaOffset || !hdrMetaSize) {
            c2_err("unexpected hdr offset meta");
            return C2_CORRUPTED;
        }
    }

    buffer_handle_t handle = nullptr;
    auto c2Handle = block->handle();

    status_t err = C2RKGraphicBufferMapper::get()->importBuffer(c2Handle, &handle);
    if (err != OK) {
        c2_err("failed to import graphic buffer");
        return C2_CORRUPTED;
    }

    if (mScaleMode == C2_SCALE_MODE_META) {
        C2PreScaleParam scaleParam;
        memset(&scaleParam, 0, sizeof(C2PreScaleParam));

        scaleParam.thumbWidth     = mpp_frame_get_width(frame) >> 1;
        scaleParam.thumbHeight    = mpp_frame_get_height(frame) >> 1;
        scaleParam.thumbHorStride = C2_ALIGN(mHorStride >> 1, 16);
        scaleParam.yOffset        = scaleYOffset;
        scaleParam.uvOffset       = scaleUVOffset;

        if (MPP_FRAME_FMT_IS_YUV_10BIT(mpp_frame_get_fmt(frame))) {
            scaleParam.format = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
        } else {
            scaleParam.format = HAL_PIXEL_FORMAT_YCrCb_NV12;
        }
        C2RKVdecExtendFeature::configFrameScaleMeta(handle, &scaleParam);

        memcpy((void *)&c2Handle->data, (void *)&handle->data,
               sizeof(int) * (handle->numFds + handle->numInts));
    }

    if (mHdrMetaEnabled) {
        C2RKVdecExtendFeature::configFrameHdrDynamicMeta(handle, hdrMetaOffset);
    }

    C2RKGraphicBufferMapper::get()->freeBuffer(handle);
    return C2_OK;
}

class C2RKMpiDecFactory : public C2ComponentFactory {
public:
    explicit C2RKMpiDecFactory(std::string name)
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
                new C2RKMpiDec(
                        mComponentName.c_str(),
                        mMime.c_str(),
                        id,
                        std::make_shared<C2RKMpiDec::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new C2RKInterface<C2RKMpiDec::IntfImpl>(
                        mComponentName.c_str(),
                        id,
                        std::make_shared<C2RKMpiDec::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual ~C2RKMpiDecFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
    std::string mComponentName;
    std::string mMime;
    C2Component::kind_t mKind;
    C2Component::domain_t mDomain;
};

C2ComponentFactory* CreateRKMpiDecFactory(std::string componentName) {
    return new ::android::C2RKMpiDecFactory(componentName);
}

}  // namespace android
