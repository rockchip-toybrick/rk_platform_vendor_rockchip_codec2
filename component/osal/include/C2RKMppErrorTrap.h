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

 #ifndef ANDROID_C2_RK_MPP_ERROR_TRAP_H_
 #define ANDROID_C2_RK_MPP_ERROR_TRAP_H_

#include <C2.h>

#include "rk_mpi.h"

namespace android {

struct MppErrorTrap {
    int32_t mppError = MPP_OK;

    // Overload the assignment operator to implement "Sticky Error".
    // Only update the internal state when ret is an error code.
    MppErrorTrap& operator=(MPP_RET ret) noexcept {
        if (ret != MPP_OK) {
            mppError = static_cast<int32_t>(ret);
        }
        return *this;
    }

    friend bool operator==(const MppErrorTrap& lhs, MPP_RET rhs) noexcept {
        return lhs.mppError == static_cast<int32_t>(rhs);
    }

    friend bool operator!=(const MppErrorTrap& lhs, MPP_RET rhs) noexcept {
        return lhs.mppError != static_cast<int32_t>(rhs);
    }

    explicit operator int32_t() const noexcept {
        return mppError;
    }

    explicit operator c2_status_t() const noexcept {
        switch (mppError) {
            case MPP_OK:            return C2_OK;
            case MPP_ERR_MALLOC:    return C2_NO_MEMORY;
            case MPP_ERR_TIMEOUT:   return C2_TIMED_OUT;
            case MPP_ERR_VALUE:     return C2_BAD_VALUE;
            default:                return C2_CORRUPTED;
        }
    }
};

} // namespace android

#endif // ANDROID_C2_RK_MPP_ERROR_TRAP_H_