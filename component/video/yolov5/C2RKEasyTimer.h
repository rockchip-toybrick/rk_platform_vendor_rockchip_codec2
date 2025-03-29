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

#include <sys/time.h>

#include "C2RKLog.h"

#define DEFAULT_TIME_ALARM_MS       40

class C2RKEasyTimer {
public:
    C2RKEasyTimer() {}
    ~C2RKEasyTimer() {}

    void startRecord() {
        gettimeofday(&startTime, NULL);
    }

    void stopRecord(const char *task) {
        gettimeofday(&stopTime, NULL);
        uint64_t timeMs = (uint64_t)(stopTime.tv_sec  - startTime.tv_sec)  * 1000 +
                          (stopTime.tv_usec - startTime.tv_usec) / 1000;
        if (timeMs > DEFAULT_TIME_ALARM_MS) {
            c2_info("%s consumes %lld ms", task, timeMs);
        }
    }

private:
    struct timeval startTime;
    struct timeval stopTime;
};
