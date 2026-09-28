/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "genxml/decode.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vulkan/util/vk_util.h"

#include "panvk_android.h"
#include "panvk_device.h"
#include "panvk_device_memory.h"
#include "panvk_image.h"

#if defined(HAVE_PAN_KMOD_KBASE)
#include "lib/kmod/kbase_kmod.h"
#endif
#include "panvk_entrypoints.h"

#include "pan_props.h"

#include "util/u_atomic.h"
#include "vk_debug_utils.h"
#include "vk_log.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#endif

static void
panvk_memory_emit_report(struct panvk_device *device,
                         const struct panvk_device_memory *mem,
                         const VkMemoryAllocateInfo *alloc_info,
                         VkResult result)
{
   struct panvk_physical_device *pdev =
      to_panvk_physical_device(device->vk.physical);

   if (likely(!device->vk.memory_reports))
      return;

   if (result != VK_SUCCESS) {
      const uint32_t heap_index =
         pdev->memory.types[alloc_info->memoryTypeIndex].heapIndex;
      vk_emit_device_memory_report(
         &device->vk, VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT,
         /* mem_obj_id */ 0, alloc_info->allocationSize,
         VK_OBJECT_TYPE_DEVICE_MEMORY,
         /* obj_handle */ 0, heap_index);
      return;
   }

   VkDeviceMemoryReportEventTypeEXT type;
   if (alloc_info) {
      type = mem->vk.import_handle_type
                ? VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT
                : VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT;
   } else {
      type = mem->vk.import_handle_type
                ? VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT
                : VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT;
   }

   const uint32_t heap_index =
      pdev->memory.types[mem->vk.memory_type_index].heapIndex;
   vk_emit_device_memory_report(&device->vk, type, mem->bo->handle,
                                mem->bo->size, VK_OBJECT_TYPE_DEVICE_MEMORY,
                                (uintptr_t)(mem), heap_index);
}

#ifdef __ANDROID__
/* AHB-backed WSI image memory (PANVK_AHB_WSI=1): allocate an Android
 * Hardware Buffer for the swapchain image extent/format, import its dma-buf
 * fd through the kbase UMM path, and hand back a BO that exports. Falls
 * back to NULL (normal allocation) on anything unexpected: unsupported
 * format, padded stride, size mismatch, missing symbols.
 *
 * Only RGBA8-family formats map 1:1 (sRGB variants share the memory layout).
 * BGR swapchains fall back to the SHM path.
 */
#define PANVK_AHB_FORMAT_R8G8B8A8_UNORM 1
#define PANVK_AHB_USAGE_GPU_SAMPLED_IMAGE (1UL << 8)
#define PANVK_AHB_USAGE_GPU_FRAMEBUFFER (1UL << 9)

struct panvk_ahb_desc {
   uint32_t width, height, layers, format;
   uint64_t usage;
   uint32_t stride;
};

typedef struct {
   int version, numFds, numInts;
   int data[0];
} panvk_ahb_handle_t;

struct panvk_ahb_fns {
   int (*allocate)(const void *, void **);
   void (*describe)(const void *, void *);
   const void *(*getNativeHandle)(const void *);
   void (*release)(const void *);
   bool ok;
};

