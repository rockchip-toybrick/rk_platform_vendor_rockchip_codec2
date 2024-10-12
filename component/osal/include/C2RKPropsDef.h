/*
 * Copyright 2024 Rockchip Electronics Co. LTD
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

#ifndef ANDROID_C2_RK_PROPS_DEF_H__
#define ANDROID_C2_RK_PROPS_DEF_H__

#include <stdio.h>

class C2RKPropsDef {
public:
    static int32_t getIsUseSpsOutputDelay();
    static int32_t getHdrDisable();
    static int32_t getScaleDisable();
    static int32_t getEncSuperMode();
};

#endif  // ANDROID_C2_RK_PROPS_DEF_H__
