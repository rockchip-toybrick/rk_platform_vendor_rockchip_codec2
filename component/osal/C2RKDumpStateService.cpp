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

#include <string.h>
#include <sys/syscall.h>
#include <chrono>
#include <cutils/properties.h>
#include <unistd.h>
#include <errno.h>
#include <sstream>
#include <queue>
#include <fstream>
#include <iomanip>

#include "C2RKLogger.h"
#include "C2RKPropsDef.h"
#include "C2RKChipCapDef.h"
#include "C2RKMediaUtils.h"
#include "C2RKDumpStateService.h"

#include "rk_mpi.h"

namespace android {

C2_LOGGER_ENABLE("C2RKDumpStateService");

#define C2_RECORD_DIR   "/data/video/"

// TODO: do more restriction on soc capacity
#define MAX_DECODER_SOC_CAPACITY       (7680*4320*60)
#define MAX_ENCODER_SOC_CAPACITY       (7680*4320*30)

int32_t C2RKDumpStateService::mDumpFlags = 0;

std::string toStr_Node(std::shared_ptr<C2NodeInfo> node) {
    if (node) {
        std::ostringstream oss;
        oss << "[" << (node->mIsEncoder ? "enc" : "dec") << "_" << node->mPid << "]";
        return oss.str();
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
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    return milliseconds.count();
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
                Log.I("%s real-time bitrate %.1f kbps", mTag.c_str(), getInstantBitrate());
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
            : mTag(tag), mWindowSeconds(windowSeconds) {
        mTotalInputFrames = 0;
        mTotalOutputFrames = 0;
        mLogging = false;
        mLogInterval = 0;
        mLastInputLogTime = 0;
        mLastOutputLogTime = 0;
    }

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
            Log.I("%s input frameCount = %lld fps = %.3f", mTag.c_str(),
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
            Log.I("%s output frameCount = %lld fps = %.3f", mTag.c_str(),
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

std::string C2NodeInfo::getNodeSummary() {
    std::ostringstream oss;

    oss << "┌──────────────────────────────────────────────────┐\n"
        << "| Process     : " << mPid << "\n";

    if (mListener) {
        std::string summary;
        mListener->onNodeSummaryRequest(summary);
        oss << summary;
    }

    oss << std::fixed << std::setprecision(1) << std::setiosflags(std::ios::showpoint)
        << "| BitRate     : " << mBpsCalculator->getInstantBitrate() << " kbps\n"
        << "| Fps         : In " << mFpsCalculator->getInstantInputFPS()
        << " / Out " << mFpsCalculator->getInstantOutputFPS() << "\n";
    oss << "└──────────────────────────────────────────────────┘\n";

    return oss.str();
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
        Log.I("update dumpFlags 0x%x -> 0x%x", mDumpFlags, flags);
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

/*
 * Updates the debug features configuration
 *
 * This function parses a string containing debug feature configurations
 * and updates the internal debug features map. The input string can be
 * in two formats:
 * 1. Feature names separated by '|' delimiter (e.g., "feature1|feature2|feature3")
 * 2. Hexadecimal value representing the bit mask of enabled features (e.g., "0xb")
 *
 * @example
 * updateDebugFeatures("low-latency|disable-fbc|enable-parser-split")
 * updateDebugFeatures("0xb")
 */
void C2RKDumpStateService::updateFeatures(std::string features) {
    static const std::map<std::string, int32_t> kFeatureMap = {
        { "low-latency",            C2_FEATURE_DEC_ENABLE_LOW_LATENCY    },
        { "disable-fbc",            C2_FEATURE_DEC_DISABLE_FBC           },
        { "disable-deinterlace",    C2_FEATURE_DEC_DISABLE_DEINTERLACE   },
        { "enable-parser-split",    C2_FEATURE_DEC_ENABLE_PARSER_SPLIT   },
        { "disable-dpb-check",      C2_FEATURE_DEC_DISABLE_DPB_CHECK     },
        { "disable-error-mark",     C2_FEATURE_DEC_DISABLE_ERROR_MARK    },
        { "exclude-padding",        C2_FEATURE_DEC_EXCLUDE_PADDING       },
        { "low-memory-mode",        C2_FEATURE_DEC_LOW_MEMORY_MODE       },
        { "internal-buffer-group",  C2_FEATURE_DEC_INTERNAL_BUFFER_GROUP },
        { "async_output",           C2_FEATURE_ENC_ASYNC_OUTPUT          },
        { "disable-load-check",     C2_FEATURE_DISABLE_LOAD_CHECK        },
    };

    Mutex::Autolock autoLock(mNodeLock);
    // clear features
    mFeatureFlags = 0;

    size_t pos = 0;
    long val = std::stol(features, &pos, 0);
    if (pos == features.length()) {
        mFeatureFlags = static_cast<int32_t>(val);
    } else {
        std::istringstream iss(features);
        std::string feature;

        while (std::getline(iss, feature, '|')) {
            auto it = kFeatureMap.find(feature);
            if (it != kFeatureMap.end()) {
                mFeatureFlags |= it->second;
                Log.I("Add Feature: %s", it->first.c_str());
            } else {
                Log.I("Invalid feature name: %s", feature.c_str());
            }
        }
    }
    Log.I("Update final Feature flags 0x%x", mFeatureFlags);
}

bool C2RKDumpStateService::hasFeatures(int32_t feature) {
    Mutex::Autolock autoLock(mNodeLock);
    return (mFeatureFlags & feature);
}

std::shared_ptr<C2NodeInfo> C2RKDumpStateService::getNodeInfo(void *nodeId) {
    auto findInMap = [&](const auto& nodes) -> std::shared_ptr<C2NodeInfo> {
        auto it = nodes.find(nodeId);
        return (it != nodes.end()) ? it->second : nullptr;
    };

    auto node = findInMap(mDecNodes);
    if (node) {
        return node;
    }
    return findInMap(mEncNodes);
}

bool C2RKDumpStateService::addNode(std::shared_ptr<C2NodeInfo> node) {
    Mutex::Autolock autoLock(mNodeLock);

    if (node->mNodeId == nullptr) {
        Log.E("can't record node without nodeId");
        return false;
    }

    if (getNodeInfo(node->mNodeId) != nullptr) {
        Log.W("ignore duplicate node, nodeId %p", node->mNodeId);
        return true;
    }

    bool overload = true;
    bool disableCapCheck = false;
    int loading = 0;

    if (C2RKPropsDef::getLoadingCheckDisable() ||
            (mFeatureFlags & C2_FEATURE_DISABLE_LOAD_CHECK)) {
        disableCapCheck = true;
    }

    node->mPid = syscall(SYS_gettid);
    node->mBpsCalculator =
            std::make_shared<BitrateCalculator>(
                    toStr_Node(node), node->mFrameRate, 3 /* stateTime */);
    node->mFpsCalculator =
            std::make_shared<FrameRateCalculator>(toStr_Node(node), 1 /* windowSeconds */);
    node->mErrorFrameCnt = 0;

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
            std::ignore = mEncNodes.emplace(node->mNodeId, std::move(node));
            mEncTotalLoading += loading;
            overload = false;
        }
    } else {
        if (disableCapCheck || ((mDecTotalLoading + loading <= MAX_DECODER_SOC_CAPACITY)
                && (mDecNodes.size() < mMaxInstanceLimit))) {
            std::ignore = mDecNodes.emplace(node->mNodeId, std::move(node));
            mDecTotalLoading += loading;
            overload = false;
        }
    }

    if (overload) {
        Log.E("overload initialize %s(%dx%d@%.1f), current loading %d",
               node->mIsEncoder ? "mIsEncoder" : "decoder",
               node->mWidth, node->mHeight, node->mFrameRate,
               node->mIsEncoder ? mEncTotalLoading : mDecTotalLoading);
        return false;
    }

    return true;
}

void C2RKDumpStateService::removeNode(void *nodeId) {
    Mutex::Autolock autoLock(mNodeLock);

    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
        if (node->mInFile) {
            std::ignore = fclose(node->mInFile);
            node->mInFile = nullptr;
        }
        if (node->mOutFile) {
            std::ignore = fclose(node->mOutFile);
            node->mOutFile = nullptr;
        }
        if (node->mIsEncoder) {
            mEncTotalLoading -= (node->mWidth * node->mHeight * node->mFrameRate);
            std::ignore = mEncNodes.erase(node->mNodeId);
        } else {
            mDecTotalLoading -= (node->mWidth * node->mHeight * node->mFrameRate);
            std::ignore = mDecNodes.erase(node->mNodeId);
        }
    }
}

void C2RKDumpStateService::resetNode(void *nodeId) {
    Mutex::Autolock autoLock(mNodeLock);

    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node) {
        node->mErrorFrameCnt = 0;
        node->mFpsCalculator->reset();
        node->mBpsCalculator->reset();
    }
}

void C2RKDumpStateService::updateNode(
        void *nodeId, uint32_t width, uint32_t height, float frameRate) {
    Mutex::Autolock autoLock(mNodeLock);

    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
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
    }
}

bool C2RKDumpStateService::getNodePortFrameCount(
        void *nodeId, int64_t *inFrames, int64_t *outFrames, int64_t *errFrames) {
    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
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
            std::ostringstream oss;
            oss << C2_RECORD_DIR << (node->mIsEncoder ? "enc" : "dec")
                << "_in_" << node->mWidth << "x" << node->mHeight
                << "_" << node->mPid << ".bin";
            std::string fileName = oss.str();
            node->mInFile = fopen(fileName.c_str(), "wb");
            if (node->mInFile == nullptr) {
                Log.E("failed to open input file, err: %s", strerror(errno));
            } else {
                Log.I("recording input to %s", fileName.c_str());
            }
        }
    } else {
        if ((!hasDebugFlags(C2_DUMP_RECORD_ENCODE_INPUT) && node->mIsEncoder) ||
            (!hasDebugFlags(C2_DUMP_RECORD_DECODE_INPUT) && !node->mIsEncoder)) {
            std::ignore = fclose(node->mInFile);
            node->mInFile = nullptr;
        }
    }

    if (!node->mOutFile) {
        if ((hasDebugFlags(C2_DUMP_RECORD_ENCODE_OUTPUT) && node->mIsEncoder) ||
            (hasDebugFlags(C2_DUMP_RECORD_DECODE_OUTPUT) && !node->mIsEncoder)) {
            std::ostringstream oss;
            oss << C2_RECORD_DIR << (node->mIsEncoder ? "enc" : "dec")
                << "_out_" << node->mWidth << "x" << node->mHeight
                << "_" << node->mPid << ".bin";
            std::string fileName = oss.str();
            node->mOutFile = fopen(fileName.c_str(), "wb");
            if (node->mOutFile == nullptr) {
                Log.E("failed to open output file, err: %s", strerror(errno));
            } else {
                Log.I("recording output to %s", fileName.c_str());
            }
        }
    } else {
        if ((!hasDebugFlags(C2_DUMP_RECORD_ENCODE_OUTPUT) && node->mIsEncoder) ||
            (!hasDebugFlags(C2_DUMP_RECORD_DECODE_OUTPUT) && !node->mIsEncoder)) {
            std::ignore = fclose(node->mOutFile);
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
    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
        int32_t port = (node->mIsEncoder) ? kPortIndexOutput : kPortIndexInput;

        if (!skipStats) {
            // statistics track for each frame
            node->mBpsCalculator->addFrame(size);
            node->mFpsCalculator->recordFrame(port == kPortIndexInput /* input */);
        }

        // file saving for codec input and output
        FILE *file = (port == kPortIndexInput) ? node->mInFile : node->mOutFile;
        if (file != nullptr) {
            size_t written = fwrite(data, 1, size, file);
            if (written != size) {
                Log.PostError("fwrite", errno);
            }
            std::ignore = fflush(file);
            Log.I("%s dump_%s: data 0x%08x size %d",
                   toStr_Node(node).c_str(), toStr_DumpPort(port), data, size);
        }
    }
}

