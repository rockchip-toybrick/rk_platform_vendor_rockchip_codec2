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

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKDumpStateService"

#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <cutils/properties.h>
#include <unistd.h>
#include <errno.h>
#include <sstream>
#include <queue>

#include "C2RKLog.h"
#include "C2RKPropsDef.h"
#include "C2RKChipCapDef.h"
#include "C2RKMediaUtils.h"
#include "C2RKDumpStateService.h"

#include "rk_mpi.h"

namespace android {

#define C2_RECORD_DIR   "/data/video/"

// TODO: do more restriction on soc capacity
#define MAX_DECODER_SOC_CAPACITY       (7680*4320*60)
#define MAX_ENCODER_SOC_CAPACITY       (7680*4320*30)

int32_t C2RKDumpStateService::mDumpFlags = 0;

std::string toStr_Node(std::shared_ptr<C2NodeInfo> node) {
    if (node) {
        const size_t SIZE = 20;
        char buffer[SIZE];

        snprintf(buffer, SIZE - 1, "[%s_%d]", node->mIsEncoder ? "enc" : "dec", node->mPid);
        return std::string(buffer);
    }
    return std::string("unknown");
}

const char *toStr_DumpPort(uint32_t port) {
    switch (port) {
        case kPortIndexInput:   return "input";
        case kPortIndexOutput:  return "output";
        default:                return "unknown";
    }
}

const char *toStr_RawType(uint32_t type) {
    switch (type) {
        case MPP_FMT_YUV420SP:         return "yuv";
        case MPP_FMT_YUV420SP_10BIT:   return "10bit_yuv";
        case MPP_FMT_RGBA8888:         return "rgba";
        default:                       return "unknown";
    }
}

int64_t _getCurrentTimeMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Sliding window-based bitrate calculation */
class BitrateCalculator {
private:
    mutable Mutex mMutex;
    std::string mTag;
    std::deque<size_t> mQueue;
    float mFrameRate;
    int32_t mSlidingSize;

    // log print
    bool mLogging;
    float mLogInterval;
    int64_t mLastLogTime;

public:
    explicit BitrateCalculator(std::string tag, float frameRate, int32_t stateTime)
            : mTag(tag) {
        setup(frameRate, stateTime);
    }

    void updateLogging(bool enable, float intervalSeconds) {
        mLogging = enable;
        mLogInterval = intervalSeconds * 1000;
        mLastLogTime = _getCurrentTimeMs();
    }

    void setup(float frameRate, int32_t stateTime) {
        this->reset();

        mFrameRate = frameRate;
        mSlidingSize = frameRate * stateTime;
    }

    void addFrame(size_t bytes) {
        if (bytes == 0) return;

        {
            Mutex::Autolock autoLock(mMutex);
            // remove expired frames
            if (mQueue.size() >= mSlidingSize) {
                mQueue.pop_front();
            }

            mQueue.push_back(bytes);
        }

        if (mLogging) {
            int64_t now = _getCurrentTimeMs();
            if ((now - mLastLogTime) > mLogInterval) {
                c2_info("%s real-time bitrate %.1f kbps", mTag.c_str(),
                        getInstantBitrate());
                mLastLogTime = now;
            }
        }
    }

    float getInstantBitrate() const {
        Mutex::Autolock autoLock(mMutex);
        if (mQueue.empty()) {
            return 0;
        }

        size_t totalBytes = 0;
        for (auto bytes : mQueue) {
            totalBytes += bytes;
        }
        return (totalBytes * 8.0 / 1000) / (mQueue.size() / mFrameRate); // kbps
    }

    void reset() {
        Mutex::Autolock autoLock(mMutex);
        mQueue.clear();
    }
};

class FrameRateCalculator {
private:
    mutable Mutex mMutex;

    std::string mTag;
    int64_t mTotalInputFrames;
    int64_t mTotalOutputFrames;
    std::queue<int64_t> mInputTimestamps;
    std::queue<int64_t> mOutputTimestamps;

    // statistics window time(seconds)
    float mWindowSeconds;

    // log print
    bool mLogging;
    float mLogInterval;
    int64_t mLastInputLogTime;
    int64_t mLastOutputLogTime;

