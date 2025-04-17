/*
 * Copyright (C) 2023 Rockchip Electronics Co. LTD
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
#define ROCKCHIP_LOG_TAG    "C2RKDump"

#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <cutils/properties.h>

#include "C2RKDump.h"
#include "C2RKLog.h"
#include "C2RKMediaUtils.h"

namespace android {

#define  C2_RECORD_DIR   "/data/video/"

int32_t C2RKDump::mFlag = 0;

const char *toStr_DumpRole(uint32_t role) {
    switch (role) {
        case ROLE_INPUT:       return "input";
        case ROLE_OUTPUT:      return "output";
    }

    c2_warn("unsupport dump role %d", role);
    return "unknown";
}

const char *toStr_RawType(uint32_t type) {
    switch (type) {
        case MPP_FMT_YUV420SP:         return "yuv";
        case MPP_FMT_YUV420SP_10BIT:   return "10bit_yuv";
        case MPP_FMT_RGBA8888:         return "rgba";
    }

    c2_warn("unsupport raw type %d", type);
    return "unknown";
}

int64_t getCurrentTimeMillis() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

C2RKDump::C2RKDump()
    : mIsEncoder(false),
      mInFile(nullptr),
      mOutFile(nullptr) {
    for (int i = 0; i < ROLE_BUTT; i++) {
        mFrameCount[i] = 0;
        mLastFrameCount[i] = 0;
        mLastFpsTime[i] = 0;
    }

    mFlag = property_get_int32("vendor.dump.c2.log", 0);
    if (mFlag != 0) {
        c2_info("get dump flag: 0x%08x", mFlag);
    }
}

C2RKDump::~C2RKDump() {
    if (mInFile != nullptr) {
        fclose(mInFile);
        mInFile = nullptr;
    }

    if (mOutFile != nullptr) {
        fclose(mOutFile);
        mOutFile = nullptr;
    }
}

void C2RKDump::initDump(int32_t width, int32_t height, bool isEncoder) {
    char fileName[128];

    if ((hasDebugFlags(C2_DUMP_RECORD_ENC_IN) && isEncoder) ||
        (hasDebugFlags(C2_DUMP_RECORD_DEC_IN) && !isEncoder)) {
        memset(fileName, 0, 128);

        sprintf(fileName, "%s%s_in_%dx%d_%ld.bin", C2_RECORD_DIR,
                isEncoder ? "enc" : "dec", width, height, syscall(SYS_gettid));
        mInFile = fopen(fileName, "wb");
        if (mInFile == nullptr) {
            c2_err("failed to open input file, err %s", strerror(errno));
        } else {
            c2_info("recording input to %s", fileName);
        }
    }

    if ((hasDebugFlags(C2_DUMP_RECORD_ENC_OUT) && isEncoder) ||
        (hasDebugFlags(C2_DUMP_RECORD_DEC_OUT) && !isEncoder)) {
        memset(fileName, 0, 128);

        sprintf(fileName, "%s%s_out_%dx%d_%ld.bin", C2_RECORD_DIR,
                isEncoder ? "enc" : "dec", width, height, syscall(SYS_gettid));
        mOutFile = fopen(fileName, "wb");
        if (mOutFile == nullptr) {
            c2_err("failed to open output file, err %s", strerror(errno));
        } else {
            c2_info("recording output to %s", fileName);
        }
    }

    mIsEncoder = isEncoder;
}

void C2RKDump::recordFile(C2DumpRole role, void *data, size_t size) {
    FILE *file = (role == ROLE_INPUT) ? mInFile : mOutFile;
    if (file) {
        fwrite(data, 1, size, file);
        fflush(file);

        c2_info("dump_%s: data 0x%08x size %d", toStr_DumpRole(role), data, size);
    }
}

void C2RKDump::recordFile(
        C2DumpRole role, void *src,
        int32_t w, int32_t h, MppFrameFormat fmt) {
    FILE *file = (role == ROLE_INPUT) ? mInFile : mOutFile;
    if (file) {
        if (MPP_FRAME_FMT_IS_FBC(fmt)) {
            c2_warn("not support fbc buffer dump");
            return;
        }

        size_t size = 0;

        if (MPP_FRAME_FMT_IS_YUV_10BIT(fmt)) {
            // convert platform 10bit into 8bit yuv
            size = w * h * 3 / 2;
            uint8_t *dst = (uint8_t *)malloc(size);
            if (!dst) {
                c2_warn("failed to malloc temp 8bit dump buffer");
                return;
            }

            C2RKMediaUtils::convert10BitNV12ToNV12(
                    { (uint8_t*)src, -1, -1, w, h, w, h },
                    { (uint8_t*)dst, -1, -1, w, h, w, h });
            fwrite(dst, 1, size, file);

            free(dst);
            dst = nullptr;
        } else {
            size = MPP_FRAME_FMT_IS_RGB(fmt) ? (w * h * 4) : (w * h * 3 / 2);
            fwrite(src, 1, size, file);
        }

        fflush(file);

        c2_info("dump_%s_%s: data 0x%08x w:h [%d:%d]",
                toStr_DumpRole(role), toStr_RawType(fmt), src, w, h);
    }
}

void C2RKDump::recordFrameTime(int64_t frameIndex) {
    if (hasDebugFlags(C2_DUMP_FRAME_TIMING)) {
        mRecordStartTimes.add(frameIndex, getCurrentTimeMillis());
    }
}

void C2RKDump::showFrameTiming(int64_t frameIndex) {
    if (hasDebugFlags(C2_DUMP_FRAME_TIMING)) {
        ssize_t index = mRecordStartTimes.indexOfKey(frameIndex);
        if (index != NAME_NOT_FOUND) {
            int64_t startTime = mRecordStartTimes.valueAt(index);
            int64_t timeDiff = (getCurrentTimeMillis() - startTime);
            mRecordStartTimes.removeItemsAt(index);
            c2_info("frameIndex %lld process consumes %lld ms", frameIndex, timeDiff);
        }
    }
}

void C2RKDump::showDebugFps(C2DumpRole role) {
    if ((role == ROLE_INPUT && !hasDebugFlags(C2_DUMP_FPS_SHOW_INPUT)) ||
        (role == ROLE_OUTPUT && !hasDebugFlags(C2_DUMP_FPS_SHOW_OUTPUT))) {
        return;
    }

    nsecs_t now = systemTime();
    nsecs_t diff = now - mLastFpsTime[role];

    mFrameCount[role]++;

    if (diff > ms2ns(500)) {
        float fps = ((mFrameCount[role] - mLastFrameCount[role]) * float(s2ns(1))) / diff;
        mLastFpsTime[role] = now;
        mLastFrameCount[role] = mFrameCount[role];
        c2_info("[%s] %s frameCount %d fps = %2.3f", mIsEncoder ? "enc" : "dec",
                toStr_DumpRole(role), mFrameCount[role], fps);
    }
}

}