static struct panvk_ahb_fns
panvk_ahb_resolve(void)
{
   static struct panvk_ahb_fns fns = { 0 };
   static void *libandroid = NULL;
   static bool done = false;
   if (done)
      return fns;
   done = true;
   /* libandroid is not linked; load it explicitly (present on Android). */
   libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
   if (!libandroid) {
      fprintf(stderr, "PANVKDBG AHB_WSI fail: dlopen libandroid: %s\n",
              dlerror());
   }
   if (libandroid) {
      fns.allocate = dlsym(libandroid, "AHardwareBuffer_allocate");
      fns.describe = dlsym(libandroid, "AHardwareBuffer_describe");
      fns.getNativeHandle = dlsym(libandroid, "AHardwareBuffer_getNativeHandle");
      fns.release = dlsym(libandroid, "AHardwareBuffer_release");
   }
   if (!fns.getNativeHandle) {
      /* getNativeHandle lives in libnativewindow on-device, not in
       * Termux's libandroid. */
      void *nw = dlopen("/system/lib64/libnativewindow.so",
                        RTLD_NOW | RTLD_LOCAL);
      if (nw)
         fns.getNativeHandle = dlsym(nw, "AHardwareBuffer_getNativeHandle");
   }
   if (!fns.allocate)
      fns.allocate = dlsym(RTLD_DEFAULT, "AHardwareBuffer_allocate");
   if (!fns.describe)
      fns.describe = dlsym(RTLD_DEFAULT, "AHardwareBuffer_describe");
   if (!fns.release)
      fns.release = dlsym(RTLD_DEFAULT, "AHardwareBuffer_release");
   if (!fns.getNativeHandle)
      fns.getNativeHandle = dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
   if (!fns.getNativeHandle) {
      void *nw = dlopen("/system/lib64/libnativewindow.so",
                        RTLD_NOW | RTLD_LOCAL);
      if (nw)
         fns.getNativeHandle = dlsym(nw, "AHardwareBuffer_getNativeHandle");
   }
   fns.ok = fns.allocate && fns.describe && fns.getNativeHandle && fns.release;
   return fns;
}

/* Try to back a dedicated WSI image with an imported AHB dma-buf.
 * Returns VK_SUCCESS with out params filled, or an error to fall back.
 */