    void removeExpiredTimestamps(std::queue<int64_t> &timestamps, int64_t now = 0) {
        if (now == 0)
            now = _getCurrentTimeMs();

        while (!timestamps.empty()) {
            if ((now - timestamps.front()) >  (mWindowSeconds * 1000)) {
                timestamps.pop();
            } else {
                break;
            }
        }
    }

public:
    FrameRateCalculator(std::string tag, float windowSeconds = 1.0)
            : mTag(tag), mWindowSeconds(windowSeconds) {}

    void updateLogging(bool enable, float intervalSeconds) {
        int64_t now = _getCurrentTimeMs();
        mLogging = enable;
        mLogInterval = intervalSeconds * 1000;
        mLastInputLogTime = now;
        mLastOutputLogTime = now;
    }

    void recordFrame(bool input) {
        if (input) {
            recordInputFrame();
        } else {
            recordOutputFrame();
        }
    }

    void recordInputFrame() {
        Mutex::Autolock autoLock(mMutex);
        int64_t now = _getCurrentTimeMs();
        mInputTimestamps.push(now);
        mTotalInputFrames++;
        removeExpiredTimestamps(mInputTimestamps, now);

        if (mLogging && (now - mLastInputLogTime) > mLogInterval) {
            c2_info("%s input frameCount = %lld fps = %.3f", mTag.c_str(),
                    mTotalInputFrames, mInputTimestamps.size()  / mWindowSeconds);
            mLastInputLogTime = now;
        }
    }

    void recordOutputFrame() {
        Mutex::Autolock autoLock(mMutex);
        int64_t now = _getCurrentTimeMs();
        mOutputTimestamps.push(now);
        mTotalOutputFrames++;
        removeExpiredTimestamps(mOutputTimestamps, now);

        if (mLogging && (now - mLastOutputLogTime) > mLogInterval) {
            c2_info("%s output frameCount = %lld fps = %.3f", mTag.c_str(),
                    mTotalOutputFrames, mOutputTimestamps.size()  / mWindowSeconds);
            mLastOutputLogTime = now;
        }
    }

    float getInstantInputFPS() {
        Mutex::Autolock autoLock(mMutex);
        removeExpiredTimestamps(mInputTimestamps);
        return mInputTimestamps.size()  / mWindowSeconds;
    }

    double getInstantOutputFPS() {
        Mutex::Autolock autoLock(mMutex);
        removeExpiredTimestamps(mOutputTimestamps);
        return mOutputTimestamps.size()  / mWindowSeconds;
    }

    int64_t getTotalInputFrames() const {
        Mutex::Autolock autoLock(mMutex);
        return mTotalInputFrames;
    }

    int64_t getTotalOutputFrames() const {
        Mutex::Autolock autoLock(mMutex);
        return mTotalOutputFrames;
    }

