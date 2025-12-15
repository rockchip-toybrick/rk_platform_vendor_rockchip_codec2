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

#pragma once

#include <android-base/logging.h>

namespace android {

/// Macro to define a global constant logger instance with a specific tag.
#define C2_LOGGER_ENABLE(tag) \
        const constexpr C2RKLogger Log(tag); \

/*
 * @brief C2RKLogger provides a convenient tag-based logger interface.
 *
 * Supported log levels:
 *   T - trace, D - debug, I - info, W - warn, E - error
 */
class C2RKLogger {
public:
    explicit constexpr C2RKLogger(const char* tag) : mTag(tag) {}

    // Log at DEBUG level.
    void D(const char* fmt, ...) const;
    // Log at INFO level.
    void I(const char* fmt, ...) const;
    // Log at WARN level.
    void W(const char* fmt, ...) const;
    // Log at ERROR level.
    void E(const char* fmt, ...) const;

    // Log at ERROR level with detailed error information.
    void PostError(
            const char* msg, int32_t errCode,
            const char* func = __builtin_FUNCTION(),
            const int32_t line = __builtin_LINE()) const;
    // Log at ERROR level if condition is true.
    void PostErrorIf(
            bool condition, const char *msg,
            const char* func = __builtin_FUNCTION(),
            const int32_t line = __builtin_LINE()) const;

    // Log at function enter/leave, default to calling function name if not specified.
    void Enter(const char* func = __builtin_FUNCTION()) const;
    void Leave(const char* func = __builtin_FUNCTION()) const;
    void TraceEnter(const char* func = __builtin_FUNCTION()) const;
    void TraceLeave(const char* func = __builtin_FUNCTION()) const;

private:
    const char* mTag;
};

} // namespace android
