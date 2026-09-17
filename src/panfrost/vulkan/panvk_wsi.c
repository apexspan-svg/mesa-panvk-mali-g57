/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2025 Arm Ltd.
 *
 * Derived from tu_wsi.c:
 * Copyright © 2016 Red Hat
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include "panvk_wsi.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"

#include <stdlib.h>
#include <string.h>

#if defined(HAVE_PAN_KMOD_KBASE)
#include "lib/kmod/kbase_kmod.h"
#endif

#include "vk_util.h"
#include "wsi_common.h"

static VKAPI_PTR PFN_vkVoidFunction
panvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(panvk_physical_device, pdevice, physicalDevice);
   struct panvk_instance *instance = to_panvk_instance(pdevice->vk.instance);

   return vk_instance_get_proc_addr_unchecked(&instance->vk, pName);
}

static bool
panvk_can_present_on_device(VkPhysicalDevice pdevice, int fd)
{
   drmDevicePtr device;
   if (drmGetDevice2(fd, 0, &device) != 0)
      return false;
   /* Allow on-device presentation for all devices with bus type PLATFORM.
    * Other device types such as PCI or USB should use the PRIME blit path. */
   bool match = device->bustype == DRM_BUS_PLATFORM;

   drmFreeDevice(&device);

   return match;
}

VkResult
panvk_wsi_init(struct panvk_physical_device *physical_device)
{
   /*
    * PANVKDBG runtime capture.
    *
    * Diagnostic only: duplicate stderr to a persistent file when possible.
    * If open() fails, leave the original stderr untouched.
    */
   {
      static bool panvkdbg_filelog_initialized = false;

      if (!panvkdbg_filelog_initialized) {
         panvkdbg_filelog_initialized = true;

         const char *panvkdbg_path =
            getenv("PANVK_DEBUG_LOG_FILE");

         if (!panvkdbg_path || !panvkdbg_path[0])
            panvkdbg_path = "/data/user/0/com.antutu.ABenchMark/files/imagefs/home/xuser/panvk_runtime.log";

         FILE *panvkdbg_file = fopen(panvkdbg_path, "a");

         if (panvkdbg_file) {
            setvbuf(panvkdbg_file, NULL, _IOLBF, 0);

            fprintf(panvkdbg_file,
                    "\n===== PANVKDBG PROCESS START pid=%ld =====\n",
                    (long)getpid());
            fflush(panvkdbg_file);

            if (dup2(fileno(panvkdbg_file), STDERR_FILENO) >= 0) {
               setvbuf(stderr, NULL, _IOLBF, 0);
               fprintf(stderr,
                       "PANVKDBG FILE_LOG path=%s pid=%ld\n",
                       panvkdbg_path, (long)getpid());
               fflush(stderr);
            }

            fclose(panvkdbg_file);
         }
      }
   }
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);
   const bool uses_kbase = physical_device->kbase_node_path[0] != '\0';
   const char *dri3_option = getenv("PANVK_KBASE_DRI3");
   /* The proprietary Termux:X11 layer uses WSI_X11_TERMUX=1 as its
    * process-wide switch.  Accept the same switch here when no explicit
    * PanVK override is present, so both ICDs exercise the same raw-FD DRI3
    * transport instead of silently falling back to the slower SHM path. */
   const char *termux_wsi = getenv("WSI_X11_TERMUX");
   const bool termux_raw_dri3 =
      uses_kbase && !dri3_option && termux_wsi && strcmp(termux_wsi, "0") != 0;
   const bool kbase_raw_dri3 =
      termux_raw_dri3 ||
      (uses_kbase && dri3_option &&
       (!strcmp(dri3_option, "raw") || !strcmp(dri3_option, "termux")));
   const bool kbase_dmabuf =
#if defined(HAVE_PAN_KMOD_KBASE)
      uses_kbase &&
      ((!dri3_option && termux_raw_dri3) ||
       (dri3_option && strcmp(dri3_option, "0") != 0)) &&
      kbase_kmod_supports_dmabuf(physical_device->kmod.dev);
#else
      false;
#endif
   VkResult result;

   fprintf(stderr,
           "PANVKDBG WSI: uses_kbase=%d termux_raw=%d raw_dri3=%d "
           "kbase_dmabuf=%d sw_device=%d raw_fd_modifier=%d\n",
           uses_kbase,
           termux_raw_dri3,
           kbase_raw_dri3,
           kbase_dmabuf,
           uses_kbase && !kbase_dmabuf,
           kbase_dmabuf && kbase_raw_dri3);

   result = wsi_device_init(&physical_device->wsi_device,
                            panvk_physical_device_to_handle(physical_device),
                            panvk_wsi_proc_addr, &instance->vk.alloc, -1,
                            &instance->drirc.options,
                            &(struct wsi_device_options){
                               .sw_device = uses_kbase && !kbase_dmabuf,
                               .wait_present_before_queue = kbase_dmabuf,
                               .x11_use_raw_fd_modifier =
                                  kbase_dmabuf && kbase_raw_dri3,
                            });
   if (result != VK_SUCCESS)
      return result;

   /*
    * PANVKDBG diagnostic only:
    * USER_BUFFER host import now exists in the kbase backend.  Enable it
    * internally for CPU/X11 WSI without advertising the Vulkan extension yet.
    */
   if (uses_kbase) {
      physical_device->wsi_device.has_import_memory_host = true;
      fprintf(stderr,
              "PANVKDBG WSI host-import diagnostic enabled\n");
   }


   /* kbase syncs carry GPU seqno state in userspace and cannot be copied via
    * DRM syncobj fd payloads.  Keep even empty WSI submits on the real queue
    * so present fences are backed by the kbase submission timeline.
    */
   physical_device->wsi_device.disable_unordered_submits = uses_kbase;

   /* kbase is not a DRM fd.  The default CPU WSI path presents through
    * MIT-SHM.  Android dma-heaps can also provide shareable
    * allocations, but some Android X servers advertise DRI3 while rejecting
    * standard PixmapFromBuffer.  PANVK_KBASE_DRI3=raw (or termux) uses the
    * Termux:X11/Winlator private raw-FD modifier instead of DRM modifiers.
    */
   physical_device->wsi_device.supports_modifiers =
      !uses_kbase || (kbase_dmabuf && !kbase_raw_dri3);
   physical_device->wsi_device.can_present_on_device =
      panvk_can_present_on_device;

   physical_device->vk.wsi_device = &physical_device->wsi_device;

   return VK_SUCCESS;
}

void
panvk_wsi_finish(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);

   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device, &instance->vk.alloc);
}