    void reset() {
        Mutex::Autolock autoLock(mMutex);
        while (!mInputTimestamps.empty())  mInputTimestamps.pop();
        while (!mOutputTimestamps.empty())  mOutputTimestamps.pop();
        mTotalInputFrames = 0;
        mTotalOutputFrames = 0;
    }
};

void C2NodeInfo::setListener(const std::shared_ptr<C2NodeInfoListener> &listener) {
    mListener = listener;
}

const char* C2NodeInfo::getNodeSummary() {
    mNodeSummary.clear();

    const size_t SIZE = 256;
    char buffer[SIZE];

    snprintf(buffer, sizeof(buffer),
        "┌──────────────────────────────────────────────────┐\n"
        "| Process     : %d\n", mPid
    );

    mNodeSummary.append(buffer);

    if (mListener) {
        mListener->onNodeSummaryRequest(mNodeSummary);
    }

    snprintf(buffer, sizeof(buffer),
        "| BitRate     : %.0f kbps\n"
        "| Fps         : In %.1f / Out %.1f\n",
        mBpsCalculator->getInstantBitrate(),
        mFpsCalculator->getInstantInputFPS(),
        mFpsCalculator->getInstantOutputFPS()
    );

    mNodeSummary.append(buffer);
    mNodeSummary.append("└──────────────────────────────────────────────────┘\n");

    return mNodeSummary.c_str();
}

C2RKDumpStateService::C2RKDumpStateService() {
    mDumpFlags = 0;
    mDecTotalLoading = 0;
    mEncTotalLoading = 0;

    if (C2RKChipCapDef::get()->getChipType() == RK_CHIP_3326) {
        mMaxInstanceLimit = 16;
    } else {
        mMaxInstanceLimit = 32;
    }
}

C2RKDumpStateService::~C2RKDumpStateService() {
}

void C2RKDumpStateService::updateDebugFlags(int32_t flags) {
    if (flags != mDumpFlags) {
        c2_info("update dumpFlags 0x%x -> 0x%x", mDumpFlags, flags);
        mDumpFlags = flags;

        // dynamically determine file capture based on dumpFlags
        for (auto &pair : mDecNodes) {
            onDumpFlagsUpdated(pair.second);
        }
        for (auto &pair : mEncNodes) {
            onDumpFlagsUpdated(pair.second);
        }
    }
}

bool C2RKDumpStateService::hasDebugFlags(int32_t flags) {
    return (mDumpFlags & flags);
}

std::shared_ptr<C2NodeInfo> C2RKDumpStateService::findNodeItem(void *nodeId) {
    auto it = mDecNodes.find(nodeId);
    if (it != mDecNodes.end()) {
        return it->second;
    }
    it = mEncNodes.find(nodeId);
    if (it != mEncNodes.end()) {
        return it->second;
    }

    return nullptr;
}

bool C2RKDumpStateService::addNode(std::shared_ptr<C2NodeInfo> node) {
    Mutex::Autolock autoLock(mNodeLock);

    if (node->mNodeId == nullptr) {
        c2_err("can't record node without nodeId");
        return false;
    }

    if (findNodeItem(node->mNodeId) != nullptr) {
        c2_info("ignore duplicate node, nodeId %p", node->mNodeId);
        return true;
    }

    bool overload = true;
    bool disableCapCheck = C2RKPropsDef::getLoadingCheckDisable();
    int loading = 0;

    node->mPid = syscall(SYS_gettid);
    node->mBpsCalculator =
            std::make_shared<BitrateCalculator>(
                    toStr_Node(node), node->mFrameRate, 3 /* stateTime */);
    node->mFpsCalculator =
            std::make_shared<FrameRateCalculator>(toStr_Node(node), 1 /* windowSeconds */);

    if (node->mFrameRate <= 1.0f) {
        node->mFrameRate = 30.0f;
    }

    loading = node->mWidth * node->mHeight * node->mFrameRate;

    // Update dump flag whenever a new client connects
    updateDebugFlags(property_get_int32("vendor.dump.c2.log", 0));

    // dynamically determine file capture based on dumpFlags
    onDumpFlagsUpdated(node);

    if (node->mIsEncoder) {
        if (disableCapCheck || ((mEncTotalLoading + loading <= MAX_ENCODER_SOC_CAPACITY)
                && (mEncNodes.size() < mMaxInstanceLimit))) {
            mEncNodes.insert(std::make_pair(node->mNodeId, std::move(node)));
            mEncTotalLoading += loading;
            overload = false;
        }
    } else {
        if (disableCapCheck || ((mDecTotalLoading + loading <= MAX_DECODER_SOC_CAPACITY)
                && (mDecNodes.size() < mMaxInstanceLimit))) {
            mDecNodes.insert(std::make_pair(node->mNodeId, std::move(node)));
            mDecTotalLoading += loading;
            overload = false;
        }
    }

    if (overload) {
        c2_err("overload initialize %s(%dx%d@%.1f), current loading %d",
                node->mIsEncoder ? "mIsEncoder" : "decoder",
                node->mWidth, node->mHeight, node->mFrameRate,
                node->mIsEncoder ? mEncTotalLoading : mDecTotalLoading);
        return false;
    }

    return true;
}

bool C2RKDumpStateService::removeNode(void *nodeId) {
    Mutex::Autolock autoLock(mNodeLock);

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        if (node->mInFile) {
            fclose(node->mInFile);
            node->mInFile = nullptr;
        }
        if (node->mOutFile) {
            fclose(node->mOutFile);
            node->mOutFile = nullptr;
        }
        if (node->mIsEncoder) {
            mEncTotalLoading -= (node->mWidth * node->mHeight * node->mFrameRate);
            mEncNodes.erase(node->mNodeId);
        } else {
            mDecTotalLoading -= (node->mWidth * node->mHeight * node->mFrameRate);
            mDecNodes.erase(node->mNodeId);
        }
        return true;
    } else {
        c2_warn("remove: unexpected nodeId %p", nodeId);
        return false;
    }
}

bool C2RKDumpStateService::resetNode(void *nodeId) {
    Mutex::Autolock autoLock(mNodeLock);

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        node->mErrorFrameCnt = 0;
        node->mFpsCalculator->reset();
        node->mBpsCalculator->reset();
        return true;
    }
    return false;
}

