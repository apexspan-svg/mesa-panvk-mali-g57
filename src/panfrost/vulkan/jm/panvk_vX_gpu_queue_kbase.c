/*
 * PanVK JM backend submit via raw kbase ioctls.
 *
 * Quando PANVK_USE_KBASE=1, esta implementacao substitui a submissao
 * DRM Panfrost por KBASE_IOCTL_JOB_SUBMIT direto em /dev/mali0.
 */

#include "panvk_device.h"
#include "panvk_physical_device.h"
#include "panvk_priv_bo.h"
#include "panvk_queue.h"
#include "panvk_cmd_buffer.h"
#include "decode.h"

#include "lib/kmod/kbase_kmod.h"
#include "lib/kmod/pan_kmod.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "util/os_time.h"

/* kbase ioctl definitions */
#define KBASE_IOCTL_TYPE 0x80
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };
#define KBASE_IOCTL_JOB_SUBMIT _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)

struct base_jd_event_v2 {
   uint32_t event_code;
   uint8_t atom_number;
   uint8_t padding[3];
   uint64_t udata[2];
};

enum {
   BASE_JD_EVENT_DONE = 0x01,
};

/* Implemented here; the only caller (the kbase path of gpu_queue_submit in
 * panvk_vX_gpu_queue.c) prototypes it as panvk_per_arch(kbase_jm_submit). */
VkResult
panvk_per_arch(kbase_jm_submit)(struct vk_queue *vk_queue,
                                struct panvk_gpu_queue *queue,
                                struct panvk_device *dev,
                                struct vk_queue_submit *submit);

/* kbase CPU syncs are resolved by the wait_many hook when someone waits on
 * them.  The JM backend submits jobs synchronously, so by the time the
 * signal is armed the GPU work is already done. */
static VkResult
panvk_jm_kbase_wait_done(UNUSED void *data,
                         UNUSED const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT],
                         UNUSED uint64_t abs_timeout_ns)
{
   return VK_SUCCESS;
}

/* base_jd_atom_v2 as understood by this kernel (empirically determined:
 * * KBASE_IOCTL_JOB_SUBMIT requires stride == sizeof(base_jd_atom_v2) and
 *   the only accepted sizes on this kbase are 64 and 72 bytes (the latter
 *   being sizeof + the v2 header's 8 bytes of "seq_nr"-free padding used by
 *   some builds).  The atom lives in *user memory*; the kernel copies it with
 *   copy_from_user and uses .jc as the GPU address of the job chain.
 * * core_req chooses the job slot (BASE_JD_REQ_T -> tiler,
 *   BASE_JD_REQ_FS -> fragment, BASE_JD_REQ_CS -> vertex/compute). */
#define BASE_JD_REQ_FS ((uint32_t)1 << 0)
#define BASE_JD_REQ_CS ((uint32_t)1 << 1)
#define BASE_JD_REQ_T ((uint32_t)1 << 2)
#define BASE_JD_REQ_V ((uint32_t)1 << 4)
#define BASE_JD_REQ_EXTERNAL_RESOURCES ((uint32_t)1 << 8)

#define BASE_EXT_RES_ACCESS_EXCLUSIVE 1ull
#define BASE_EXT_RES_COUNT_MAX 10

struct base_external_resource {
   uint64_t ext_resource;
};
#define BASE_JD_PRIO_MEDIUM 0u

struct base_dependency {
   uint8_t atom_id;
   uint8_t dependency_type;
} __attribute__((packed));

struct base_jd_atom_v2 {
   uint64_t jc;
   uint64_t udata[2];
   uint64_t extres_list;
   uint16_t nr_extres;
   uint8_t jit_id[2];
   struct base_dependency pre_dep[2];
   uint8_t atom_number;
   uint8_t prio;
   uint8_t device_nr;
   uint8_t jobslot;
   uint32_t core_req;
   uint8_t payload[16]; /* padding to 64 bytes */
} __attribute__((packed, aligned(16)));

