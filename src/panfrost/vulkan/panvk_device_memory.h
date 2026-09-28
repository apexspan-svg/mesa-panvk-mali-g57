/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_DEVICE_MEMORY_H
#define PANVK_DEVICE_MEMORY_H

#include <stdint.h>

#include "vk_device_memory.h"

struct panvk_priv_bo;

struct panvk_device_memory {
   struct vk_device_memory vk;
   struct pan_kmod_bo *bo;
   struct {
      uint64_t dev;
      void *host;
   } addr;

   /* Owned Android Hardware Buffer backing an AHB-imported BO
    * (PANVK_AHB_WSI path). Released in FreeMemory after bo_put. */
   void *ahb;

   struct {
      /* Don't use this pointer, it's only to have user memory dumped when
       * PANVK_DEBUG=dump. */
      void *host_mapping;
   } debug;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_device_memory, vk.base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)

VkResult panvk_create_dma_buf_buffer_mem(VkDevice device, uint32_t width,
                                         uint32_t height, VkFormat format,
                                         VkDeviceSize size,
                                         uint32_t row_pitch_bytes,
                                         uint32_t memory_type_index,
                                         VkDeviceMemory *out_mem);

#endif