bool C2RKDumpStateService::updateNode(
        void *nodeId, uint32_t width, uint32_t height, float frameRate) {
    Mutex::Autolock autoLock(mNodeLock);

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        if (frameRate == .0f) {
            frameRate = node->mFrameRate;
        } else if (frameRate <= 1.0f) {
            frameRate = 30.0f;
        }

        if (node->mIsEncoder) {
            mEncTotalLoading -= (node->mWidth * node->mHeight * node->mFrameRate);
            mEncTotalLoading += (width * height * frameRate);
        } else {
            mDecTotalLoading -= (node->mWidth * node->mHeight * node->mFrameRate);
            mDecTotalLoading += (width * height * frameRate);
        }

        node->mBpsCalculator->setup(frameRate, 3 /* stateTime */);

        node->mWidth = width;
        node->mHeight = height;
        node->mFrameRate = frameRate;

        return true;
    }

    return false;
}

bool C2RKDumpStateService::getNodePortFrameCount(
        void *nodeId, int64_t *inFrames, int64_t *outFrames, int64_t *errFrames) {
    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        *inFrames = node->mFpsCalculator->getTotalInputFrames();
        *outFrames = node->mFpsCalculator->getTotalOutputFrames();
        if (errFrames) {
            *errFrames = node->mErrorFrameCnt;
        }
        return true;
    }

    return false;
}

void C2RKDumpStateService::onDumpFlagsUpdated(std::shared_ptr<C2NodeInfo> node) {
    if (node == nullptr) {
        return;
    }

    if (!node->mInFile) {
        if ((hasDebugFlags(C2_DUMP_RECORD_ENCODE_INPUT) && node->mIsEncoder) ||
            (hasDebugFlags(C2_DUMP_RECORD_DECODE_INPUT) && !node->mIsEncoder)) {
            char fileName[128];
            memset(fileName, 0, 128);

            sprintf(fileName, "%s%s_in_%dx%d_%d.bin", C2_RECORD_DIR,
                    node->mIsEncoder ? "enc" : "dec", node->mWidth, node->mHeight, node->mPid);
            node->mInFile = fopen(fileName, "wb");
            if (node->mInFile == nullptr) {
                c2_err("failed to open input file, err: %s", strerror(errno));
            } else {
                c2_info("recording input to %s", fileName);
            }
        }
    } else {
        if ((!hasDebugFlags(C2_DUMP_RECORD_ENCODE_INPUT) && node->mIsEncoder) ||
            (!hasDebugFlags(C2_DUMP_RECORD_DECODE_INPUT) && !node->mIsEncoder)) {
            fclose(node->mInFile);
            node->mInFile = nullptr;
        }
    }

    if (!node->mOutFile) {
        if ((hasDebugFlags(C2_DUMP_RECORD_ENCODE_OUTPUT) && node->mIsEncoder) ||
            (hasDebugFlags(C2_DUMP_RECORD_DECODE_OUTPUT) && !node->mIsEncoder)) {
            char fileName[128];
            memset(fileName, 0, 128);

            sprintf(fileName, "%s%s_out_%dx%d_%d.bin", C2_RECORD_DIR,
                    node->mIsEncoder ? "enc" : "dec", node->mWidth, node->mHeight, node->mPid);
            node->mOutFile = fopen(fileName, "wb");
            if (node->mOutFile == nullptr) {
                c2_err("failed to open output file, err: %s", strerror(errno));
            } else {
                c2_info("recording output to %s", fileName);
            }
        }
    } else {
        if ((!hasDebugFlags(C2_DUMP_RECORD_ENCODE_OUTPUT) && node->mIsEncoder) ||
            (!hasDebugFlags(C2_DUMP_RECORD_DECODE_OUTPUT) && !node->mIsEncoder)) {
            fclose(node->mOutFile);
            node->mOutFile = nullptr;
        }
    }

    node->mFpsCalculator->updateLogging(
            hasDebugFlags(C2_DUMP_FPS_DEBUGGING), 1 /*intervalSeconds*/);
    node->mBpsCalculator->updateLogging(
            hasDebugFlags(C2_DUMP_BPS_DEBUGGING), 1 /*intervalSeconds*/);
}

