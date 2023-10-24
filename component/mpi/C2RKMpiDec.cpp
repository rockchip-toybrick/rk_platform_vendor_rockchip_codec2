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

#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <C2AllocatorGralloc.h>
#include <Codec2Mapper.h>
#include <media/stagefright/foundation/ALookup.h>

#include "hardware/hardware_rockchip.h"
#include "hardware/gralloc_rockchip.h"
#include "C2RKMpiDec.h"
#include "C2RKLog.h"
#include "C2RKMediaUtils.h"
#include "C2RKRgaDef.h"
#include "C2RKChipCapDef.h"
#include "C2RKColorAspects.h"
#include "C2RKNalParser.h"
#include "C2VdecExtendFeature.h"
#include "C2RKCodecMapper.h"
#include "C2RKExtendParam.h"
#include "C2RKMlvecLegacy.h"
#include "C2RKGrallocOps.h"
#include "C2RKVersion.h"

namespace android {

/* max support video resolution */
constexpr uint32_t kMaxVideoWidth = 8192;
constexpr uint32_t kMaxVideoHeight = 4320;

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
                .withDefault(new C2PortActualDelayTuning::output(C2_DEFAULT_OUTPUT_DELAY))
                .withFields({C2F(mActualOutputDelay, value).inRange(0, C2_MAX_OUTPUT_DELAY)})
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
                    C2F(mSize, width).inRange(2, kMaxVideoWidth, 2),
                    C2F(mSize, height).inRange(2, kMaxVideoWidth, 2),
                })
                .withSetter(SizeSetter)
                .build());

        addParameter(
                DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
                .withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
                .withFields({
                    C2F(mSize, width).inRange(2, kMaxVideoWidth, 2),
                    C2F(mSize, height).inRange(2, kMaxVideoWidth, 2),
                })
                .withSetter(MaxPictureSizeSetter, mSize)
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
                                C2Config::LEVEL_AVC_6, C2Config::LEVEL_AVC_6_1, C2Config::LEVEL_AVC_6_2})
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
                               C2Config::LEVEL_HEVC_HIGH_5_2, C2Config::LEVEL_HEVC_HIGH_6,
                               C2Config::LEVEL_HEVC_HIGH_6_1, C2Config::LEVEL_HEVC_HIGH_6_2})
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
                                C2Config::LEVEL_VP9_6,
                                C2Config::LEVEL_VP9_6_1,
                                C2Config::LEVEL_VP9_6_2})
                     })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_AV1) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_AV1_0, C2Config::LEVEL_AV1_7_3))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_AV1_0,
                                C2Config::PROFILE_AV1_0}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_AV1_2, C2Config::LEVEL_AV1_2_1, C2Config::LEVEL_AV1_2_2,
                                C2Config::LEVEL_AV1_2_3, C2Config::LEVEL_AV1_3, C2Config::LEVEL_AV1_3_1,
                                C2Config::LEVEL_AV1_3_2, C2Config::LEVEL_AV1_3_3, C2Config::LEVEL_AV1_4,
                                C2Config::LEVEL_AV1_4_1, C2Config::LEVEL_AV1_4_2, C2Config::LEVEL_AV1_4_3,
                                C2Config::LEVEL_AV1_5, C2Config::LEVEL_AV1_5_1, C2Config::LEVEL_AV1_5_2,
                                C2Config::LEVEL_AV1_5_3, C2Config::LEVEL_AV1_6, C2Config::LEVEL_AV1_6_1,
                                C2Config::LEVEL_AV1_6_2, C2Config::LEVEL_AV1_6_3, C2Config::LEVEL_AV1_7,
                                C2Config::LEVEL_AV1_7_1, C2Config::LEVEL_AV1_7_2, C2Config::LEVEL_AV1_7_3})
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

            addParameter(
                    DefineParam(mLowLatency, C2_PARAMKEY_LOW_LATENCY_MODE)
                    .withDefault(new C2GlobalLowLatencyModeTuning(false))
                    .withFields({C2F(mLowLatency, value)})
                    .withSetter(Setter<decltype(*mLowLatency)>::NonStrictValueWithNoDeps)
                    .build());

            /* extend parameter definition */
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
        // assume compression ratio of 2
        me.set().value = c2_max((((maxSize.v.width + 63) / 64)
                * ((maxSize.v.height + 63) / 64) * 3072), kMinInputBufferSize);
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

    static C2R MLowLatenctyModeSetter(
        bool mayBlock, C2P<C2LowLatencyMode::output> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    std::shared_ptr<C2StreamPictureSizeInfo::output> getSize_l() {
        return mSize;
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

    std::shared_ptr<MlvecParams> getMlvecParams_l() {
        return mMlvecParams;
    }

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamBlockSizeInfo::output> mBlockSize;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2GlobalLowLatencyModeTuning> mLowLatency;
    std::shared_ptr<MlvecParams> mMlvecParams;
};

C2RKMpiDec::C2RKMpiDec(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mDump(nullptr),
      mMppCtx(nullptr),
      mMppMpi(nullptr),
      mCodingType(MPP_VIDEO_CodingUnused),
      mColorFormat(MPP_FMT_YUV420SP),
      mFrmGrp(nullptr),
      mWidth(0),
      mHeight(0),
      mHorStride(0),
      mVerStride(0),
      mGrallocVersion(C2RKChipCapDef::get()->getGrallocVersion()),
      mLastPts(-1),
      mStarted(false),
      mFlushed(true),
      mOutputEos(false),
      mSignalledInputEos(false),
      mSignalledError(false),
      mSizeInfoUpdate(false),
      mLowLatencyMode(false),
      mIsGBSource(false),
      mScaleEnabled(false),
      mBufferMode(false) {
    if (!C2RKMediaUtils::getCodingTypeFromComponentName(name, &mCodingType)) {
        c2_err("failed to get codingType from component %s", name);
    }

    sDecConcurrentInstances.fetch_add(1, std::memory_order_relaxed);

    c2_info("name: %s\r\nversion: %s", name, C2_GIT_BUILD_VERSION);
}

C2RKMpiDec::~C2RKMpiDec() {
    if (sDecConcurrentInstances.load() > 0) {
        sDecConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);
    }
    onRelease();
}

c2_status_t C2RKMpiDec::onInit() {
    c2_log_func_enter();

    c2_status_t ret = updateOutputDelay();
    if (ret != C2_OK) {
        c2_err("failed to update output delay, ret %d", ret);
    }

    return ret;
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
    c2_log_func_enter();

    mStarted = false;
    mIsGBSource = false;

    if (!mFlushed) {
        onFlush_sm();
    }

    if (mOutBlock) {
        mOutBlock.reset();
    }

    if (mDump != nullptr) {
        delete mDump;
        mDump = nullptr;
    }

    if (mFrmGrp != nullptr) {
        mpp_buffer_group_put(mFrmGrp);
        mFrmGrp = nullptr;
    }

    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }
}

