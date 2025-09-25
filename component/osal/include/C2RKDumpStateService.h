/*
 * Copyright (C) 2025 Rockchip Electronics Co. LTD
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

#ifndef C2_RK_DUMP_STATE_SERVICE_H
#define C2_RK_DUMP_STATE_SERVICE_H

#include <stdio.h>
#include <string>
#include <map>

#include <utils/Mutex.h>
#include <utils/KeyedVector.h>

namespace android {

/* Log dump flags */
#define C2_DUMP_LOG_TRACE                   (0x00000001)
#define C2_DUMP_LOG_DETAIL                  (0x00000002)
#define C2_DUMP_FPS_SHOW_INPUT              (0x00000004)
#define C2_DUMP_FPS_SHOW_OUTPUT             (0x00000008)

/* Record dump flags */
#define C2_DUMP_RECORD_ENCODE_INPUT         (0x00000010)
#define C2_DUMP_RECORD_ENCODE_OUTPUT        (0x00000020)
#define C2_DUMP_RECORD_DECODE_INPUT         (0x00000040)
#define C2_DUMP_RECORD_DECODE_OUTPUT        (0x00000080)
#define C2_DUMP_RECORD_IO_MASK              (0x000000f0)


/* Performance monitoring flags */
#define C2_DUMP_FRAME_TIMING                (0x00000100)


enum C2DumpRole {
    ROLE_INPUT = 0,
    ROLE_OUTPUT,
    ROLE_BUTT,
};

struct C2NodeInfo {
public:
    C2NodeInfo(void *nodeId,
            const char *name, const char *mime,
            uint32_t width, uint32_t height,
            bool isEncoder, float frameRate) :
        mNodeId(nodeId), mName(name),
        mMime(mime), mWidth(width), mHeight(height),
        mIsEncoder(isEncoder), mFrameRate(frameRate),
        mPid(0), mInFile(nullptr), mOutFile(nullptr) {}

    /* codec basic information */
    void*       mNodeId;
    const char *mName;
    const char *mMime;
    uint32_t    mWidth;
    uint32_t    mHeight;
    bool        mIsEncoder;
    float       mFrameRate;
    uint32_t    mPid;

    /* dump file for input/output */
    FILE       *mInFile;
    FILE       *mOutFile;

    /* fps debugging */
    uint32_t    mFrameCount[ROLE_BUTT];
    uint32_t    mLastFrameCount[ROLE_BUTT];
    nsecs_t     mLastFpsTime[ROLE_BUTT];

    /*  frame timing analysis */
    /* <frameIndex, frameStartTime> */
    Mutex       mRecordLock;
    KeyedVector<int64_t, int64_t> mRecordStartTimes;
};

class C2RKDumpStateService {
public:
    static C2RKDumpStateService* get() {
        static C2RKDumpStateService _gInstance;
        return &_gInstance;
    }

    static bool hasDebugFlags(int32_t flags);

    // Node management
    std::shared_ptr<C2NodeInfo> findNodeItem(void *nodeId);
    bool addNode(std::shared_ptr<C2NodeInfo> node);
    bool removeNode(void *nodeId);

    // Input/output recording
    void recordFile(void *nodeId, void *data, size_t size);
    void recordFile(void *nodeId, void *src, int32_t w, int32_t h, int32_t fmt);

    // FPS debugging
    void showDebugFps(void *nodeId, C2DumpRole role);

    // Frame timing analysis
    void recordFrameTime(void *nodeId, int64_t frameIndex);
    void showFrameTiming(void *nodeId, int64_t frameIndex);

    // Node summary
    std::string dumpNodesSummary(bool logging = true);

private:
    C2RKDumpStateService();
    virtual ~C2RKDumpStateService();

    // Dynamically determine file capture based on dumpFlags
    void updateDumpFileStatus(std::shared_ptr<C2NodeInfo> node);

private:
    static int32_t mDumpFlags;

    Mutex   mNodeLock;
    int64_t mDecTotalLoading;
    int64_t mEncTotalLoading;
    int32_t mMaxInstanceLimit;

    std::map<void*, std::shared_ptr<C2NodeInfo>> mDecNodes;
    std::map<void*, std::shared_ptr<C2NodeInfo>> mEncNodes;
};

} // namespace android

#endif // C2_RK_DUMP_STATE_SERVICE_H
