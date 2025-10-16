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

//#define LOG_NDEBUG 0
#define LOG_TAG "android.hardware.media.c2@1.1-service"

#include <android-base/logging.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.1/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <minijail.h>

#include "C2RKPlatformSupport.h"

using namespace ::android::hardware::media::c2::V1_1;

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2@1.1-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2@1.1-seccomp_policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2@1.1-extended-seccomp-policy";


struct MyComponentStoreUtils : public utils::ComponentStore {
    using ComponentStore::ComponentStore;

    // Dumps information when lshal is called.
    android::hardware::Return<void> debug(
            const android::hardware::hidl_handle& handle,
            const android::hardware::hidl_vec<android::hardware::hidl_string>& args) override {
        const native_handle_t *h = handle.getNativeHandle();
        if (!h || h->numFds != 1) {
           LOG(ERROR) << "debug -- dumping failed -- "
                   "invalid file descriptor to dump to";
           return ::android::hardware::Return<void>();
        }

        android::Vector<C2String> c2Args;
        for (size_t i = 0; i < args.size(); i++) {
            c2Args.push_back(C2String(args[i].c_str()));
        }

        if (!android::UpdateComponentDump(h->data[0], c2Args)) {
            LOG(WARNING) << "debug -- dumping failed";
        }

        return ::android::hardware::Return<void>();
    }
};

int main(int /* argc */, char** /* argv */) {
    using namespace ::android;
    LOG(INFO) << "android.hardware.media.c2@1.1-service starting...";

    // Set up minijail to limit system calls.
    signal(SIGPIPE, SIG_IGN);
    SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    // Enable vndbinder to allow vendor-to-vendor binder calls.
    ProcessState::initWithDriver("/dev/vndbinder");
    ProcessState::self()->startThreadPool();
    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    // Create IComponentStore service.
    {
        sp<IComponentStore> store;

        LOG(INFO) << "Instantiating Codec2's IComponentStore service...";
        store = new MyComponentStoreUtils(
                android::GetCodec2RKComponentStore());

        if (store == nullptr) {
            LOG(ERROR) << "Cannot create Codec2's IComponentStore service.";
        } else {
            constexpr char const* serviceName = "default";
            if (store->registerAsService(serviceName) != OK) {
                LOG(ERROR) << "Cannot register Codec2's IComponentStore service"
                              " with instance name << \""
                           << serviceName << "\".";
            } else {
                LOG(INFO) << "Codec2's IComponentStore service registered. "
                              "Instance name: \"" << serviceName << "\".";
            }
        }
    }

    hardware::joinRpcThreadpool();
    return 0;
}