void C2RKDumpStateService::recordFrame(
        void *nodeId, void *src, int32_t w, int32_t h, int32_t fmt) {
    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
        int32_t port = (node->mIsEncoder) ? kPortIndexInput : kPortIndexOutput;

        // statistics track for each frame
        node->mFpsCalculator->recordFrame(port == kPortIndexInput /* input */);

        // file saving for codec input and output
        FILE *file = (port == kPortIndexInput) ? node->mInFile : node->mOutFile;
        if (file != nullptr && src != nullptr) {
            size_t written = 0;
            size_t totalSize = 0;

            if (MPP_FRAME_FMT_IS_FBC(fmt)) {
                Log.W("not support fbc buffer dump");
                return;
            }

            if (MPP_FRAME_FMT_IS_YUV_10BIT(fmt)) {
                // convert platform 10bit into 8bit yuv
                totalSize = w * h * 3 / 2;
                uint8_t *dst = (uint8_t *)malloc(totalSize);
                if (!dst) {
                    Log.W("failed to malloc temp 8bit dump buffer");
                    return;
                }

                C2RKMediaUtils::convert10BitNV12ToNV12(
                        { (uint8_t*)src, -1, -1, w, h, w, h },
                        { (uint8_t*)dst, -1, -1, w, h, w, h });

                written = fwrite(dst, 1, totalSize, file);
                if (written != totalSize) {
                    Log.PostError("fwrite", errno);
                }

                free(dst);
            } else {
                totalSize = MPP_FRAME_FMT_IS_RGB(fmt) ? (w * h * 4) : (w * h * 3 / 2);
                written = fwrite(src, 1, totalSize, file);
                if (written != totalSize) {
                    Log.PostError("fwrite", errno);
                }
            }

            std::ignore = fflush(file);
            Log.I("%s dump_%s_%s: data 0x%08x w:h [%d:%d]", toStr_Node(node).c_str(),
                   toStr_DumpPort(port), toStr_RawType(fmt), src, w, h);
        }
    }
}

