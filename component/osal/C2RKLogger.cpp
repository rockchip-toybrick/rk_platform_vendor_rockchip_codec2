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
#include <android/log.h>

#include "C2RKDumpStateService.h"
#include "C2RKLogger.h"

namespace android {

const constexpr int ANDROID_LOG_TRACE = -1;

void logger(int logLevel, const char* tag, const char* fmt, va_list args) {
    if (logLevel == ANDROID_LOG_DEBUG) {
        if (!C2RKDumpStateService::hasDebugFlags(C2_DUMP_LOG_TRACE) &&
            !C2RKDumpStateService::hasDebugFlags(C2_DUMP_LOG_DETAIL)) {
            return;
        }
        logLevel = ANDROID_LOG_INFO;
    }
    std::ignore = __android_log_vprint(logLevel, tag, fmt, args);
}

void C2RKLogger::D(const char* fmt, ...) const {
    va_list args; va_start(args, fmt);
    logger(ANDROID_LOG_DEBUG, mTag, fmt, args);
    va_end(args);
}

void C2RKLogger::I(const char* fmt, ...) const {
    va_list args; va_start(args, fmt);
    logger(ANDROID_LOG_INFO, mTag, fmt, args);
    va_end(args);
}

void C2RKLogger::W(const char* fmt, ...) const {
    va_list args; va_start(args, fmt);
    logger(ANDROID_LOG_WARN, mTag, fmt, args);
    va_end(args);
}

void C2RKLogger::E(const char* fmt, ...) const {
    va_list args; va_start(args, fmt);
    logger(ANDROID_LOG_ERROR, mTag, fmt, args);
    va_end(args);
}

void C2RKLogger::Enter(const char* func) const {
    I("%s enter", func);
}

void C2RKLogger::Leave(const char* func) const {
    I("%s leave", func);
}

void C2RKLogger::TraceEnter(const char* func) const {
    D("%s enter", func);
}

void C2RKLogger::TraceLeave(const char* func) const {
    D("%s leave", func);
}

void C2RKLogger::PostError(
        const char* msg, int32_t errCode, const char* func, int32_t line) const {
    E("failed to %s with err %d (@%s:%d)", msg, errCode, func, line);
}

void C2RKLogger::PostErrorIf(
        bool condition, const char *msg, const char* func, int32_t line) const {
    if (condition) {
        E("failed to %s (@%s:%d)", msg, func, line);
    }
}

} // namespace android