c2_status_t C2RKMpiDec::onFlush_sm() {
    c2_status_t ret = C2_OK;

    c2_log_func_enter();

    if (!mFlushed) {
        mOutputEos = false;
        mSignalledInputEos = false;
        mSignalledError = false;

        clearOutBuffers();

        if (mFrmGrp) {
            mpp_buffer_group_clear(mFrmGrp);
        }

        if (mMppMpi) {
            mMppMpi->reset(mMppCtx);
        }

        mFlushed = true;
    }

    return ret;
}

c2_status_t C2RKMpiDec::updateOutputDelay() {
    c2_status_t err = C2_OK;
    uint32_t outputDelay = 0;
    C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
    C2StreamProfileLevelInfo::input profileLevel(0u, PROFILE_UNUSED, LEVEL_UNUSED);

    err = mIntf->query(
            { &size, &profileLevel },
            {},
            C2_DONT_BLOCK,
            nullptr);

    outputDelay = C2RKMediaUtils::calculateOutputDelay(
            size.width, size.height, mCodingType, profileLevel.level);

    c2_info("codec(%s) video(%dx%d) profile&level(%d %d) needs %d reference frames",
            toStr_Coding(mCodingType), size.width, size.height,
            profileLevel.profile, profileLevel.level, outputDelay);

    C2PortActualDelayTuning::output tuningOutputDelay(outputDelay);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    err = mIntf->config({&tuningOutputDelay}, C2_MAY_BLOCK, &failures);

    return err;
}

bool C2RKMpiDec::checkPreferFbcOutput(const std::unique_ptr<C2Work> &work) {
    if (mIsGBSource) {
        c2_info("get graphicBufferSource in, perfer non-fbc mode");
        return false;
    }

    if (mBufferMode) {
        c2_info("bufferMode perfer non-fbc mode");
        return false;
    }

    /* SMPTEST2084 = 6 */
    if (mTransfer == 6) {
        c2_info("get transfer SMPTEST2084, prefer fbc output mode");
        return true;
    }

    if (mProfile == PROFILE_AVC_HIGH_10 || mProfile == PROFILE_HEVC_MAIN_10) {
        c2_info("get 10bit profile, prefer fbc output mode");
        return true;
    }

    // kodi/photos/files does not transmit profile level(10bit etc) to C2, so
    // get bitDepth info from spspps in this case.
    if (work != nullptr && work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
        C2ReadView rView = mDummyReadView;
        if (!work->input.buffers.empty()) {
            rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
            if (!rView.error()) {
                uint8_t *inData = const_cast<uint8_t *>(rView.data());
                size_t inSize = rView.capacity();
                int32_t depth = C2RKNalParser::getBitDepth(inData, inSize, mCodingType);
                if (depth == 10) {
                    c2_info("get 10bit profile tag from spspps, prefer fbc output mode");
                    return true;
                }
            }
        }
    }

    if (mWidth * mHeight > 2304 * 1080) {
        return true;
    }

    return false;
}

bool C2RKMpiDec::checkSurfaceConfig(
        const std::shared_ptr<C2BlockPool> &pool, bool *isGBSource, bool *scaleEnable) {
    c2_status_t ret = C2_OK;

    uint64_t usage = RK_GRALLOC_USAGE_SPECIFY_STRIDE;
    std::shared_ptr<C2GraphicBlock> block;

    // alloc a temporary graphicBuffer to get surface features.
    ret = pool->fetchGraphicBlock(
            176, 144, HAL_PIXEL_FORMAT_YCrCb_NV12,
            C2AndroidMemoryUsage::FromGrallocUsage(usage), &block);
    if (ret != C2_OK) {
        c2_err("failed to fetchGraphicBlock, err %d", ret);
        return false;
    }

    uint64_t getUsage = 0;
    auto c2Handle = block->handle();
    native_handle_t *grallocHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

    getUsage = C2RKGrallocOps::get()->getUsage(grallocHandle);
    if (getUsage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
        *isGBSource = true;
    }

    if (C2RKChipCapDef::get()->getScaleMetaCap()
            && C2VdecExtendFeature::checkNeedScale(grallocHandle) == 1) {
        MppDecCfg cfg;
        mpp_dec_cfg_init(&cfg);
        mMppMpi->control(mMppCtx, MPP_DEC_GET_CFG, cfg);
        if (!mpp_dec_cfg_set_u32(cfg, "base:enable_thumbnail", 1)) {
            *scaleEnable = true;
        }
        mMppMpi->control(mMppCtx, MPP_DEC_SET_CFG, cfg);
        mpp_dec_cfg_deinit(cfg);
    }

    block.reset();
    native_handle_delete(grallocHandle);

    return true;
}