static VkResult
panvk_kbase_wait_jobs(struct panvk_device *dev,
                     const struct base_jd_atom_v2 *atoms, unsigned count)
{
   bool pending[256] = { false };
   for (unsigned i = 0; i < count; i++)
      pending[atoms[i].atom_number] = true;

   VkResult result = VK_SUCCESS;
   const int64_t start_time = os_time_get_nano();
   const int64_t deadline = start_time + 60000000000ll;
   while (count) {
      int64_t remaining = deadline - os_time_get_nano();
      if (remaining <= 0) {
         mesa_loge("kbase: timed out waiting for %u JD atoms after %.2f ms", count,
                   (os_time_get_nano() - start_time) / 1000000.0);
         return VK_ERROR_DEVICE_LOST;
      }

      struct pollfd pfd = { .fd = dev->kmod.dev->fd, .events = POLLIN };
      int ret = poll(&pfd, 1, (remaining + 999999) / 1000000);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret <= 0 || !(pfd.revents & POLLIN))
         return VK_ERROR_DEVICE_LOST;

      struct base_jd_event_v2 ev;
      ssize_t len = read(dev->kmod.dev->fd, &ev, sizeof(ev));
      if (len < 0 && (errno == EINTR || errno == EAGAIN))
         continue;
      if (len != sizeof(ev)) {
         mesa_loge("kbase: invalid JD event read length %zd", len);
         return VK_ERROR_DEVICE_LOST;
      }

       if (unlikely(getenv("PANVK_VERBOSE")))
          fprintf(stderr, "PANVKDBG JD_EVENT: atom=%u code=0x%02x count=%u udata=0x%llx,0x%llx\n",
                  ev.atom_number, ev.event_code, count,
                  (unsigned long long)ev.udata[0], (unsigned long long)ev.udata[1]);
      if (!pending[ev.atom_number]) {
         mesa_loge("kbase: unexpected JD event for atom %u", ev.atom_number);
         return VK_ERROR_DEVICE_LOST;
      }

      pending[ev.atom_number] = false;
      count--;
      if (ev.event_code != BASE_JD_EVENT_DONE) {
         mesa_loge("kbase: atom %u failed with JD event 0x%02x after %.2f ms (udata=0x%llx,0x%llx)",
                   ev.atom_number, ev.event_code,
                   (os_time_get_nano() - start_time) / 1000000.0,
                   (unsigned long long)ev.udata[0], (unsigned long long)ev.udata[1]);
         result = VK_ERROR_DEVICE_LOST;
      }
   }

   return result;
}

