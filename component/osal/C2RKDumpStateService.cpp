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

const char *toStr_DumpRole(uint32_t role) {
    switch (role) {
        case ROLE_INPUT:       return "input";
        case ROLE_OUTPUT:      return "output";
        default:               return "unknown";
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

    if (node->mFrameRate <= 1.0f) {
        node->mFrameRate = 30.0f;
    }

    loading = node->mWidth * node->mHeight * node->mFrameRate;

    // Update dump flag whenever a new client connects
    mDumpFlags = property_get_int32("vendor.dump.c2.log", 0);

    // dynamically determine file capture based on dumpFlags
    updateDumpFileStatus(node);

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

void C2RKDumpStateService::updateDumpFileStatus(std::shared_ptr<C2NodeInfo> node) {
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
    }
}

void C2RKDumpStateService::recordFile(void *nodeId, void *data, size_t size) {
    if (!hasDebugFlags(C2_DUMP_RECORD_IO_MASK)) {
        return;
    }

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        C2DumpRole role = (node->mIsEncoder) ? ROLE_OUTPUT : ROLE_INPUT;
        FILE *file = (role == ROLE_INPUT) ? node->mInFile : node->mOutFile;
        if (file) {
            fwrite(data, 1, size, file);
            fflush(file);
            c2_info("%s dump_%s: data 0x%08x size %d",
                    toStr_Node(node).c_str(), toStr_DumpRole(role), data, size);
        }
    }
}

void C2RKDumpStateService::recordFile(
        void *nodeId, void *src, int32_t w, int32_t h, int32_t fmt) {
    if (!hasDebugFlags(C2_DUMP_RECORD_IO_MASK)) {
        return;
    }


    if (MPP_FRAME_FMT_IS_FBC(fmt)) {
        c2_warn("not support fbc buffer dump");
        return;
    }

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        size_t size = 0;
        C2DumpRole role = (node->mIsEncoder) ? ROLE_INPUT : ROLE_OUTPUT;
        FILE *file = (role == ROLE_INPUT) ? node->mInFile : node->mOutFile;
        if (file) {
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
            c2_info("%s dump_%s_%s: data 0x%08x w:h [%d:%d]", toStr_Node(node).c_str(),
                    toStr_DumpRole(role), toStr_RawType(fmt), src, w, h);
        }
    }
}

void C2RKDumpStateService::showDebugFps(void *nodeId, C2DumpRole role) {
    if ((!hasDebugFlags(C2_DUMP_FPS_SHOW_INPUT) && role == ROLE_INPUT) ||
        (!hasDebugFlags(C2_DUMP_FPS_SHOW_OUTPUT) && role == ROLE_OUTPUT)) {
        return;
    }

    std::shared_ptr<C2NodeInfo> node = findNodeItem(nodeId);
    if (node) {
        nsecs_t now = systemTime();
        nsecs_t diff = now - node->mLastFpsTime[role];

        node->mFrameCount[role]++;

        if (diff > ms2ns(500)) {
            float fps = ((node->mFrameCount[role] -
                    node->mLastFrameCount[role]) * float(s2ns(1))) / diff;
            node->mLastFpsTime[role] = now;
            node->mLastFrameCount[role] = node->mFrameCount[role];
            c2_info("%s %s frameCount %d fps = %2.3f", toStr_Node(node).c_str(),
                    toStr_DumpRole(role), node->mFrameCount[role], fps);
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
            std::shared_ptr<C2NodeInfo> node = pair.second;
            snprintf(buffer, SIZE - 1, "    Client: %p\n    Pid   : %d\n    Mime"
                    "  : %s\n    Name  : %s\n    Size  : %dx%d\n    FrameRate: %.1f\n\n",
                    node->mNodeId, node->mPid, node->mMime, node->mName,
                    node->mWidth, node->mHeight, node->mFrameRate);
            result.append(buffer);
        }
    }

    if (mEncNodes.size() > 0) {
        result.append("\nEncoder:    \n");
        for (auto &pair : mEncNodes) {
            std::shared_ptr<C2NodeInfo> node = pair.second;
            snprintf(buffer, SIZE - 1, "    Client: %p\n    Pid   : %d\n    Mime"
                    "  : %s\n    Name  : %s\n    Size  : %dx%d\n    FrameRate: %.1f\n\n",
                    node->mNodeId, node->mPid, node->mMime, node->mName,
                    node->mWidth, node->mHeight, node->mFrameRate);
            result.append(buffer);
        }
    }
    result.append("========================================\n");

    if (logging) {
        c2_info("%s", result.c_str());
    }

    return result;
}

} // namespace android