c2_status_t C2RKMpiDec::initDecoder(const std::unique_ptr<C2Work> &work) {
    MPP_RET err = MPP_OK;

    c2_log_func_enter();

    {
        IntfImpl::Lock lock = mIntf->lock();
        mWidth = mIntf->getSize_l()->width;
        mHeight = mIntf->getSize_l()->height;
        mPrimaries = (uint32_t)mIntf->getDefaultColorAspects_l()->primaries;
        mTransfer = (uint32_t)mIntf->getDefaultColorAspects_l()->transfer;
        mRange = (uint32_t)mIntf->getDefaultColorAspects_l()->range;
        mHalPixelFormat = mIntf->getPixelFormat_l()->value;
        if (mIntf->getLowLatency_l() != nullptr) {
            mLowLatencyMode = (mIntf->getLowLatency_l()->value > 0) ? true : false ;
        }
        if (!mLowLatencyMode && mIntf->getMlvecParams_l()->lowLatencyMode != nullptr) {
            mLowLatencyMode = (mIntf->getMlvecParams_l()->lowLatencyMode->enable != 0);
        }

        if (mIntf->getProfileLevel_l() != nullptr) {
            mProfile = (uint32_t)mIntf->getProfileLevel_l()->profile;
        }
    }

    c2_info("init: w %d h %d coding %s", mWidth, mHeight, toStr_Coding(mCodingType));

    err = mpp_create(&mMppCtx, &mMppMpi);
    if (err != MPP_OK) {
        c2_err("failed to mpp_create, ret %d", err);
        goto error;
    }

    // TODO: workround: CTS-CodecDecoderTest
    // testFlushNative[15(c2.rk.mpeg2.decoder_video/mpeg2)
    if (mCodingType == MPP_VIDEO_CodingMPEG2) {
        uint32_t vmode = 0, split = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &vmode);
        mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);
    } else {
        // enable deinterlace, but not decting
        uint32_t vmode = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &vmode);
    }

    {
        // enable fast mode,
        uint32_t fastParser = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_FAST_MODE, &fastParser);

        uint32_t disableErr = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_DISABLE_ERROR, &disableErr);
    }

    err = mpp_init(mMppCtx, MPP_CTX_DEC, mCodingType);
    if (err != MPP_OK) {
        c2_err("failed to mpp_init, ret %d", err);
        goto error;
    }

    {
        // enable fast-play mode, ignore the effect of B-frame.
        uint32_t fastPlay = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_FAST_PLAY, &fastPlay);

        if (mLowLatencyMode) {
            uint32_t deinterlace = 0, immediate = 1;
            c2_info("enable lowLatency, enable mpp immediate-out mode");
            mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &deinterlace);
            mMppMpi->control(mMppCtx, MPP_DEC_SET_IMMEDIATE_OUT, &immediate);
        }
    }

    {
        MppFrame frame  = nullptr;

        if (mProfile == PROFILE_AVC_HIGH_10 ||
            mProfile == PROFILE_HEVC_MAIN_10 ||
            (mBufferMode && mHalPixelFormat == HAL_PIXEL_FORMAT_YCBCR_P010)) {
            c2_info("setup 10Bit format with profile %d halPixelFmt %d",
                    mProfile, mHalPixelFormat);
            mColorFormat = MPP_FMT_YUV420SP_10BIT;
        }

        uint32_t mppFmt = mColorFormat;

        mFbcCfg.mode = C2RKChipCapDef::get()->getFbcOutputMode(mCodingType);
        if (mFbcCfg.mode && checkPreferFbcOutput(work)) {
            mppFmt |= MPP_FRAME_FBC_AFBC_V2;
            /* fbc decode output has padding inside, set crop before display */
            C2RKChipCapDef::get()->getFbcOutputOffset(
                    mCodingType, &mFbcCfg.paddingX, &mFbcCfg.paddingY);
            c2_info("use mpp fbc output mode, padding offset(%d, %d)",
                    mFbcCfg.paddingX, mFbcCfg.paddingY);
        } else {
            mFbcCfg.mode = 0;
        }

        mMppMpi->control(mMppCtx, MPP_DEC_SET_OUTPUT_FORMAT, (MppParam)&mppFmt);

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, mWidth);
        mpp_frame_set_height(frame, mHeight);
        mpp_frame_set_fmt(frame, (MppFrameFormat)mppFmt);
        mMppMpi->control(mMppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);

        mHorStride = mpp_frame_get_hor_stride(frame);
        mVerStride = mpp_frame_get_ver_stride(frame);
        mColorFormat = mpp_frame_get_fmt(frame);

        mpp_frame_deinit(&frame);

        c2_info("init: hor %d ver %d color 0x%08x", mHorStride, mVerStride, mColorFormat);
    }

    /*
     * For buffer mode, since we don't konw when the last buffer will use
     * up by user, so we use MPP internal buffer group, and copy output to
     * dst block(mOutBlock).
     */
    if (!mBufferMode) {
        err = mpp_buffer_group_get_external(&mFrmGrp, MPP_BUFFER_TYPE_ION);
        if (err != MPP_OK) {
            c2_err("failed to get buffer_group, err %d", err);
            goto error;
        }
        mMppMpi->control(mMppCtx, MPP_DEC_SET_EXT_BUF_GROUP, mFrmGrp);
    }

    if (!mDump) {
        // init dump object
        mDump = new C2RKDump();
        mDump->initDump(mHorStride, mVerStride, false);
    }

    mStarted = true;

    return C2_OK;

