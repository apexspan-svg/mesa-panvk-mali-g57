/*
 * Copyright © 2026 Collabora Ltd.
 *
 * SPDX-License-Identifier: MIT
 *
 * kbase JM asynchronous submission engine.
 *
 * The historical JM path submits each batch (or job bag) and CPU-waits for
 * completion before returning from vkQueueSubmit. That serializes the CPU
 * record path against GPU execution and leaves the GPU starved between
 * batches. This engine instead returns from submit immediately and tracks
 * the single outstanding job bag per device:
 *
 * - Submissions are still serialized per device (each submit waits for the
 *   previous bag), so kernel-side execution order is identical to the
 *   synchronous path. The win is CPU/GPU overlap: recording the next work
 *   proceeds while the GPU runs the current bag.
 * - Completion is observed lazily by polling the kbase event fd (no
 *   background thread): every wait path (fences, semaphores, resets,
 *   presents, idles, destroys) drains events first.
 * - Fences and semaphores reuse the existing kbase_cpu_sync machinery: the
 *   submit arms them with a pending-wait bound to the bag's sequence
 *   number (targets[0]) plus the device (targets[1]), and the wait callback
 *   blocks until that sequence retires.
 * - Lifetimes: command buffers/pools wait for their own last sequence
 *   number on reset/destroy (precise, usually already complete). Image,
 *   buffer, memory, descriptor/query pool teardown drains only when
 *   something is actually in flight.
 *
 * Locking: dev->async.lock serializes event consumption and bag state.
 * It is never held across blocking polls (wait loops drop it while
 * polling) and never nested under any other lock.
 *
 * Active only on kbase devices with PANVK_ASYNC=1; every entry point
 * no-ops otherwise.
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util/log.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"

#include "panvk_device.h"
#include "panvk_physical_device.h"
#include "kmod/pan_kmod.h"

/* JD event as reported by the kbase fd (matches the JM backend's
 * base_jd_event_v2 layout). */
struct panvk_kbase_jd_event {
   uint32_t event_code;
   uint8_t atom_number;
   uint8_t padding[3];
   uint64_t udata[2];
};

enum {
   PANVK_KBASE_JD_EVENT_DONE = 0x01,
};

/* Submit ioctl (same layout as the JM backend's kbase_ioctl_job_submit:
 * type 0x80, nr 2). */
struct panvk_kbase_jm_submit_ioctl {
   uint64_t addr;
   uint32_t nr_atoms;
   uint32_t stride;
};

#define PANVK_KBASE_IOCTL_JOB_SUBMIT \
   _IOW(0x80, 2, struct panvk_kbase_jm_submit_ioctl)

struct panvk_kbase_jm_bag {
   uint64_t seqno;
   void *atoms;
   void *extres_blob;
   unsigned pending;
   bool failed;
};

static bool
kbase_async_is_kbase(struct panvk_device *dev)
{
   return to_panvk_physical_device(dev->vk.physical)->kbase_node_path[0] !=
          '\0';
}

void
panvk_kbase_async_init(struct panvk_device *dev)
{
   simple_mtx_init(&dev->async.lock, mtx_plain);
   dev->async.init = true;
   dev->async.enabled =
      kbase_async_is_kbase(dev) && getenv("PANVK_ASYNC") != NULL;
   dev->async.lost = false;
   dev->async.seqno = 0;
   dev->async.completed_seqno = 0;
   dev->async.bag = NULL;
}

void
panvk_kbase_async_fini(struct panvk_device *dev)
{
   if (!dev->async.init)
      return;

   /* Drain anything still flying, then tear down. Device destroy with
    * pending work is an app bug; block rather than leak. */
   panvk_kbase_async_drain(dev);
   simple_mtx_destroy(&dev->async.lock);
   dev->async.init = false;
   dev->async.enabled = false;
}

bool
panvk_kbase_async_is_enabled(struct panvk_device *dev)
{
   return dev->async.init && dev->async.enabled && !dev->async.lost;
}