void C2RKDumpStateService::recordFrame(
        void *nodeId, void *data, size_t size, bool skipStats) {
    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        int32_t port = (node->mIsEncoder) ? kPortIndexOutput : kPortIndexInput;

        if (!skipStats) {
            // statistics track for each frame
            node->mBpsCalculator->addFrame(size);
            node->mFpsCalculator->recordFrame(port == kPortIndexInput /* input */);
        }

        // file saving for codec input and output
        FILE *file = (port == kPortIndexInput) ? node->mInFile : node->mOutFile;
        if (file) {
            fwrite(data, 1, size, file);
            fflush(file);
            c2_info("%s dump_%s: data 0x%08x size %d",
                    toStr_Node(node).c_str(), toStr_DumpPort(port), data, size);
        }
    }
}

void C2RKDumpStateService::recordFrame(
        void *nodeId, void *src, int32_t w, int32_t h, int32_t fmt) {
    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        int32_t port = (node->mIsEncoder) ? kPortIndexInput : kPortIndexOutput;

        // statistics track for each frame
        node->mFpsCalculator->recordFrame(port == kPortIndexInput /* input */);

        // file saving for codec input and output
        FILE *file = (port == kPortIndexInput) ? node->mInFile : node->mOutFile;
        if (file && src) {
            if (MPP_FRAME_FMT_IS_FBC(fmt)) {
                c2_warn("not support fbc buffer dump");
                return;
            }

            if (MPP_FRAME_FMT_IS_YUV_10BIT(fmt)) {
                // convert platform 10bit into 8bit yuv
                size_t size = w * h * 3 / 2;
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
                size_t size = MPP_FRAME_FMT_IS_RGB(fmt) ? (w * h * 4) : (w * h * 3 / 2);
                fwrite(src, 1, size, file);
            }

            fflush(file);
            c2_info("%s dump_%s_%s: data 0x%08x w:h [%d:%d]", toStr_Node(node).c_str(),
                    toStr_DumpPort(port), toStr_RawType(fmt), src, w, h);
        }
    }
}

void C2RKDumpStateService::recordFrame(void *nodeId, int32_t frameFlags) {
    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        if (frameFlags & kErrorFrame || frameFlags & kDropFrame) {
            node->mErrorFrameCnt += 1;
        }
        if (frameFlags & kEOSFrame) {
            node->mFpsCalculator->recordFrame(false /* input */);
        }
    }
}

void C2RKDumpStateService::recordFrameTime(void *nodeId, int64_t frameIndex) {
    if (!hasDebugFlags(C2_DUMP_FRAME_TIMING))
        return;

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        Mutex::Autolock autoLock(node->mRecordLock);
        node->mRecordStartTimes.add(frameIndex, _getCurrentTimeMs());
    }
}

void C2RKDumpStateService::showFrameTiming(void *nodeId, int64_t frameIndex) {
    if (!hasDebugFlags(C2_DUMP_FRAME_TIMING))
        return;

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        Mutex::Autolock autoLock(node->mRecordLock);
        ssize_t index = node->mRecordStartTimes.indexOfKey(frameIndex);
        if (index != NAME_NOT_FOUND) {
            int64_t startTime = node->mRecordStartTimes.valueAt(index);
            int64_t timeDiff = (_getCurrentTimeMs() - startTime);
            node->mRecordStartTimes.removeItemsAt(index);
            c2_info("%s frameIndex %lld process consumes %lld ms",
                    toStr_Node(node).c_str(), frameIndex, timeDiff);
        }
    }
}

std::string C2RKDumpStateService::dumpNodesSummary(bool logging) {
    Mutex::Autolock autoLock(mNodeLock);

    const size_t SIZE = 256;
    char buffer[SIZE];
    std::string result;

    result.append("========================================\n");
    result.append("Hardware Codec2 Memory Summary\n");

    snprintf(buffer, SIZE - 1, "Total: %zu dec nodes / %zu enc nodes\n",
            mDecNodes.size(), mEncNodes.size());
    result.append(buffer);

    if (mDecNodes.size() > 0) {
        result.append("\nDecoder:    \n");
        for (auto &pair : mDecNodes) {
            result.append(pair.second->getNodeSummary());
        }
    }

    if (mEncNodes.size() > 0) {
        result.append("\nEncoder:    \n");
        for (auto &pair : mEncNodes) {
            result.append(pair.second->getNodeSummary());
        }
    }
    result.append("========================================\n");

    if (logging) {
        std::stringstream ss(result);
        std::string line;

        while (std::getline(ss, line)) {
            c2_info("%s", line.c_str());
        }
    }

    return result;
}

} // namespace android