error:
    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    return C2_CORRUPTED;
}

void C2RKMpiDec::fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;

    c2_trace_func_enter();

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        c2_info("signalling eos");
    }

    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2RKMpiDec::finishWork(OutWorkEntry *entry) {
    if (!entry->outblock) {
        c2_err("empty block, finish work failed.");
        return;
    }

    uint32_t left = mFbcCfg.mode ? mFbcCfg.paddingX : 0;
    uint32_t top  = mFbcCfg.mode ? mFbcCfg.paddingY : 0;

    std::shared_ptr<C2Buffer> buffer
            = createGraphicBuffer(std::move(entry->outblock),
                                  C2Rect(mWidth, mHeight).at(left, top));

    mOutBlock = nullptr;

    {
        if (mCodingType == MPP_VIDEO_CodingAVC ||
            mCodingType == MPP_VIDEO_CodingHEVC ||
            mCodingType == MPP_VIDEO_CodingMPEG2) {
            IntfImpl::Lock lock = mIntf->lock();
            buffer->setInfo(mIntf->getColorAspects_l());
        }
    }

    auto fillWork = [buffer, entry](const std::unique_ptr<C2Work> &work) {
        // now output work is new work, frame index remove by input work,
        // output work set to incomplete to ignore frame index check
        work->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->worklets.front()->output.ordinal.timestamp = entry->timestamp;
        work->workletsProcessed = 1u;
    };

    std::unique_ptr<C2Work> outputWork(new C2Work);
    outputWork->worklets.clear();
    outputWork->worklets.emplace_back(new C2Worklet);
    outputWork->input.ordinal.timestamp = 0;
    outputWork->input.ordinal.frameIndex = OUTPUT_WORK_INDEX;
    outputWork->input.ordinal.customOrdinal = 0;
    outputWork->result = C2_OK;

    if (mSizeInfoUpdate) {
        c2_info("update new size %dx%d config to framework.", mWidth, mHeight);
        C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
        outputWork->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
        mSizeInfoUpdate = false;
    }

    finish(outputWork, fillWork);
}

c2_status_t C2RKMpiDec::drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work) {
    c2_log_func_enter();

    if (!mStarted) {
        c2_warn("decoder is not initialized: no-op");
        return C2_OK;
    }

    if (drainMode == NO_DRAIN) {
        c2_warn("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        c2_warn("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    c2_status_t ret = C2_OK;
    OutWorkEntry entry;
    uint32_t kMaxRetryNum = 20;
    uint32_t retry = 0;

    while (true){
        ret = ensureDecoderState(pool);
        if (ret != C2_OK && work) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return C2_CORRUPTED;
        }

        ret = getoutframe(&entry, false);
        if (ret == C2_OK && entry.outblock) {
            finishWork(&entry);
        } else if (drainMode == DRAIN_COMPONENT_NO_EOS && !work) {
            c2_info("drain without wait eos, done.");
            break;
        }

        if (mOutputEos && work) {
            fillEmptyWork(work);
            break;
        }

        if ((++retry) > kMaxRetryNum) {
            mOutputEos = true;
            c2_warn("drain: eos not found, force set output EOS.");
        } else {
            usleep(5 * 1000);
        }
    }

    c2_log_func_leave();

    return C2_OK;
}

c2_status_t C2RKMpiDec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    return drainInternal(drainMode, pool, nullptr);
}