bool
panvk_kbase_async_busy(struct panvk_device *dev)
{
   bool busy = false;

   if (!dev->async.init || !dev->async.enabled)
      return false;

   simple_mtx_lock(&dev->async.lock);
   busy = dev->async.bag != NULL;
   simple_mtx_unlock(&dev->async.lock);

   return busy;
}

/* Consume all immediately-available JD events, retiring the outstanding bag
 * when its atoms complete. Called with the lock held; never blocks.
 * Retired bags are returned through the list for freeing outside the lock. */
static void
async_reap_locked(struct panvk_device *dev,
                  struct panvk_kbase_jm_bag **retired,
                  unsigned *nr_retired)
{
   while (dev->async.bag) {
      struct pollfd pfd = {
         .fd = dev->kmod.dev->fd,
         .events = POLLIN,
      };
      int ret = poll(&pfd, 1, 0);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret <= 0 || !(pfd.revents & POLLIN))
         break;

      struct panvk_kbase_jd_event ev;
      ssize_t len = read(dev->kmod.dev->fd, &ev, sizeof(ev));
      if (len < 0 && (errno == EINTR || errno == EAGAIN))
         continue;
      if (len != sizeof(ev)) {
         mesa_loge("kbase async: invalid JD event read length %zd", len);
         dev->async.lost = true;
         break;
      }

      /* Single outstanding bag: every event belongs to it. */
      struct panvk_kbase_jm_bag *bag = dev->async.bag;
      if (!bag || bag->pending == 0)
         continue;

      if (ev.event_code != PANVK_KBASE_JD_EVENT_DONE) {
         mesa_loge("kbase async: atom %u failed with JD event 0x%02x",
                   ev.atom_number, ev.event_code);
         bag->failed = true;
      }

      bag->pending--;
      if (bag->pending == 0) {
         if (bag->failed)
            dev->async.lost = true;
         if (bag->seqno > dev->async.completed_seqno)
            dev->async.completed_seqno = bag->seqno;
         dev->async.bag = NULL;
         retired[(*nr_retired)++] = bag;
      }
   }
}

static void
async_retire(struct panvk_kbase_jm_bag **retired, unsigned nr_retired)
{
   for (unsigned i = 0; i < nr_retired; i++) {
      free(retired[i]->atoms);
      free(retired[i]->extres_blob);
      free(retired[i]);
   }
}

/* Submit a bag without waiting. Takes ownership of the atoms/extres memory
 * (freed on retirement). Blocks only until any previous bag retires
 * (per-device serialization). Returns the bag sequence number, or 0 on
 * ioctl failure (device is then marked lost). */
uint64_t
panvk_kbase_async_submit(struct panvk_device *dev, void *atoms,
                         unsigned nr_atoms, unsigned stride, void *extres_blob)
{
   struct panvk_kbase_jm_bag *bag = calloc(1, sizeof(*bag));
   if (!bag) {
      free(atoms);
      free(extres_blob);
      return 0;
   }

   struct panvk_kbase_jm_bag *retired[1];
   unsigned nr_retired;

   simple_mtx_lock(&dev->async.lock);

   /* Serialize: wait for the previous bag before submitting. */
   while (dev->async.bag) {
      simple_mtx_unlock(&dev->async.lock);
      struct pollfd pfd = {
         .fd = dev->kmod.dev->fd,
         .events = POLLIN,
      };
      int ret = poll(&pfd, 1, 100);
      if (ret < 0 && errno != EINTR) {
         free(atoms);
         free(extres_blob);
         free(bag);
         return 0;
      }
      simple_mtx_lock(&dev->async.lock);
      nr_retired = 0;
      async_reap_locked(dev, retired, &nr_retired);
      simple_mtx_unlock(&dev->async.lock);
      async_retire(retired, nr_retired);
      simple_mtx_lock(&dev->async.lock);
   }

   if (dev->async.lost) {
      simple_mtx_unlock(&dev->async.lock);
      free(atoms);
      free(extres_blob);
      free(bag);
      return 0;
   }

   struct panvk_kbase_jm_submit_ioctl submit = {
      .addr = (uint64_t)(uintptr_t)atoms,
      .nr_atoms = nr_atoms,
      .stride = stride,
   };
   int ret = ioctl(dev->kmod.dev->fd, PANVK_KBASE_IOCTL_JOB_SUBMIT, &submit);
   if (ret) {
      mesa_loge("kbase async: KBASE_IOCTL_JOB_SUBMIT failed: %s",
                strerror(errno));
      dev->async.lost = true;
      simple_mtx_unlock(&dev->async.lock);
      free(atoms);
      free(extres_blob);
      free(bag);
      return 0;
   }

   bag->seqno = ++dev->async.seqno;
   bag->atoms = atoms;
   bag->extres_blob = extres_blob;
   bag->pending = nr_atoms;
   bag->failed = false;
   dev->async.bag = bag;
   uint64_t seqno = bag->seqno;
   simple_mtx_unlock(&dev->async.lock);

   return seqno;
}

