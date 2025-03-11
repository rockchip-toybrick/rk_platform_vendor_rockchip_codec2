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

#ifndef ANDROID_C2_RK_DUMP_H__
#define ANDROID_C2_RK_DUMP_H__

#include <stdio.h>
#include <utils/Timers.h>
#include <utils/KeyedVector.h>

#include "rk_mpi.h"

using namespace android;

#define C2_DUMP_LOG_TRACE                   (0x00000001)
#define C2_DUMP_LOG_DETAIL                  (0x00000002)
#define C2_DUMP_FPS_SHOW_INPUT              (0x00000004)
#define C2_DUMP_FPS_SHOW_OUTPUT             (0x00000008)

#define C2_DUMP_RECORD_ENC_IN               (0x00000010)
#define C2_DUMP_RECORD_ENC_OUT              (0x00000020)
#define C2_DUMP_RECORD_DEC_IN               (0x00000040)
#define C2_DUMP_RECORD_DEC_OUT              (0x00000080)

/* decoding/encoding frame time consuming */
#define C2_DUMP_FRAME_TIMING                (0x00000100)

enum C2DumpRole {
    ROLE_INPUT = 0,
    ROLE_OUTPUT,
    ROLE_BUTT,
};

class C2RKDump {
public:
    C2RKDump();
    ~C2RKDump();

    void initDump(int32_t width, int32_t height, bool isEncoder);

    void recordFile(C2DumpRole role, void *data, size_t size);
    void recordFile(
            C2DumpRole role, void *src,
            int32_t w, int32_t h, MppFrameFormat fmt);

    void recordFrameTime(int64_t frameIndex);
    void showFrameTiming(int64_t frameIndex);

    void showDebugFps(C2DumpRole role);

    static bool hasDebugFlags(int32_t flag) { return (mFlag & flag); }

private:
    static int32_t mFlag;
    /* <frameIndex, frameStartTime> */
    KeyedVector<int64_t, int64_t> mRecordStartTimes;

    bool     mIsEncoder;

    FILE    *mInFile;
    FILE    *mOutFile;

    /* debug show fps */
    uint32_t mFrameCount[ROLE_BUTT];
    uint32_t mLastFrameCount[ROLE_BUTT];
    nsecs_t  mLastFpsTime[ROLE_BUTT];
};

#endif // ANDROID_C2_RK_DUMP_H__