void C2RKMpiDec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err = C2_OK;

    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    mBufferMode = (pool->getLocalId() <= C2BlockPool::PLATFORM_START);

    // Initialize decoder if not already initialized
    if (!mStarted) {
        err = initDecoder(work);
        if (err != C2_OK) {
            work->result = C2_BAD_VALUE;
            c2_info("failed to initialize, signalled Error");
            return;
        }
        if (checkSurfaceConfig(pool, &mIsGBSource, &mScaleEnabled)) {
            c2_info("surface config: surfaceMode %d isGBSource %d scaleEnable %d",
                    !mBufferMode, mIsGBSource, mScaleEnabled);
        }
        if (mIsGBSource)
            updateFbcModeIfNeeded();
    }

    if (mSignalledInputEos || mSignalledError) {
        work->result = C2_BAD_VALUE;
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

    c2_trace("in buffer attr. size %zu timestamp %lld frameindex %lld, flags %x",
             inSize, timestamp, frameIndex, flags);

    bool eos = ((flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    bool hasPicture = false;
    bool needGetFrame = false;
    bool sendPacketFlag = true;
    uint32_t outfrmCnt = 0;
    OutWorkEntry entry;

    if ((flags & C2FrameData::FLAG_CODEC_CONFIG) == 0) {
        // reset flush flag when get non-config frame.
        mFlushed = false;
    }

    err = ensureDecoderState(pool);
    if (err != C2_OK) {
        mSignalledError = true;
        work->workletsProcessed = 1u;
        work->result = C2_CORRUPTED;
        return;
    }

inPacket:
    needGetFrame   = false;
    sendPacketFlag = true;
    // may block, quit util enqueue success.
    err = sendpacket(inData, inSize, timestamp, flags);
    if (err != C2_OK) {
        c2_warn("failed to enqueue packet, pts %lld", timestamp);
        needGetFrame = true;
        sendPacketFlag = false;
    } else {
        if (!eos) {
            fillEmptyWork(work);
        }

        // TODO workround: CTS-CodecDecoderTest
        // testFlushNative[15(c2.rk.mpeg2.decoder_video/mpeg2)
        if (mLastPts != timestamp) {
            mLastPts = timestamp;
        }
    }

outframe:
    if (!eos) {
        err = getoutframe(&entry, needGetFrame);
        if (err == C2_OK) {
            outfrmCnt++;
            needGetFrame = false;
            hasPicture = true;
        } else if (err == C2_CORRUPTED) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        }
    }

    if (eos) {
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
        mSignalledInputEos = true;
    } else if (hasPicture) {
        finishWork(&entry);
        /* Avoid stock frame, continue to search available output */
        ensureDecoderState(pool);
        hasPicture = false;

        if (sendPacketFlag == false) {
            goto inPacket;
        }
        goto outframe;
    } else if (err == C2_NO_MEMORY) {
        // update new size config.
        C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        err = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
        if (err != OK) {
            c2_err("failed to set width and height");
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        }
        err = updateOutputDelay();
        if (err != C2_OK) {
            c2_err("failed to update output delay, ret %d", err);
            return;
        }
        ensureDecoderState(pool);
        // feekback config update to first output frame.
        mSizeInfoUpdate = true;
        goto outframe;
    } else if (outfrmCnt == 0) {
        usleep(1000);
        if (mLowLatencyMode && flags == 0) {
            goto outframe;
        }
    }
}

void C2RKMpiDec::setDefaultCodecColorAspectsIfNeeded(ColorAspects &aspects) {
    typedef ColorAspects CA;

    // reset unsupport other aspect
    if (aspects.mMatrixCoeffs == CA::MatrixOther)
        aspects.mMatrixCoeffs = CA::MatrixUnspecified;
    if (aspects.mPrimaries == CA::PrimariesOther)
        aspects.mPrimaries = CA::PrimariesUnspecified;

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
            sPMAspectMap.map(aspects.mMatrixCoeffs, &aspects.mPrimaries);
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
    uint32_t format = mColorFormat;
    bool needUpdate = false;
    bool preferFbc = checkPreferFbcOutput();

    if (!MPP_FRAME_FMT_IS_FBC(format)) {
        int32_t fbcMode = C2RKChipCapDef::get()->getFbcOutputMode(mCodingType);
        if (fbcMode && preferFbc) {
            format |= MPP_FRAME_FBC_AFBC_V2;
            mFbcCfg.mode = fbcMode;
            /* fbc decode output has padding inside, set crop before display */
            C2RKChipCapDef::get()->getFbcOutputOffset(
                    mCodingType, &mFbcCfg.paddingX, &mFbcCfg.paddingY);
            needUpdate = true;
            c2_info("change use mpp fbc output mode, padding offset(%d, %d)",
                    mFbcCfg.paddingX, mFbcCfg.paddingY);
        }
    } else {
        if (!preferFbc) {
            format &= ~MPP_FRAME_FBC_AFBC_V2;
            memset(&mFbcCfg, 0, sizeof(mFbcCfg));
            needUpdate = true;
            c2_info("change use mpp non-fbc output mode");
        }
    }

    if (needUpdate) {
        MppFrame frame = nullptr;

        mMppMpi->control(mMppCtx, MPP_DEC_SET_OUTPUT_FORMAT, (MppParam)&format);

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, mWidth);
        mpp_frame_set_height(frame, mHeight);
        mpp_frame_set_fmt(frame, (MppFrameFormat)format);
        mMppMpi->control(mMppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);

        mHorStride = mpp_frame_get_hor_stride(frame);
        mVerStride = mpp_frame_get_ver_stride(frame);
        mColorFormat = mpp_frame_get_fmt(frame);

        mpp_frame_deinit(&frame);
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::commitBufferToMpp(std::shared_ptr<C2GraphicBlock> block) {
    uint32_t bufferId = 0;
    auto c2Handle = block->handle();
    uint32_t fd = c2Handle->data[0];
    native_handle_t *grallocHandle = nullptr;

    grallocHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

    bufferId = C2RKGrallocOps::get()->getBufferId(grallocHandle);

    OutBuffer *buffer = findOutBuffer(bufferId);
    if (buffer) {
        /* commit this buffer back to mpp */
        MppBuffer mppBuffer = buffer->mppBuffer;
        if (mppBuffer) {
            mpp_buffer_put(mppBuffer);
        }
        buffer->block = block;
        buffer->site = BUFFER_SITE_BY_MPI;

        c2_trace("put this buffer, index %d fd %d mppBuf %p", bufferId, fd, mppBuffer);
    } else {
        /* register this buffer to mpp group */
        MppBuffer mppBuffer;
        MppBufferInfo info;
        memset(&info, 0, sizeof(info));

        info.type = MPP_BUFFER_TYPE_ION;
        info.fd = fd;
        info.ptr = nullptr;
        info.hnd = nullptr;
        info.size = C2RKGrallocOps::get()->getAllocationSize(grallocHandle);
        info.index = bufferId;

        mpp_buffer_import_with_tag(
                mFrmGrp, &info, &mppBuffer, "codec2", __FUNCTION__);

        OutBuffer *buffer = new OutBuffer;
        buffer->index = bufferId;
        buffer->mppBuffer = mppBuffer;
        buffer->block = block;
        buffer->site = BUFFER_SITE_BY_MPI;

        // signal buffer available to mpp
        mpp_buffer_put(mppBuffer);

        mOutBuffers.push(buffer);

        c2_trace("import this buffer, index %d fd %d size %d mppBuf %p listSize %d",
                 bufferId, fd, info.size, mppBuffer, mOutBuffers.size());
    }

    native_handle_delete(grallocHandle);

    return C2_OK;
}

c2_status_t C2RKMpiDec::ensureDecoderState(
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t ret = C2_OK;

    uint32_t blockW = mHorStride;
    uint32_t blockH = mVerStride;

    uint64_t usage  = RK_GRALLOC_USAGE_SPECIFY_STRIDE;
    uint32_t format = C2RKMediaUtils::colorFormatMpiToAndroid(mColorFormat, mFbcCfg.mode);

    if (mBufferMode && mHalPixelFormat == HAL_PIXEL_FORMAT_YCBCR_P010) {
        format = HAL_PIXEL_FORMAT_YCBCR_P010;
    }

    std::lock_guard<std::mutex> lock(mPoolMutex);

    // NOTE: private grallc align flag only support in gralloc 4.0.
    if (mGrallocVersion == 4 && !mFbcCfg.mode && !mIsGBSource) {
        blockW = mWidth;
        usage = C2RKMediaUtils::getStrideUsage(mWidth, mHorStride);

        blockH = mHeight;
        usage |= C2RKMediaUtils::getHStrideUsage(mHeight, mVerStride);
    }

    if (mFbcCfg.mode) {
        // NOTE: FBC case may have offset y on top and vertical stride
        // should aligned to 16.
        blockH = C2_ALIGN(mVerStride + mFbcCfg.paddingY, 16);

        // In fbc 10bit mode, treat width of buffer as pixer_stride.
        if (format == HAL_PIXEL_FORMAT_YUV420_10BIT_I ||
            format == HAL_PIXEL_FORMAT_Y210) {
            blockW = C2_ALIGN(mWidth, 64);
        }
    } else if (mCodingType == MPP_VIDEO_CodingVP9 && mGrallocVersion < 4) {
        // vp9 need odd 256 align
        blockW = C2_ALIGN_ODD(mWidth, 256);
    }

    switch(mTransfer) {
        case ColorTransfer::kColorTransferST2084:
            usage |= ((GRALLOC_NV12_10_HDR_10 << 24) & GRALLOC_COLOR_SPACE_MASK);  // hdr10;
            break;
        case ColorTransfer::kColorTransferHLG:
            usage |= ((GRALLOC_NV12_10_HDR_HLG << 24) & GRALLOC_COLOR_SPACE_MASK);  // hdr-hlg
            break;
    }

    switch (mPrimaries) {
        case C2Color::PRIMARIES_BT601_525:
            usage |= MALI_GRALLOC_USAGE_YUV_COLOR_SPACE_BT601;
            break;
        case C2Color::PRIMARIES_BT709:
            usage |= MALI_GRALLOC_USAGE_YUV_COLOR_SPACE_BT709;
            break;
    }
    switch (mRange) {
        case C2Color::RANGE_FULL:
            usage |= MALI_GRALLOC_USAGE_RANGE_WIDE;
            break;
        case C2Color::RANGE_LIMITED:
            usage |= MALI_GRALLOC_USAGE_RANGE_NARROW;
            break;
    }

    // only large than gralloc 4 can support int64 usage.
    // otherwise, gralloc 3 will check high 32bit is empty,
    // if not empty, will alloc buffer failed and return
    // error. So we need clear high 32 bit.
    if (mGrallocVersion < 4) {
        usage &= 0xffffffff;
    }
    if (mScaleEnabled) {
        usage |= GRALLOC_USAGE_RKVDEC_SCALING;
    }

    /*
     * For buffer mode, since we don't konw when the last buffer will use
     * up by user, so we use MPP internal buffer group, and copy output to
     * dst block(mOutBlock).
     */
    if (mBufferMode) {
        if (mOutBlock &&
                (mOutBlock->width() != blockW || mOutBlock->height() != blockH)) {
            mOutBlock.reset();
        }
        if (!mOutBlock) {
            usage |= (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
            ret = pool->fetchGraphicBlock(blockW, blockH, format,
                                          C2AndroidMemoryUsage::FromGrallocUsage(usage),
                                          &mOutBlock);
            if (ret != C2_OK) {
                c2_err("failed to fetchGraphicBlock, err %d usage 0x%llx", ret, usage);
                return ret;
            }
            c2_trace("required (%dx%d) usage 0x%llx format 0x%x , fetch done",
                     blockW, blockH, usage, format);
        }
    } else {
        std::shared_ptr<C2GraphicBlock> outblock;
        uint32_t count = mIntf->mActualOutputDelay->value - getOutBufferCountOwnByMpi();

        uint32_t i = 0;
        while (i < count) {
            ret = pool->fetchGraphicBlock(blockW, blockH, format,
                                          C2AndroidMemoryUsage::FromGrallocUsage(usage),
                                          &outblock);
            if (ret != C2_OK) {
                c2_err("failed to fetchGraphicBlock, err %d", ret);
                break;
            }

            if (outblock) {
                commitBufferToMpp(outblock);
                i++;
            }
        }

        c2_trace("required (%dx%d) usage 0x%llx format 0x%x, fetch %d/%d",
                 blockW, blockH, usage, format, i, count);
    }

    return ret;
}

c2_status_t C2RKMpiDec::sendpacket(uint8_t *data, size_t size, uint64_t pts, uint32_t flags) {
    c2_status_t ret = C2_OK;
    MppPacket packet = nullptr;

    mpp_packet_init(&packet, data, size);
    mpp_packet_set_pts(packet, pts);
    mpp_packet_set_pos(packet, data);
    mpp_packet_set_length(packet, size);

    if (flags & C2FrameData::FLAG_END_OF_STREAM) {
        c2_info("send input eos");
        mpp_packet_set_eos(packet);
    }

    if (flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mpp_packet_set_extra_data(packet);
    }

    MPP_RET err = MPP_OK;
    uint32_t kMaxRetryNum = 3;
    uint32_t retry = 0;

    while (true) {
        err = mMppMpi->decode_put_packet(mMppCtx, packet);
        if (err == MPP_OK) {
            c2_trace("send packet pts %lld size %d", pts, size);
            /* dump input data if neccessary */
            mDump->recordInFile(data, size);
            /* dump show input process fps if neccessary */
            mDump->showDebugFps(DUMP_ROLE_INPUT);
            break;
        }

        if ((++retry) > kMaxRetryNum) {
            ret = C2_CORRUPTED;
            break;
        }
        usleep(4 * 1000);
    }

    mpp_packet_deinit(&packet);

    return ret;
}

c2_status_t C2RKMpiDec::getoutframe(OutWorkEntry *entry, bool needGetFrame) {
    c2_status_t ret = C2_OK;
    MPP_RET err = MPP_OK;
    MppFrame frame = nullptr;

    uint64_t pts = 0;
    uint32_t tryCount = 0;
    std::shared_ptr<C2GraphicBlock> outblock = nullptr;

REDO:
    err = mMppMpi->decode_get_frame(mMppCtx, &frame);
    tryCount++;
    if (MPP_OK != err || !frame) {
        if (needGetFrame == true && tryCount < 10) {
            c2_info("need to get frame");
            usleep(5 * 1000);
            goto REDO;
        }
        return C2_NOT_FOUND;
    }

    uint32_t width  = mpp_frame_get_width(frame);
    uint32_t height = mpp_frame_get_height(frame);
    uint32_t hstride = mpp_frame_get_hor_stride(frame);
    uint32_t vstride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat format = mpp_frame_get_fmt(frame);

    if (mpp_frame_get_info_change(frame)) {
        c2_info("info-change with old dimensions(%dx%d) stride(%dx%d) fmt %d", \
                mWidth, mHeight, mHorStride, mVerStride, mColorFormat);
        c2_info("info-change with new dimensions(%dx%d) stride(%dx%d) fmt %d", \
                width, height, hstride, vstride, format);

        if (width > kMaxVideoWidth || height > kMaxVideoWidth) {
            c2_err("unsupport video size %dx%d, signalled Error.", width, height);
            ret = C2_CORRUPTED;
            goto exit;
        }

        if (!mBufferMode) {
            clearOutBuffers();
            mpp_buffer_group_clear(mFrmGrp);
        }

        mWidth = width;
        mHeight = height;
        mColorFormat = format;
        mHorStride = hstride;
        mVerStride = vstride;

        // support fbc mode change on info change stage
        updateFbcModeIfNeeded();

        /*
         * All buffer group config done. Set info change ready to let
         * decoder continue decoding
         */
        err = mMppMpi->control(mMppCtx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        if (err) {
            c2_err("failed to set info-change ready, ret %d", ret);
            ret = C2_CORRUPTED;
            goto exit;
        }

        ret = C2_NO_MEMORY;
    } else {
        uint32_t err = mpp_frame_get_errinfo(frame);
        uint32_t eos = mpp_frame_get_eos(frame);
        MppBuffer mppBuffer = mpp_frame_get_buffer(frame);
        pts = mpp_frame_get_pts(frame);

        c2_trace("get one frame [%d:%d] stride [%d:%d] pts %lld err %d eos %d",
                 width, height, hstride, vstride, pts, err, eos);

        if (eos) {
            c2_info("get output eos.");
            mOutputEos = true;
            // ignore null frame with eos
            if (!mppBuffer) goto exit;
        }

        if (mBufferMode) {
            if (mHalPixelFormat == HAL_PIXEL_FORMAT_YCBCR_P010) {
                C2GraphicView wView = mOutBlock->map().get();
                C2PlanarLayout layout = wView.layout();
                uint8_t *src = (uint8_t*)mpp_buffer_get_ptr(mppBuffer);
                uint8_t *dstY = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_Y]);
                uint8_t *dstUV = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_U]);
                size_t dstYStride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
                size_t dstUVStride = layout.planes[C2PlanarLayout::PLANE_U].rowInc;

                C2RKMediaUtils::convert10BitNV12ToP010(
                        dstY, dstUV, dstYStride, dstUVStride,
                        src, hstride, vstride, width, height);
            } else {
                RgaInfo srcInfo, dstInfo;
                int32_t srcFd = 0, dstFd = 0;

                auto c2Handle = mOutBlock->handle();
                srcFd = mpp_buffer_get_fd(mppBuffer);
                dstFd = c2Handle->data[0];

                C2RKRgaDef::SetRgaInfo(
                        &srcInfo, srcFd, mWidth, mHeight, mHorStride, mVerStride);
                C2RKRgaDef::SetRgaInfo(
                        &dstInfo, dstFd, mWidth, mHeight, mWidth, mHeight);
                if (!C2RKRgaDef::NV12ToNV12(srcInfo, dstInfo)) {
                    // use cpu copy if get rga error
                    uint8_t *srcPtr = (uint8_t*)mpp_buffer_get_ptr(mppBuffer);
                    uint8_t *dstPtr = mOutBlock->map().get().data()[C2PlanarLayout::PLANE_Y];
                    memcpy(dstPtr, srcPtr, mHorStride * mVerStride * 3 / 2);
                }
            }
            outblock = mOutBlock;
        } else {
            OutBuffer *outBuffer = findOutBuffer(mppBuffer);
            if (outBuffer) {
                mpp_buffer_inc_ref(mppBuffer);
            } else {
                c2_err("get outdated mppBuffer %p, release it.", mppBuffer);
                goto exit;
            }
            outBuffer->site = BUFFER_SITE_BY_C2;
            outblock = outBuffer->block;
        }

        if (mCodingType == MPP_VIDEO_CodingAVC ||
            mCodingType == MPP_VIDEO_CodingHEVC ||
            mCodingType == MPP_VIDEO_CodingMPEG2) {
            getVuiParams(frame);
        }

        if (mScaleEnabled) {
            configFrameScaleMeta(frame, outblock);
        }

        /* dump output data if neccessary */
        if (C2RKDump::getDumpFlag() & C2_DUMP_RECORD_DEC_OUT) {
            void *data = mpp_buffer_get_ptr(mppBuffer);
            mDump->recordOutFile(data, hstride, vstride, RAW_TYPE_YUV420SP);
        }

        /* dump show output process fps if neccessary */
        mDump->showDebugFps(DUMP_ROLE_OUTPUT);

        ret = C2_OK;
    }

exit:
    if (frame) {
        mpp_frame_deinit(&frame);
        frame = nullptr;
    }

    entry->outblock = outblock;
    entry->timestamp = pts;

    return ret;
}