VkResult
panvk_kbase_async_wait_seqno(struct panvk_device *dev, uint64_t seqno,
                             uint64_t abs_timeout_ns)
{
   if (!dev->async.init || !dev->async.enabled)
      return VK_SUCCESS;

   struct panvk_kbase_jm_bag *retired[1];
   unsigned nr_retired;

   simple_mtx_lock(&dev->async.lock);
   while (true) {
      if (dev->async.lost) {
         simple_mtx_unlock(&dev->async.lock);
         return VK_ERROR_DEVICE_LOST;
      }
      if (seqno == 0 || seqno <= dev->async.completed_seqno) {
         simple_mtx_unlock(&dev->async.lock);
         return VK_SUCCESS;
      }
      if (!dev->async.bag) {
         /* Nothing in flight but the sequence hasn't retired: it must
          * have completed through another path; treat as done. */
         simple_mtx_unlock(&dev->async.lock);
         return VK_SUCCESS;
      }

      int timeout_ms;
      if (abs_timeout_ns == UINT64_MAX) {
         timeout_ms = 100;
      } else {
         int64_t remaining = (int64_t)abs_timeout_ns - os_time_get_nano();
         if (remaining <= 0) {
            simple_mtx_unlock(&dev->async.lock);
            return VK_TIMEOUT;
         }
         timeout_ms = remaining > 100000000 ? 100 : (int)(remaining / 1000000);
      }

      simple_mtx_unlock(&dev->async.lock);
      struct pollfd pfd = {
         .fd = dev->kmod.dev->fd,
         .events = POLLIN,
      };
      int ret = poll(&pfd, 1, timeout_ms);
      simple_mtx_lock(&dev->async.lock);
      if (ret < 0 && errno == EINTR)
         continue;
      nr_retired = 0;
      async_reap_locked(dev, retired, &nr_retired);
      simple_mtx_unlock(&dev->async.lock);
      async_retire(retired, nr_retired);
      simple_mtx_lock(&dev->async.lock);
   }
}

VkResult
panvk_kbase_async_drain(struct panvk_device *dev)
{
   if (!dev->async.init || !dev->async.enabled)
      return VK_SUCCESS;

   while (panvk_kbase_async_busy(dev)) {
      uint64_t seqno = 0;
      simple_mtx_lock(&dev->async.lock);
      if (dev->async.bag)
         seqno = dev->async.bag->seqno;
      simple_mtx_unlock(&dev->async.lock);
      if (!seqno)
         break;
      VkResult result =
         panvk_kbase_async_wait_seqno(dev, seqno, UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

VkResult
panvk_kbase_async_wait_bag(void *data,
                           const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT],
                           uint64_t abs_timeout_ns)
{
   /* targets[0] carries the sequence number, targets[1] the device. */
   (void)data;
   struct panvk_device *dev =
      (struct panvk_device *)(uintptr_t)targets[1];
   if (!dev)
      return VK_SUCCESS;
   return panvk_kbase_async_wait_seqno(dev, targets[0], abs_timeout_ns);
}