void C2RKDumpStateService::recordFrame(void *nodeId, int32_t frameFlags) {
    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
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

    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
        Mutex::Autolock autoLock(node->mRecordLock);
        std::ignore = node->mRecordStartTimes.add(frameIndex, _getCurrentTimeMs());
    }
}

void C2RKDumpStateService::showFrameTiming(void *nodeId, int64_t frameIndex) {
    if (!hasDebugFlags(C2_DUMP_FRAME_TIMING))
        return;

    std::shared_ptr<C2NodeInfo> node = getNodeInfo(nodeId);
    if (node != nullptr) {
        Mutex::Autolock autoLock(node->mRecordLock);
        ssize_t index = node->mRecordStartTimes.indexOfKey(frameIndex);
        if (index != NAME_NOT_FOUND) {
            int64_t startTime = node->mRecordStartTimes.valueAt(index);
            int64_t timeDiff = (_getCurrentTimeMs() - startTime);
            std::ignore = node->mRecordStartTimes.removeItemsAt(index);
            Log.I("%s frameIndex %lld process consumes %lld ms",
                   toStr_Node(node).c_str(), frameIndex, timeDiff);
        }
    }
}

std::string C2RKDumpStateService::dumpNodesSummary() {
    Mutex::Autolock autoLock(mNodeLock);

    std::ostringstream oss;
    oss << "========================================\n";

    if (mFeatureFlags > 0) {
        oss << "Feature-Flags: 0x" << std::hex << mFeatureFlags << std::dec << "\n";
    }

    oss << "Hardware Codec2 Memory Summary\n";

    oss << "Total: " << mDecNodes.size() << " dec nodes / "
        << mEncNodes.size() << " enc nodes\n";

    if (mDecNodes.size() > 0) {
        oss << "\nDecoder:    \n";
        for (auto &pair : mDecNodes) {
            oss << pair.second->getNodeSummary();
        }
    }

    if (mEncNodes.size() > 0) {
        oss << "\nEncoder:    \n";
        for (auto &pair : mEncNodes) {
            oss << pair.second->getNodeSummary();
        }
    }
    oss << "========================================\n";

    return oss.str();
}

void C2RKDumpStateService::logNodesSummary() {
    std::stringstream ss(dumpNodesSummary());
    std::string line;
    while (std::getline(ss, line)) {
        Log.I("%s", line.c_str());
    }
}

} // namespace android