c2_status_t C2RKMpiDec::configFrameScaleMeta(
        MppFrame frame, std::shared_ptr<C2GraphicBlock> block) {
    if (block && block->handle()
            && mpp_frame_has_meta(frame) && mpp_frame_get_thumbnail_en(frame)) {
        MppMeta meta = NULL;
        int32_t scaleYOffset = 0;
        int32_t scaleUVOffset = 0;
        int32_t width = 0, height = 0;
        MppFrameFormat format;
        C2PreScaleParam scaleParam;

        memset(&scaleParam, 0, sizeof(C2PreScaleParam));

        native_handle_t *nHandle = UnwrapNativeCodec2GrallocHandle(block->handle());

        width  = mpp_frame_get_width(frame);
        height = mpp_frame_get_height(frame);
        format = mpp_frame_get_fmt(frame);
        meta   = mpp_frame_get_meta(frame);

        mpp_meta_get_s32(meta, KEY_DEC_TBN_Y_OFFSET, &scaleYOffset);
        mpp_meta_get_s32(meta, KEY_DEC_TBN_UV_OFFSET, &scaleUVOffset);

        scaleParam.thumbWidth = width >> 1;
        scaleParam.thumbHeight = height >> 1;
        scaleParam.thumbHorStride = C2_ALIGN(mHorStride >> 1, 16);
        scaleParam.yOffset = scaleYOffset;
        scaleParam.uvOffset = scaleUVOffset;
        if ((format & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP_10BIT) {
            scaleParam.format = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
        } else {
            scaleParam.format = HAL_PIXEL_FORMAT_YCrCb_NV12;
        }
        C2VdecExtendFeature::configFrameScaleMeta(nHandle, &scaleParam);
        memcpy((void *)&block->handle()->data,
               (void *)&nHandle->data,
               sizeof(int) * (nHandle->numFds + nHandle->numInts));

        native_handle_delete(nHandle);
    }

    return C2_OK;
}

class C2RKMpiDecFactory : public C2ComponentFactory {
public:
    explicit C2RKMpiDecFactory(std::string componentName)
            : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
                  GetCodec2PlatformComponentStore()->getParamReflector())),
              mComponentName(componentName) {
        if (!C2RKMediaUtils::getMimeFromComponentName(componentName, &mMime)) {
            c2_err("failed to get mime from component %s", componentName.c_str());
        }
        if (!C2RKMediaUtils::getDomainFromComponentName(componentName, &mDomain)) {
            c2_err("failed to get domain from component %s", componentName.c_str());
        }
        if (!C2RKMediaUtils::getKindFromComponentName(componentName, &mKind)) {
            c2_err("failed to get kind from component %s", componentName.c_str());
        }
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        if (sDecConcurrentInstances.load() >= kMaxDecConcurrentInstances) {
            c2_warn("Reject to Initialize() due to too many dec instances: %d",
                    sDecConcurrentInstances.load());
            return C2_NO_MEMORY;
        }

        *component = std::shared_ptr<C2Component>(
                new C2RKMpiDec(
                        mComponentName.c_str(),
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