static VkResult
panvk_ahb_import_image(struct panvk_device *device, VkImage image,
                       VkDeviceSize alloc_size, struct pan_kmod_bo **out_bo,
                       void **out_ahb)
{
   VK_FROM_HANDLE(panvk_image, pimage, image);
   uint32_t w = pimage->vk.extent.width;
   uint32_t h = pimage->vk.extent.height;
   fprintf(stderr,
           "PANVKDBG AHB_WSI entry fmt=%u %ux%ux%u mips=%u layers=%u\n",
           (unsigned)pimage->vk.format, w, h, pimage->vk.extent.depth,
           pimage->vk.mip_levels, pimage->vk.array_layers);
   if (pimage->vk.extent.depth != 1 || pimage->vk.mip_levels != 1 ||
       pimage->vk.array_layers != 1 || w == 0 || h == 0)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   uint32_t ahb_fmt = 0;
   /* R8G8B8A8 sRGB shares the memory layout (transfer is bytewise). BGR
    * swapchains reuse the RGBA buffer: scanout presents raw bytes, so
    * channel order is preserved end-to-end (no sampling reinterpretation
    * on the present path). */
   switch (pimage->vk.format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      ahb_fmt = PANVK_AHB_FORMAT_R8G8B8A8_UNORM;
      break;
   default:
      fprintf(stderr, "PANVKDBG AHB_WSI fail: format %u unmappable\n",
              (unsigned)pimage->vk.format);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   struct panvk_ahb_fns fns = panvk_ahb_resolve();
   if (!fns.ok) {
      fprintf(stderr, "PANVKDBG AHB_WSI fail: symbols unavailable\n");
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   struct {
      uint32_t width, height, layers, format;
      uint64_t usage;
      uint32_t stride, rfu0;
      uint64_t rfu1;
   } desc;
   memset(&desc, 0, sizeof(desc));
   desc.width = w;
   desc.height = h;
   desc.layers = 1;
   desc.format = ahb_fmt;
   desc.usage = PANVK_AHB_USAGE_GPU_FRAMEBUFFER | PANVK_AHB_USAGE_GPU_SAMPLED_IMAGE;

   void *ahb = NULL;
   if (fns.allocate(&desc, &ahb) || !ahb) {
      fprintf(stderr, "PANVKDBG AHB_WSI fail: AHB_allocate w=%u h=%u\n", w, h);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   struct {
      uint32_t width, height, layers, format;
      uint64_t usage;
      uint32_t stride, rfu0;
      uint64_t rfu1;
   } out;
   memset(&out, 0, sizeof(out));
   fns.describe(ahb, &out);
   if (out.stride != w) {
      fprintf(stderr, "PANVKDBG AHB_WSI fail: stride %u != width %u\n", out.stride, w);
      fns.release(ahb);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   const panvk_ahb_handle_t *nh = fns.getNativeHandle(ahb);
   int dmabuf = -1;
   if (nh) {
      for (int i = 0; i < nh->numFds; i++) {
         char path[64], target[128];
         snprintf(path, sizeof(path), "/proc/self/fd/%d", nh->data[i]);
         ssize_t len = readlink(path, target, sizeof(target) - 1);
         if (len > 0) {
            target[len] = 0;
            if (strstr(target, "dmabuf")) {
               dmabuf = nh->data[i];
               break;
            }
         }
      }
   }
   if (dmabuf < 0) {
      fprintf(stderr, "PANVKDBG AHB_WSI fail: no dmabuf fd\n");
      fns.release(ahb);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   struct pan_kmod_bo *bo =
      pan_kmod_bo_import(device->kmod.dev, dmabuf);
   if (!bo) {
      fprintf(stderr, "PANVKDBG AHB_WSI fail: kmod import fd=%d\n", dmabuf);
      fns.release(ahb);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   if (pan_kmod_bo_size(bo) < alloc_size) {
      fprintf(stderr, "PANVKDBG AHB_WSI fail: bo_size=%llu < alloc=%llu\n",
              (unsigned long long)pan_kmod_bo_size(bo),
              (unsigned long long)alloc_size);
      pan_kmod_bo_put(bo);
      fns.release(ahb);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   *out_bo = bo;
   *out_ahb = ahb;
   return VK_SUCCESS;
}
#else
static VkResult
panvk_ahb_import_image(struct panvk_device *dev, VkImage image,
                       VkDeviceSize alloc_size, struct pan_kmod_bo **out_bo,
                       void **out_ahb)
{
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
#endif

VKAPI_ATTR VkResult VKAPI_CALL
panvk_AllocateMemory(VkDevice _device,
                     const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkDeviceMemory *pMem)
{
   if (panvk_android_is_ahb_memory(pAllocateInfo)) {
      return panvk_android_allocate_ahb_memory(_device, pAllocateInfo,
                                               pAllocator, pMem);
   }

   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);
   struct panvk_device_memory *mem;
   bool can_be_exported = false;
   VkResult result;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   const VkImportMemoryHostPointerInfoEXT *host_ptr_info =
      vk_find_struct_const(pAllocateInfo->pNext,
                           IMPORT_MEMORY_HOST_POINTER_INFO_EXT);

   if (unlikely(getenv("PANVK_VERBOSE"))) {
      fprintf(stderr,
              "PANVKDBG ALLOC size=%llu type=%u host_import=%d host_ptr=%p\n",
              (unsigned long long)pAllocateInfo->allocationSize,
              pAllocateInfo->memoryTypeIndex,
              host_ptr_info != NULL,
              host_ptr_info ? host_ptr_info->pHostPointer : NULL);
   }

   const VkMemoryType *type =
      &physical_device->memory.types[pAllocateInfo->memoryTypeIndex];

   const VkExportMemoryAllocateInfo *export_info =
      vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO);

   if (export_info) {
      if (export_info->handleTypes &
          ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
         return panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      else if (export_info->handleTypes)
         can_be_exported = true;
   }

   mem = vk_device_memory_create(&device->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (mem == NULL)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);

   if (fd_info && !fd_info->handleType)
      fd_info = NULL;

   if (host_ptr_info && host_ptr_info->handleType) {
      if (host_ptr_info->handleType !=
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) {
         result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }

#if defined(HAVE_PAN_KMOD_KBASE)
      if (!physical_device->kbase_node_path[0]) {
         result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }

      if (unlikely(getenv("PANVK_VERBOSE"))) {
         fprintf(stderr,
                 "PANVKDBG HOST_IMPORT USER_BUFFER ptr=%p size=%llu\n",
                 host_ptr_info->pHostPointer,
                 (unsigned long long)pAllocateInfo->allocationSize);
      }

      mem->bo = kbase_kmod_import_user_buffer(
         device->kmod.dev,
         host_ptr_info->pHostPointer,
         pAllocateInfo->allocationSize);

      if (!mem->bo) {
         result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }
#else
      result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      goto err_destroy_mem;
#endif
   } else if (fd_info) {
      assert(
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      mem->bo = pan_kmod_bo_import(device->kmod.dev, fd_info->fd);
      if (!mem->bo) {
         result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }
   } else {
      uint32_t bo_flags = 0;

      /* We don't do cached on exported buffers to keep the pre-WB_MMAP
       * behavior.
       */
      if (!can_be_exported &&
          (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         bo_flags |= PAN_KMOD_BO_FLAG_WB_MMAP;

      bo_flags = panvk_device_adjust_bo_flags(device, bo_flags);
      mem->bo = pan_kmod_bo_alloc(device->kmod.dev,
                                  can_be_exported ? NULL : device->kmod.vm,
                                  pAllocateInfo->allocationSize, bo_flags);
      if (!mem->bo) {
         result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_destroy_mem;
      }
   }


   /* Always GPU-map at creation time. */
   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = PAN_KMOD_VM_MAP_AUTO_VA,
         .size = pan_kmod_bo_size(mem->bo),
      },
      .map = {
         .bo = mem->bo,
         .bo_offset = 0,
      },
   };

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      uint64_t alignment =
         pan_choose_gpu_va_alignment(device->kmod.vm, op.va.size);
      unsigned arch = pan_arch(device->kmod.dev->props.gpu_id);
      /* For sizes larger than or equal to 64k, align the VA on 64k to meet the
       * requirement for interleaved_64k images (added in v10). */
      if (arch >= 10 && op.va.size >= 64 * 1024)
         alignment = MAX2(alignment, 64 * 1024);

      if (unlikely(mem->vk.alloc_flags &
                   VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT)) {
         const VkMemoryOpaqueCaptureAddressAllocateInfo *capture_alloc_info =
            vk_find_struct_const(pAllocateInfo->pNext,
                                 MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO);
         if (capture_alloc_info == NULL ||
             capture_alloc_info->opaqueCaptureAddress == 0) {
            op.va.start = panvk_as_alloc(device, &device->as.fixed_heap,
                                         op.va.size, alignment);
         } else {
            op.va.start = panvk_as_alloc_fixed_address(
               device, &device->as.fixed_heap,
               capture_alloc_info->opaqueCaptureAddress, op.va.size);
         }
      } else {
         op.va.start =
            panvk_as_alloc(device, &device->as.heap, op.va.size, alignment);
      }

      if (!op.va.start) {
         result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_put_bo;
      }
   }

   int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   if (ret) {
      result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto err_return_va;
   }

   mem->addr.dev = op.va.start;

   panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                pan_kmod_bo_size(mem->bo),
                                VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);

   if (fd_info) {
      /* From the Vulkan spec:
       *
       *    "Importing memory from a file descriptor transfers ownership of
       *    the file descriptor from the application to the Vulkan
       *    implementation. The application must not perform any operations on
       *    the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd_info->fd);
   }

   if (device->debug.decode_ctx) {
      if (PANVK_DEBUG(DUMP) || PANVK_DEBUG(TRACE)) {
         void *cpu =
            pan_kmod_bo_mmap(mem->bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
         if (cpu != MAP_FAILED)
            mem->debug.host_mapping = cpu;
         else
            vk_logw(VK_LOG_OBJS(_device),
                    "failed to map VkMemory for dump or trace.\n");
      }

      pandecode_inject_mmap(device->debug.decode_ctx, mem->addr.dev,
                            mem->debug.host_mapping, pan_kmod_bo_size(mem->bo),
                            NULL);
   }

   panvk_memory_emit_report(device, mem, pAllocateInfo, VK_SUCCESS);

   p_atomic_add(&physical_device->memory.heap_used,
                (uint64_t)pan_kmod_bo_size(mem->bo));

   *pMem = panvk_device_memory_to_handle(mem);

   return VK_SUCCESS;

err_return_va:
   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      panvk_as_free(device, &device->as.heap, op.va.start, op.va.size);
   }

err_put_bo:
   pan_kmod_bo_put(mem->bo);

err_destroy_mem:
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
   panvk_memory_emit_report(device, /* mem */ NULL, pAllocateInfo, result);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_FreeMemory(VkDevice _device, VkDeviceMemory _mem,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   /* Async mode: the memory may still be in flight. */
   panvk_kbase_async_drain_if_busy(device);

   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);

   p_atomic_add(&physical_device->memory.heap_used,
                -((int64_t)pan_kmod_bo_size(mem->bo)));

   if (device->debug.decode_ctx) {
      pandecode_inject_free(device->debug.decode_ctx, mem->addr.dev,
                            pan_kmod_bo_size(mem->bo));

      if (mem->debug.host_mapping)
         pan_kmod_bo_munmap(mem->bo, mem->debug.host_mapping,
                            pan_kmod_bo_size(mem->bo));
   }

   if (mem->addr.host) {
      ASSERTED int ret = pan_kmod_bo_munmap(mem->bo, mem->addr.host,
                                            pan_kmod_bo_size(mem->bo));
      assert(!ret);
   }

   panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                pan_kmod_bo_size(mem->bo),
                                VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {
         .start = mem->addr.dev,
         .size = pan_kmod_bo_size(mem->bo),
      },
   };

   ASSERTED int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   assert(!ret);

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      const bool fixed = (mem->vk.alloc_flags &
                          VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT);
      struct util_vma_heap *heap =
         fixed ? &device->as.fixed_heap : &device->as.heap;
      panvk_as_free(device, heap, op.va.start, op.va.size);
   }

   panvk_memory_emit_report(device, mem, /* alloc_info */ NULL, VK_SUCCESS);

   pan_kmod_bo_put(mem->bo);
#ifdef __ANDROID__
   if (mem->ahb) {
      static void (*release_fn)(const void *) = NULL;
      static bool release_done = false;
      if (!release_done) {
         release_done = true;
         release_fn = dlsym(RTLD_DEFAULT, "AHardwareBuffer_release");
      }
      if (release_fn)
         release_fn(mem->ahb);
      mem->ahb = NULL;
   }
#endif
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
}

/* WSI dma-buf blit-buffer hook (PANVK_AHB_WSI=1): back the present
 * buffer with an imported AHB dma-buf. row_pitch_bytes is WSI's tight row
 * length; the AHB stride must match it exactly (guaranteed for 4Bpp when
 * optimalBufferCopyRowPitchAlignment is 256, verified below).
 */
VkResult
panvk_create_dma_buf_buffer_mem(VkDevice _device, uint32_t w, uint32_t h,
                                VkFormat format, VkDeviceSize size,
                                uint32_t row_pitch_bytes,
                                uint32_t memory_type_index,
                                VkDeviceMemory *out_mem)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);

   if (!physical_device->kbase_node_path[0] || !getenv("PANVK_AHB_WSI"))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* 4 bytes/pixel only (row math below assumes it). */
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      break;
   default:
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   if (w == 0 || h == 0)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   struct panvk_ahb_fns fns = panvk_ahb_resolve();
   if (!fns.ok)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   struct {
      uint32_t width, height, layers, format;
      uint64_t usage;
      uint32_t stride, rfu0;
      uint64_t rfu1;
   } desc;
   memset(&desc, 0, sizeof(desc));
   desc.width = w;
   desc.height = h;
   desc.layers = 1;
   desc.format = PANVK_AHB_FORMAT_R8G8B8A8_UNORM;
   desc.usage = PANVK_AHB_USAGE_GPU_FRAMEBUFFER | PANVK_AHB_USAGE_GPU_SAMPLED_IMAGE;

   void *ahb = NULL;
   if (fns.allocate(&desc, &ahb) || !ahb)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   struct {
      uint32_t width, height, layers, format;
      uint64_t usage;
      uint32_t stride, rfu0;
      uint64_t rfu1;
   } out;
   memset(&out, 0, sizeof(out));
   fns.describe(ahb, &out);
   if ((uint64_t)out.stride * 4 != (uint64_t)row_pitch_bytes) {
      fprintf(stderr,
              "PANVKDBG AHB_WSI buffer: stride %u px != row %u B, fallback\n",
              out.stride, row_pitch_bytes);
      fns.release(ahb);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   if ((uint64_t)out.stride * 4 * h < (uint64_t)size) {
      fns.release(ahb);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   const panvk_ahb_handle_t *nh = fns.getNativeHandle(ahb);
   int dmabuf = -1;
   if (nh) {
      for (int i = 0; i < nh->numFds; i++) {
         char path[64], target[128];
         snprintf(path, sizeof(path), "/proc/self/fd/%d", nh->data[i]);
         ssize_t len = readlink(path, target, sizeof(target) - 1);
         if (len > 0) {
            target[len] = 0;
            if (strstr(target, "dmabuf")) {
               dmabuf = nh->data[i];
               break;
            }
         }
      }
   }
   if (dmabuf < 0) {
      fns.release(ahb);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   VkImportMemoryFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dmabuf,
   };
   VkMemoryAllocateInfo sub_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &fd_info,
      .allocationSize = size,
      .memoryTypeIndex = memory_type_index,
   };
   VkDeviceMemory mem_handle = VK_NULL_HANDLE;
   VkResult result =
      panvk_AllocateMemory(_device, &sub_info, NULL, &mem_handle);
   if (result != VK_SUCCESS) {
      fns.release(ahb);
      return result;
   }

   VK_FROM_HANDLE(panvk_device_memory, mem, mem_handle);
   mem->ahb = ahb;
   fprintf(stderr, "PANVKDBG AHB_WSI buffer imported %ux%u stride=%u fd=%d\n",
           w, h, out.stride, dmabuf);
   *out_mem = mem_handle;
   return VK_SUCCESS;
}


VKAPI_ATTR VkResult VKAPI_CALL
panvk_MapMemory2KHR(VkDevice _device, const VkMemoryMapInfoKHR *pMemoryMapInfo,
                    void **ppData)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size = vk_device_memory_range(
      &mem->vk, pMemoryMapInfo->offset, pMemoryMapInfo->size);

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "requested size 0x%" PRIx64
                          " does not fit in %u bits",
                          size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->addr.host)
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "Memory object already mapped.");

   void *host_addr = NULL;
   int map_flags = MAP_SHARED;

   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) {
      const VkMemoryMapPlacedInfoEXT *placed_info = vk_find_struct_const(
         pMemoryMapInfo->pNext, MEMORY_MAP_PLACED_INFO_EXT);
      assert(placed_info != NULL);
      assert(offset == 0 && size == mem->bo->size);
      host_addr = placed_info->pPlacedAddress;
      map_flags |= MAP_FIXED;
   }

   void *addr =
      pan_kmod_bo_mmap(mem->bo, PROT_READ | PROT_WRITE, map_flags, host_addr);
   if (addr == MAP_FAILED)
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "Memory object couldn't be mapped.");

   assert(!host_addr || addr == host_addr);
   mem->addr.host = addr;
   *ppData = (char *)addr + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_UnmapMemory2KHR(VkDevice _device,
                      const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryUnmapInfo->memory);

   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
      void *reserved =
         os_mmap(mem->addr.host, pan_kmod_bo_size(mem->bo), PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      if (reserved == MAP_FAILED)
         return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                             "Failed to reserve VA on unmap.");
   } else {
      ASSERTED int ret = pan_kmod_bo_munmap(mem->bo, mem->addr.host,
                                            pan_kmod_bo_size(mem->bo));
      assert(!ret);
   }

   mem->addr.host = NULL;

   return VK_SUCCESS;
}

