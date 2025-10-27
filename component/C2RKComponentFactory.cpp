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
#define ROCKCHIP_LOG_TAG    "C2RKComponentFactory"

#include <C2ComponentFactory.h>

#include "C2RKPlatformSupport.h"
#include "C2RKDumpStateService.h"
#include "C2RKMpiDec.h"
#include "C2RKMpiEnc.h"
#include "C2RKLog.h"

using namespace android;

extern "C" bool UpdateComponentDump(int fd, void* argsPtr, size_t argsSize) {
    auto dumpService = C2RKDumpStateService::get();
    std::string* args = static_cast<std::string*>(argsPtr);

    for (size_t i = 0; i < argsSize; i++) {
        const auto& arg = args[i];

        if (arg == "-flags" || arg == "--flags") {
            if (++i < argsSize)  {
                char* endptr = nullptr;
                long val = strtol(args[i].c_str(), &endptr, 0);
                if (*endptr != '\0') {
                    const char* msg = "Error: Invalid number format for flag.\n";
                    write(fd, msg, strlen(msg));
                    return false;
                }
                dumpService->updateDebugFlags(static_cast<int32_t>(val));
            }
        } else if (arg == "-features" || arg == "--features") {
            if (++i < argsSize)  {
                dumpService->updateFeatures(args[i]);
            }
        }
     }

    // dump all nodes summary
    std::string summary = dumpService->dumpNodesSummary(false);
    write(fd, summary.c_str(), summary.size());
    return true;
}

extern "C" ::C2ComponentFactory* CreateRKCodec2Factory(std::string componentName) {
    C2RKComponentEntry *entry = NULL;
    C2ComponentFactory *factory = NULL;

    entry = GetRKComponentEntry(componentName);
    if (!entry) {
        c2_err("failed to get component entry from name %s", componentName.c_str());
        goto __FAILED;
    }

    switch (entry->kind) {
      case C2Component::KIND_DECODER:
        factory = ::android::CreateRKMpiDecFactory(componentName);
      break;
      case C2Component::KIND_ENCODER:
        factory = ::android::CreateRKMpiEncFactory(componentName);
      break;
      default:
        c2_err("the kind %d is unsupport for create codec2 factory", entry->kind);
        goto __FAILED;
      break;
    }

    return factory;
__FAILED:
    return NULL;
}

extern "C" void DestroyRKCodec2Factory(::C2ComponentFactory* factory) {
    delete factory;
}