static VkResult
panvk_kbase_jm_submit_batch(struct panvk_gpu_queue *queue,
                            struct panvk_cmd_buffer *cmdbuf,
                            struct panvk_batch *batch, uint32_t *bos,
                            unsigned nr_bos, uint32_t *in_fences,
                            unsigned nr_in_fences)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   ASSERTED int ret;

   if (unlikely(getenv("PANVK_VERBOSE")))
      fprintf(stderr, "PANVKDBG submit_batch kbase: batch=%p vtc=%s frag=%s bos=%u\n",
              (void *)batch,
              batch->vtc_jc.first_job ? "Y" : "N",
              batch->frag_jc.first_job ? "Y" : "N", nr_bos);
   mesa_logd("panvk: submit_batch start vtc=%s frag=%s",
             batch->vtc_jc.first_job ? "yes" : "no",
             batch->frag_jc.first_job ? "yes" : "no");

   if (batch->issued) {
      /*
       * PANVKDBG JOB384:
       * Inspect the complete Valhall MALLOC_VERTEX job after its previous
       * execution and BEFORE clearing the 16-byte GPU-written job status.
       */
      if (unlikely(getenv("PANVK_VERBOSE"))) {
         if (batch->vtc_jc.first_job) {
            const uint32_t *j384 =
               (const uint32_t *)(uintptr_t)batch->vtc_jc.first_job;

            fprintf(stderr,
                    "PANVKDBG JOB384_PRE_RESET batch=%p ptr=%016llx\n",
                    (void *)batch,
                    (unsigned long long)batch->vtc_jc.first_job);

            for (unsigned i = 0; i < 96; i += 8) {
               fprintf(stderr,
                       "PANVKDBG JOB384_PRE_RESET W%02u "
                       "%08x %08x %08x %08x "
                       "%08x %08x %08x %08x\n",
                       i,
                       j384[i + 0], j384[i + 1],
                       j384[i + 2], j384[i + 3],
                       j384[i + 4], j384[i + 5],
                       j384[i + 6], j384[i + 7]);
            }
         }

         if (batch->tiler.ctx_descs.cpu) {
            const uint32_t *tc_pre =
               (const uint32_t *)batch->tiler.ctx_descs.cpu;

            uint64_t q0 = ((uint64_t)tc_pre[1] << 32) | tc_pre[0];
            uint64_t q1 = ((uint64_t)tc_pre[3] << 32) | tc_pre[2];

            fprintf(stderr,
                    "PANVKDBG REUSE_PRE_RESET_TC batch=%p "
                    "q0=%016llx q1=%016llx "
                    "w=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                    (void *)batch,
                    (unsigned long long)q0,
                    (unsigned long long)q1,
                    tc_pre[0], tc_pre[1], tc_pre[2], tc_pre[3],
                    tc_pre[4], tc_pre[5], tc_pre[6], tc_pre[7]);
         }
      }

      /* GPU writes status/context data into the descriptor pool.
       * Invalidate CPU mappings before restoring descriptors for re-submit. */
      panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
      pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

      /*
       * Re-submit reset.
       *
       * The first 16 bytes of JOB_HEADER are GPU-written status.
       * On Valhall, MALLOC_VERTEX also writes Draw.Vertex array
       * (descriptor words 34..36). Restore that input state for
       * every MALLOC_VERTEX in the batch, not only first_job.
       */
#if PAN_ARCH >= 9
      unsigned reset_mv_idx = 0;
#endif

      util_dynarray_foreach(&batch->jobs, void *, job) {
         uint32_t *j = (uint32_t *)(*job);

#if PAN_ARCH >= 9
         /*
          * MALLOC_VERTEX is Valhall/PAN_ARCH >= 9.
          * Save the type before clearing the GPU-written status.
          */
         uint8_t job_type = (j[4] >> 1) & 0x7f;
#endif

         memset(j, 0, 4 * 4);

#if PAN_ARCH >= 9
         if (job_type == MALI_JOB_TYPE_MALLOC_VERTEX) {
            if (unlikely(getenv("PANVK_VERBOSE")))
               fprintf(stderr,
                       "PANVKDBG RESET_MV_ALL idx=%u ptr=%p "
                       "before=%08x %08x %08x\n",
                       reset_mv_idx, *job,
                       j[34], j[35], j[36]);

            /*
             * Restore the pristine Draw.Vertex array input.
             * The MALLOC_VERTEX hardware overwrites these words.
             */
            /*
             * MALLOC_VERTEX writes Draw.Vertex Array during execution.
             * Re-pack its pristine input state before re-submitting the
             * recorded command buffer.  At command generation time PanVK
             * initializes only Packet=true; Pointer and both strides are
             * hardware outputs and therefore start at zero.
             */
            struct mali_vertex_array_packed *vertex_array =
               (struct mali_vertex_array_packed *)&j[34];

            pan_pack(vertex_array, VERTEX_ARRAY, cfg) {
               cfg.packet = true;
            }

            if (unlikely(getenv("PANVK_VERBOSE")))
               fprintf(stderr,
                       "PANVKDBG RESET_MV_ALL idx=%u ptr=%p "
                       "after=%08x %08x %08x\n",
                       reset_mv_idx, *job,
                       j[34], j[35], j[36]);

            reset_mv_idx++;
         }
#endif
      }

      if (batch->tiler.ctx_descs.cpu) {
         memcpy(batch->tiler.heap_desc.cpu, &batch->tiler.heap_templ,
                sizeof(batch->tiler.heap_templ));

         struct mali_tiler_context_packed *ctxs =
            batch->tiler.ctx_descs.cpu;

         for (uint32_t i = 0; i < batch->fb.layer_count; i++)
            memcpy(&ctxs[i], &batch->tiler.ctx_templ, sizeof(*ctxs));
      }

      panvk_pool_flush_maps(&cmdbuf->desc_pool);
   }

   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

   if (unlikely(getenv("PANVK_VERBOSE"))) {
      /* Debug: compare the exact first job before first submit and re-submit. */
      fprintf(stderr,
              "PANVKDBG PRESUB issued=%u batch=%p vtc=%016llx frag=%016llx\n",
              batch->issued ? 1 : 0, (void *)batch,
              (unsigned long long)batch->vtc_jc.first_job,
              (unsigned long long)batch->frag_jc.first_job);

      if (batch->vtc_jc.first_job) {
         const uint32_t *j =
            (const uint32_t *)(uintptr_t)batch->vtc_jc.first_job;

         fprintf(stderr,
                 "PANVKDBG VTC16 "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x\n",
                 j[0], j[1], j[2], j[3],
                 j[4], j[5], j[6], j[7],
                 j[8], j[9], j[10], j[11],
                 j[12], j[13], j[14], j[15]);
      }

      if (batch->tiler.heap_desc.cpu) {
         const uint32_t *h =
            (const uint32_t *)batch->tiler.heap_desc.cpu;

         const uint32_t *ht =
            (const uint32_t *)&batch->tiler.heap_templ;

         fprintf(stderr,
                 "PANVKDBG HEAP gpu=%016llx "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x\n",
                 (unsigned long long)batch->tiler.heap_desc.gpu,
                 h[0], h[1], h[2], h[3],
                 h[4], h[5], h[6], h[7],
                 h[8], h[9], h[10], h[11],
                 h[12], h[13], h[14], h[15]);

         fprintf(stderr,
                 "PANVKDBG HEAPT "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x\n",
                 ht[0], ht[1], ht[2], ht[3],
                 ht[4], ht[5], ht[6], ht[7],
                 ht[8], ht[9], ht[10], ht[11],
                 ht[12], ht[13], ht[14], ht[15]);
      }

      if (batch->tiler.ctx_descs.cpu) {
         const uint32_t *tc =
            (const uint32_t *)batch->tiler.ctx_descs.cpu;

         fprintf(stderr,
                 "PANVKDBG TC16 "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x\n",
                 tc[0], tc[1], tc[2], tc[3],
                 tc[4], tc[5], tc[6], tc[7],
                 tc[8], tc[9], tc[10], tc[11],
                 tc[12], tc[13], tc[14], tc[15]);
      }

      {
         unsigned dbg_idx = 0;

         util_dynarray_foreach(&batch->jobs, void *, job) {
            const uint32_t *w = (const uint32_t *)(*job);

            fprintf(stderr,
                    "PANVKDBG JOB issued=%u idx=%u ptr=%p "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x\n",
                    batch->issued ? 1 : 0, dbg_idx++, *job,
                    w[0], w[1], w[2], w[3],
                    w[4], w[5], w[6], w[7],
                    w[8], w[9], w[10], w[11],
                    w[12], w[13], w[14], w[15],
                    w[16], w[17], w[18], w[19],
                    w[20], w[21], w[22], w[23],
                    w[24], w[25], w[26], w[27],
                    w[28], w[29], w[30], w[31]);
         }
      }

      if (batch->vtc_jc.first_job) {
         const uint32_t *j384 =
            (const uint32_t *)(uintptr_t)batch->vtc_jc.first_job;

         fprintf(stderr,
                 "PANVKDBG JOB384_PRESUB issued=%u batch=%p ptr=%016llx\n",
                 batch->issued ? 1 : 0,
                 (void *)batch,
                 (unsigned long long)batch->vtc_jc.first_job);

         for (unsigned i = 0; i < 96; i += 8) {
            fprintf(stderr,
                    "PANVKDBG JOB384_PRESUB issued=%u W%02u "
                    "%08x %08x %08x %08x "
                    "%08x %08x %08x %08x\n",
                    batch->issued ? 1 : 0, i,
                    j384[i + 0], j384[i + 1],
                    j384[i + 2], j384[i + 3],
                    j384[i + 4], j384[i + 5],
                    j384[i + 6], j384[i + 7]);
         }
      }
   }

   /* A draw chain starts with a MALLOC_VERTEX (Valhall IDVS) job, which
    * needs both the shader cores and the tiler: submitting it with only
    * BASE_JD_REQ_T makes the job fail with JOB_AFFINITY_FAULT (0x44).
    * Compute/NULL chains stay on the vertex/compute slot. */
   uint32_t vtc_core = BASE_JD_REQ_CS | BASE_JD_REQ_T;
   uint32_t frag_core = BASE_JD_REQ_FS;
   if (batch->vtc_jc.first_job) {
      /* Pick the job slot from the first job in the chain: compute (and
       * NULL sync) jobs run on the vertex/compute slot, draw chains on
       * the tiler slot. */
      uint32_t w0 = ((uint32_t *)(uintptr_t)batch->vtc_jc.first_job)[4];
      uint8_t job_type = (w0 >> 1) & 0x7f;
      if (job_type == MALI_JOB_TYPE_COMPUTE || job_type == MALI_JOB_TYPE_NULL)
         vtc_core = BASE_JD_REQ_CS;
   }
   if (unlikely(getenv("PANVK_VERBOSE")))
      fprintf(stderr, "PANVKDBG core envraw vtc=%s frag=%s\n",
              getenv("PANVK_KBASE_VTC_CORE"), getenv("PANVK_KBASE_FRAG_CORE"));
   if (getenv("PANVK_KBASE_VTC_CORE"))
      vtc_core = strtoul(getenv("PANVK_KBASE_VTC_CORE"), NULL, 0);
   if (getenv("PANVK_KBASE_FRAG_CORE"))
      frag_core = strtoul(getenv("PANVK_KBASE_FRAG_CORE"), NULL, 0);

   uint64_t userbuf_vas[BASE_EXT_RES_COUNT_MAX];
   struct base_external_resource extres[BASE_EXT_RES_COUNT_MAX];

   unsigned nr_extres =
      kbase_kmod_get_user_buffer_vas(dev->kmod.dev,
                                     userbuf_vas,
                                     ARRAY_SIZE(userbuf_vas));

   for (unsigned i = 0; i < nr_extres; i++) {
      assert((userbuf_vas[i] & 0xfff) == 0);
      extres[i].ext_resource =
         userbuf_vas[i] | BASE_EXT_RES_ACCESS_EXCLUSIVE;

      if (unlikely(getenv("PANVK_VERBOSE")))
         fprintf(stderr,
                 "PANVKDBG EXTRES[%u]=%016llx\n",
                 i,
                 (unsigned long long)extres[i].ext_resource);
   }

   bool use_split = (getenv("PANVK_SPLIT_SUBMIT") ||
                     getenv("PANVK_SPLIT_MASK") ||
#if PAN_ARCH >= 9
                     true
#else
                     false
#endif
                    ) && !getenv("PANVK_NO_SPLIT");

   if (use_split && batch->vtc_jc.first_job &&
       batch->frag_jc.first_job) {
      /* Submit vertex/tiler atom first, wait for completion, then submit fragment atom. */
      struct base_jd_atom_v2 vatom = {
         .jc = batch->vtc_jc.first_job,
         .atom_number = 1,
         .core_req = vtc_core,
      };
      if (nr_extres) {
         vatom.extres_list = (uint64_t)(uintptr_t)extres;
         vatom.nr_extres = nr_extres;
         vatom.core_req |= BASE_JD_REQ_EXTERNAL_RESOURCES;
      }
      struct kbase_ioctl_job_submit vsub = {
         .addr = (uint64_t)(uintptr_t)&vatom,
         .nr_atoms = 1,
         .stride = sizeof(vatom),
      };
      ret = pan_kmod_ioctl(dev->kmod.dev->fd, KBASE_IOCTL_JOB_SUBMIT, &vsub);
      if (ret) {
         mesa_loge("kbase: vtc submit failed: %s", strerror(errno));
         return VK_ERROR_DEVICE_LOST;
      }
      VkResult result = panvk_kbase_wait_jobs(dev, &vatom, 1);
      if (result != VK_SUCCESS)
         return result;

      if (batch->tiler.ctx_descs.cpu) {
         panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
         pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
         uint32_t *tc = (uint32_t *)batch->tiler.ctx_descs.cpu;
         uint64_t poly = ((uint64_t)tc[1] << 32) | tc[0];
         if (unlikely(getenv("PANVK_VERBOSE"))) {
            fprintf(stderr, "PANVKDBG SPLIT vtc done: poly=%016llx tc[2]=%08x tc[3]=%08x heap=%08x%08x\n",
                    (unsigned long long)poly, tc[2], tc[3], tc[7], tc[6]);
         }
         /* Do NOT mask poly pointer tag bits by default; Valhall JM tiler hardware tag bits
          * (0x00ff) in bits 48..55 are required by the fragment frontend. */
         if (getenv("PANVK_SPLIT_DOMASK")) {
            uint64_t masked = poly & 0x0000ffffffffffffull;
            tc[0] = (uint32_t)masked;
            tc[1] = (uint32_t)(masked >> 32);
            panvk_pool_flush_maps(&cmdbuf->desc_pool);
            pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
         }
      }

      struct base_jd_atom_v2 fatom = {
         .jc = batch->frag_jc.first_job,
         .atom_number = 2,
         .core_req = frag_core,
      };
      if (nr_extres) {
         fatom.extres_list = (uint64_t)(uintptr_t)extres;
         fatom.nr_extres = nr_extres;
         fatom.core_req |= BASE_JD_REQ_EXTERNAL_RESOURCES;
      }
      struct kbase_ioctl_job_submit fsub = {
         .addr = (uint64_t)(uintptr_t)&fatom,
         .nr_atoms = 1,
         .stride = sizeof(fatom),
      };
      ret = pan_kmod_ioctl(dev->kmod.dev->fd, KBASE_IOCTL_JOB_SUBMIT, &fsub);
      if (ret) {
         mesa_loge("kbase: frag submit failed: %s", strerror(errno));
         return VK_ERROR_DEVICE_LOST;
      }
      result = panvk_kbase_wait_jobs(dev, &fatom, 1);
      if (result != VK_SUCCESS) {
         panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
         pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
          if (batch->tiler.ctx_descs.cpu) {
             const uint32_t *tc = (const uint32_t *)batch->tiler.ctx_descs.cpu;
             fprintf(stderr,
                     "PANVKDBG split failed tiler ctx: poly=%08x%08x mask=%08x fbw=%u fbh=%u heap=%08x%08x\n",
                     tc[1], tc[0], tc[2], (tc[3] & 0xffff) + 1, ((tc[3] >> 16) & 0xffff) + 1, tc[7], tc[6]);
          }
         if (batch->fb.desc.cpu) {
            const uint32_t *f = (const uint32_t *)batch->fb.desc.cpu;
            fprintf(stderr, "PANVKDBG split failed FBD[0..7]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            fprintf(stderr, "PANVKDBG split failed FBD[8..15]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
            fprintf(stderr, "PANVKDBG split failed FBD[16..31]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]);
            fprintf(stderr, "PANVKDBG split failed RT0[0..7]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    f[32], f[33], f[34], f[35], f[36], f[37], f[38], f[39]);
            fprintf(stderr, "PANVKDBG split failed RT0[8..15]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    f[40], f[41], f[42], f[43], f[44], f[45], f[46], f[47]);
         }
         fprintf(stderr, "PANVKDBG split failed info: nr_extres=%u fatom_core_req=%08x\n",
                 nr_extres, fatom.core_req);
         for (unsigned i = 0; i < nr_extres; i++) {
            fprintf(stderr, "PANVKDBG split failed extres[%u]=%016llx\n",
                    i, (unsigned long long)extres[i].ext_resource);
         }
         if (dev->debug.decode_ctx) {
            if (batch->vtc_jc.first_job)
               pandecode_jc(dev->debug.decode_ctx, batch->vtc_jc.first_job,
                            to_panvk_physical_device(dev->vk.physical)->kmod.dev->props.gpu_id);
            if (batch->frag_jc.first_job)
               pandecode_jc(dev->debug.decode_ctx, batch->frag_jc.first_job,
                            to_panvk_physical_device(dev->vk.physical)->kmod.dev->props.gpu_id);
         }
         util_dynarray_foreach(&batch->jobs, void *, job) {
            const uint32_t *h = *job;
            fprintf(stderr,
                    "PANVKDBG split failed job: status=%08x task=%08x "
                    "fault=%08x%08x type_index=%08x\n",
                    h[0], h[1], h[3], h[2], h[4]);
            fprintf(stderr,
                    "PANVKDBG job payload: w5=%08x next=%08x%08x w8=%08x w9=%08x fbd=%08x%08x\n",
                    h[5], h[7], h[6], h[8], h[9], h[11], h[10]);
            uint64_t fault = ((uint64_t)h[3] << 32) | h[2];
            if (fault && dev->tiler_heap && dev->tiler_heap->addr.host) {
               uint64_t base = dev->tiler_heap->addr.dev;
               uint64_t size = pan_kmod_bo_size(dev->tiler_heap->bo);
               if (fault >= base && (fault - base) < size) {
                  uint64_t off = fault - base;
                  const uint32_t *pw = (const uint32_t *)(dev->tiler_heap->addr.host + off);
                  fprintf(stderr, "PANVKDBG fault mem @%016llx (off=%llx): %08x %08x %08x %08x %08x %08x %08x %08x\n",
                          (unsigned long long)fault, (unsigned long long)off,
                          pw[0], pw[1], pw[2], pw[3], pw[4], pw[5], pw[6], pw[7]);
               }
            }
         }
         return result;
      }
      batch->issued = true;
      return VK_SUCCESS;
   }

   {
      /* Submit the (optional) vertex/tiler chain and the (optional) fragment
       * chain as atoms in a single kbase job bag.  The fragment atom declares
       * a data dependency on the vertex/tiler atom so the scheduler keeps the
       * tiler->fragment ordering the hardware requires. */
      struct base_jd_atom_v2 atoms[2];
      unsigned nr_atoms = 0;

      memset(atoms, 0, sizeof(atoms));

      if (batch->vtc_jc.first_job) {
         atoms[nr_atoms].jc = batch->vtc_jc.first_job;
         atoms[nr_atoms].atom_number = 1;
         atoms[nr_atoms].core_req = vtc_core;

         if (nr_extres) {
            atoms[nr_atoms].extres_list =
               (uint64_t)(uintptr_t)extres;
            atoms[nr_atoms].nr_extres = nr_extres;
            atoms[nr_atoms].core_req |= BASE_JD_REQ_EXTERNAL_RESOURCES;

            if (unlikely(getenv("PANVK_VERBOSE")))
               fprintf(stderr,
                       "PANVKDBG VTC EXTRES count=%u core_req=%08x list=%p\n",
                       nr_extres,
                       atoms[nr_atoms].core_req,
                       (void *)extres);
         }

         nr_atoms++;
      }

      if (batch->frag_jc.first_job) {
         atoms[nr_atoms].jc = batch->frag_jc.first_job;
         atoms[nr_atoms].atom_number = 2;
         if (batch->vtc_jc.first_job) {
            atoms[nr_atoms].pre_dep[0].atom_id = 1;
            atoms[nr_atoms].pre_dep[0].dependency_type = 1; /* DATA */
         }
         atoms[nr_atoms].core_req = frag_core;

         if (nr_extres) {
            atoms[nr_atoms].extres_list =
               (uint64_t)(uintptr_t)extres;
            atoms[nr_atoms].nr_extres = nr_extres;
            atoms[nr_atoms].core_req |= BASE_JD_REQ_EXTERNAL_RESOURCES;

            if (unlikely(getenv("PANVK_VERBOSE")))
               fprintf(stderr,
                       "PANVKDBG FRAG EXTRES count=%u core_req=%08x list=%p\n",
                       nr_extres,
                       atoms[nr_atoms].core_req,
                       (void *)extres);
         }

         nr_atoms++;
      }

      if (nr_atoms) {
         struct kbase_ioctl_job_submit submit = {
            .addr = (uint64_t)(uintptr_t)atoms,
            .nr_atoms = nr_atoms,
            .stride = sizeof(atoms[0]),
         };

         if (PANVK_DEBUG(TRACE)) {
            panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
            pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
            if (batch->vtc_jc.first_job)
               pandecode_jc(dev->debug.decode_ctx, batch->vtc_jc.first_job,
                            to_panvk_physical_device(dev->vk.physical)->kmod.dev->props.gpu_id);
            if (batch->frag_jc.first_job)
               pandecode_jc(dev->debug.decode_ctx, batch->frag_jc.first_job,
                            to_panvk_physical_device(dev->vk.physical)->kmod.dev->props.gpu_id);
         }

         ret = pan_kmod_ioctl(dev->kmod.dev->fd, KBASE_IOCTL_JOB_SUBMIT, &submit);
         if (ret) {
            mesa_loge("kbase: KBASE_IOCTL_JOB_SUBMIT failed: %s", strerror(errno));
            return VK_ERROR_DEVICE_LOST;
         }
         if (unlikely(getenv("PANVK_VERBOSE")))
            fprintf(stderr,
                    "PANVKDBG JD submit ok vtc=%s frag=%s atoms=%u vtc_core=%x frag_core=%x\n",
                    batch->vtc_jc.first_job ? "Y" : "N",
                    batch->frag_jc.first_job ? "Y" : "N", nr_atoms, vtc_core,
                    frag_core);
         mesa_logd("panvk: job bag submit ok");

         VkResult result = panvk_kbase_wait_jobs(dev, atoms, nr_atoms);

         if (unlikely(getenv("PANVK_VERBOSE"))) {
            if (result == VK_SUCCESS && batch->frag_jc.first_job) {
               fprintf(stderr,
                       "PANVKDBG FRAG DONE: dumping native BOs\n");
               kbase_kmod_debug_dump_native_bos(dev->kmod.dev);
            }

            if (result == VK_SUCCESS && batch->vtc_jc.first_job) {
               fprintf(stderr,
                       "PANVKDBG VTC DONE: dumping USER_BUFFER mappings\n");
               kbase_kmod_debug_dump_user_buffers(dev->kmod.dev);
            }
         }

         if (result != VK_SUCCESS) {
            panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
            pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
            util_dynarray_foreach(&batch->jobs, void *, job) {
               const uint32_t *h = *job;
               fprintf(stderr,
                       "PANVKDBG failed batch job: status=%08x task=%08x "
                       "fault=%08x%08x type_index=%08x\n",
                       h[0], h[1], h[3], h[2], h[4]);
               fprintf(stderr,
                       "PANVKDBG job payload: w5=%08x next=%08x%08x w8=%08x w9=%08x fbd=%08x%08x\n",
                       h[5], h[7], h[6], h[8], h[9], h[11], h[10]);
            }
            if (batch->tiler.ctx_descs.cpu) {
               const uint32_t *tc = (const uint32_t *)batch->tiler.ctx_descs.cpu;
               fprintf(stderr,
                       "PANVKDBG tiler ctx: poly=%08x%08x mask=%08x fbw=%u fbh=%u heap=%08x%08x\n",
                       tc[1], tc[0], tc[2], (tc[3] & 0xffff) + 1, ((tc[3] >> 16) & 0xffff) + 1, tc[7], tc[6]);
            }
            return result;
         }
      }
   }

   if (getenv("PANVK_DUMP_HEAP") && batch->tiler.ctx_descs.cpu) {
      const uint32_t *tc = (const uint32_t *)batch->tiler.ctx_descs.cpu;
      uint64_t poly = ((uint64_t)tc[1] << 32) | tc[0];
      uint64_t heap = ((uint64_t)tc[7] << 32) | tc[6];
      fprintf(stderr,
              "PANVKDBG tiler ctx: poly_list=%016llx heap_desc=%016llx (tc[2]=%08x)\n",
              (unsigned long long)poly, (unsigned long long)heap, tc[2]);
      if (dev->tiler_heap && dev->tiler_heap->addr.host) {
         uint64_t base = dev->tiler_heap->addr.dev;
         uint64_t size = pan_kmod_bo_size(dev->tiler_heap->bo);
         fprintf(stderr,
                 "PANVKDBG heap: base=%016llx size=%llx poly_off=%llx\n",
                 (unsigned long long)base, (unsigned long long)size,
                 (unsigned long long)(poly - base));
         if (batch->tiler.heap_desc.cpu) {
            const uint32_t *hd = (const uint32_t *)batch->tiler.heap_desc.cpu;
            fprintf(stderr,
                    "PANVKDBG heap_desc: w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x w5=%08x w6=%08x w7=%08x\n",
                    hd[0], hd[1], hd[2], hd[3], hd[4], hd[5], hd[6], hd[7]);
         }
         unsigned char *h = dev->tiler_heap->addr.host;
         uint64_t masked = poly & 0x0000ffffffffffffull;
         if (masked >= base && (masked - base) < size) {
            uint64_t page = (masked - base) & ~0xfffull;
            panvk_priv_bo_invalidate(dev->tiler_heap, page, 0x2000);
            for (unsigned i = 0; i < 0x2000; i += 16) {
               const uint32_t *w = (const uint32_t *)(h + page + i);
               if (!(w[0] | w[1] | w[2] | w[3]))
                  continue;
               fprintf(stderr, "PANVKDBG poly[%05llx] %08x %08x %08x %08x\n",
                       (unsigned long long)(page + i), w[0], w[1], w[2], w[3]);
            }
         } else {
            fprintf(stderr, "PANVKDBG poly masked=%016llx off=%lld out of range\n",
                    (unsigned long long)masked, (long long)(masked - base));
         }
      }
   }

   batch->issued = true;
   mesa_logd("panvk: submit_batch end");
   return VK_SUCCESS;
}

VkResult
panvk_per_arch(kbase_jm_submit)(struct vk_queue *vk_queue,
                                struct panvk_gpu_queue *queue,
                                struct panvk_device *dev,
                                struct vk_queue_submit *submit)
{
   uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT] = {};

   if (unlikely(getenv("PANVK_VERBOSE")))
      fprintf(stderr, "PANVKDBG kbase submit: wait=%u signal=%u cmdbuf=%u\n",
              submit->wait_count, submit->signal_count,
              submit->command_buffer_count);
   mesa_logd("panvk: kbase gpu_queue_submit start, cmd_count=%u",
             submit->command_buffer_count);

   /* On kbase there are no DRM syncobjs: resolve incoming semaphore waits on
    * the CPU before emitting the jobs. */
   if (submit->wait_count) {
      VkResult result = vk_sync_wait_many(&dev->vk, submit->wait_count,
                                          submit->waits, VK_SYNC_WAIT_COMPLETE,
                                          UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

   for (uint32_t j = 0; j < submit->command_buffer_count; ++j) {
      struct panvk_cmd_buffer *cmdbuf =
         container_of(submit->command_buffers[j], struct panvk_cmd_buffer, vk);

      unsigned nb = 0;
      list_for_each_entry(struct panvk_batch, batch, &cmdbuf->batches, node)
         nb++;
      if (unlikely(getenv("PANVK_VERBOSE")))
         fprintf(stderr, "PANVKDBG kbase submit cmdbuf[%u]: batches=%u\n", j, nb);

      list_for_each_entry(struct panvk_batch, batch, &cmdbuf->batches, node) {
         VkResult result = panvk_kbase_jm_submit_batch(queue, cmdbuf, batch,
                                                      NULL, 0, NULL, 0);
         if (result != VK_SUCCESS)
            return vk_queue_set_lost(vk_queue, "kbase JM submission failed");
      }
   }

   /* Jobs are submitted sychronously (each KBASE_IOCTL_JOB_SUBMIT is waited
    * on before the next one), so the out fence needs no GPU-side
    * synchronization: arm the CPU syncs to be signalled on wait. */
   for (unsigned i = 0; i < submit->signal_count; i++) {
      assert(submit->signals[i].signal_value == 0);
      panvk_kbase_sync_set_pending(submit->signals[i].sync, queue,
                                   panvk_jm_kbase_wait_done, targets);
   }

   return VK_SUCCESS;
}