static VkResult
sync_memory_ranges(struct panvk_device *device, uint32_t memoryRangeCount,
                   const VkMappedMemoryRange *pMemoryRanges,
                   enum pan_kmod_bo_sync_type type)
{
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      const VkMappedMemoryRange *r = &pMemoryRanges[i];
      if (r->size == 0)
         continue;

      VK_FROM_HANDLE(panvk_device_memory, memory, r->memory);

      pan_kmod_queue_bo_map_sync(
         memory->bo, r->offset, (char *)memory->addr.host + r->offset,
         vk_device_memory_range(&memory->vk, r->offset, r->size), type);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_FlushMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                              const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(panvk_device, device, _device);

   return sync_memory_ranges(device, memoryRangeCount, pMemoryRanges,
                             PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VkResult result =
      sync_memory_ranges(device, memoryRangeCount, pMemoryRanges,
                         PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE);

   if (result == VK_SUCCESS)
      pan_kmod_flush_bo_map_syncs(device->kmod.dev);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

   /* At the moment, we support only the below handle types. */
   assert(
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   int prime_fd = pan_kmod_bo_export(memory->bo);
   if (prime_fd < 0)
      return panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = prime_fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdPropertiesKHR(VkDevice _device,
                               VkExternalMemoryHandleTypeFlagBits handleType,
                               int fd,
                               VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(device->vk.physical);

   assert(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   struct pan_kmod_bo *bo = pan_kmod_bo_import(device->kmod.dev, fd);
   if (!bo)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   pMemoryFdProperties->memoryTypeBits = 0;

   /* Keep things simple by only allowing host-visible if the BO doesn't require
    * kernel-side synchronization going through the dma-buf exporter, which is
    * reflected through the PAN_KMOD_BO_FLAG_FORCE_FULL_KERNEL_SYNC flag.
    */
   const bool can_do_host_visible = !(bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP);
   const bool can_do_host_coherent = !(bo->flags & PAN_KMOD_BO_FLAG_WB_MMAP) ||
                                     (bo->flags & PAN_KMOD_BO_FLAG_IO_COHERENT);
   const bool can_do_host_cached = (bo->flags & PAN_KMOD_BO_FLAG_WB_MMAP);

   pMemoryFdProperties->memoryTypeBits = 0;
   for (uint32_t i = 0; i < phys_dev->memory.type_count; i++) {
      if (!can_do_host_visible && (phys_dev->memory.types[i].propertyFlags &
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         continue;
      if (!can_do_host_coherent && (phys_dev->memory.types[i].propertyFlags &
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
         continue;
      if (!can_do_host_cached && (phys_dev->memory.types[i].propertyFlags &
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         continue;

      pMemoryFdProperties->memoryTypeBits |= BITFIELD_BIT(i);
   }

   pan_kmod_bo_put(bo);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory,
                                VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VKAPI_ATTR uint64_t VKAPI_CALL
panvk_GetDeviceMemoryOpaqueCaptureAddress(
   VkDevice _device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
   VK_FROM_HANDLE(panvk_device_memory, memory, pInfo->memory);

   return memory->addr.dev;
}
