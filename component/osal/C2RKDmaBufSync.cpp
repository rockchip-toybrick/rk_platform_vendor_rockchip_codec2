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

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include "C2RKDmaBufSync.h"

typedef unsigned long long __u64;
typedef  unsigned int __u32;

struct dma_heap_allocation_data {
	__u64 len;
	__u32 fd;
	__u32 fd_flags;
	__u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC		'H'
#define DMA_HEAP_IOCTL_ALLOC	_IOWR(DMA_HEAP_IOC_MAGIC, 0x0,\
				      struct dma_heap_allocation_data)

#define DMA_BUF_SYNC_READ       (1 << 0)
#define DMA_BUF_SYNC_WRITE      (2 << 0)
#define DMA_BUF_SYNC_RW         (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START      (0 << 2)
#define DMA_BUF_SYNC_END        (1 << 2)

struct dma_buf_sync {
	__u64 flags;
};

#define DMA_BUF_BASE		    'b'
#define DMA_BUF_IOCTL_SYNC	    _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)


bool dma_sync_device_to_cpu(int fd) {
    struct dma_buf_sync sync = {0};

    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    return (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) >= 0) ? true : false;
}

bool dma_sync_cpu_to_device(int fd) {
    struct dma_buf_sync sync = {0};

    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    return (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) >= 0) ? true : false;
}
