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
 * limitations under the License.00
 *
 */

#ifndef ANDROID_RK_C2_DMA_SYNC_H_
#define ANDROID_RK_C2_DMA_SYNC_H_

bool dma_sync_device_to_cpu(int fd);
bool dma_sync_cpu_to_device(int fd);

#endif // #ifndef ANDROID_RK_C2_DMA_SYNC_H_
