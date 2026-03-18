/*
 * Copyright © 2024 Mesa kbase backend contributors
 * SPDX-License-Identifier: MIT
 *
 * kbase_kmod.c - pan_kmod backend for the Arm mali_kbase kernel driver.
 *
 * Allows Mesa Panfrost/PanVK to run on Android kernels that ship
 * mali_kbase (OEM/MTK/Samsung) instead of upstream Panfrost/Panthor.
 *
 * IOCTL reference: mali_kbase_ioctl.h (Arm DDK r32p0+, type 0x80)
 *
 * All four missing pieces are implemented here:
 *  1. bo_wait       - real event-queue wait: poll + KBASE_IOCTL_EVENT_DEQUEUE
 *  2. job_submit    - JM atom path: KBASE_IOCTL_JOB_SUBMIT + atom tracking
 *  3. bo_export     - dma-buf export: MEM_FLAGS_CHANGE + KBASE_IOCTL_MEM_SHARE
 *  4. eviction      - bo_make_evictable/unevictable via MEM_FLAGS_CHANGE
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"
#include "util/macros.h"
#include "util/os_mman.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"

#include "pan_kmod_backend.h"

/* ============================================================
 * mali_kbase ioctl definitions  (type 0x80)
 * ============================================================ */

#define KBASE_IOCTL_TYPE 0x80

struct kbase_ioctl_version_check { __u16 major; __u16 minor; };
#define KBASE_IOCTL_VERSION_CHECK \
   _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)

struct kbase_ioctl_set_flags { __u32 create_flags; };
#define KBASE_IOCTL_SET_FLAGS \
   _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)

struct kbase_ioctl_get_gpuprops { __u64 buffer; __u32 size; __u32 flags; };
#define KBASE_IOCTL_GET_GPUPROPS \
   _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)

/* GPU property blob: pairs of { u32 (key<<2|size_class) | value } */
#define KBASE_GPUPROP_VALUE_SIZE_U8  0
#define KBASE_GPUPROP_VALUE_SIZE_U16 1
#define KBASE_GPUPROP_VALUE_SIZE_U32 2
#define KBASE_GPUPROP_VALUE_SIZE_U64 3
#define KBASE_GPUPROP_PRODUCT_ID                1
#define KBASE_GPUPROP_MINOR_REVISION            3
#define KBASE_GPUPROP_MAJOR_REVISION            4
#define KBASE_GPUPROP_SHADER_PRESENT_LO        27
#define KBASE_GPUPROP_SHADER_PRESENT_HI        28
#define KBASE_GPUPROP_TILER_FEATURES           37
#define KBASE_GPUPROP_MEM_FEATURES             38
#define KBASE_GPUPROP_MMU_FEATURES             41
#define KBASE_GPUPROP_TEX_FEATURES_0           68
#define KBASE_GPUPROP_THREAD_MAX_THREADS       74
#define KBASE_GPUPROP_THREAD_MAX_WORKGROUP_SIZE 76
#define KBASE_GPUPROP_THREAD_FEATURES          77
#define KBASE_GPUPROP_CYCLE_COUNTER_FREQUENCY  87

union kbase_ioctl_mem_alloc {
   struct { __u64 va_pages; __u64 commit_pages; __u64 extent; __u64 flags; } in;
   struct { __u64 flags; __u64 gpu_va; } out;
};
#define KBASE_IOCTL_MEM_ALLOC \
   _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)

#define BASE_MEM_PROT_CPU_RD      (1u << 0)
#define BASE_MEM_PROT_CPU_WR      (1u << 1)
#define BASE_MEM_PROT_GPU_RD      (1u << 2)
#define BASE_MEM_PROT_GPU_WR      (1u << 3)
#define BASE_MEM_PROT_GPU_EX      (1u << 4)
#define BASE_MEM_GROW_ON_GPF      (1u << 9)
#define BASE_MEM_SAME_VA          (1u << 11)
#define BASE_MEM_COHERENT_SYSTEM  (1u << 12)
#define BASE_MEM_DONT_NEED        (1u << 16)
#define BASE_MEM_IMPORT_SHARED    (1u << 17)

struct kbase_ioctl_mem_free { __u64 gpu_addr; };
#define KBASE_IOCTL_MEM_FREE \
   _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)

union kbase_ioctl_mem_import {
   struct { __u64 phandle; __u32 type; __u32 padding; __u64 flags; } in;
   struct { __u64 flags; __u64 gpu_va; __u64 va_pages; } out;
};
#define KBASE_IOCTL_MEM_IMPORT \
   _IOWR(KBASE_IOCTL_TYPE, 9, union kbase_ioctl_mem_import)
#define KBASE_MEM_TYPE_IMPORTED_UMM 3

struct kbase_ioctl_mem_flags_change { __u64 gpu_va; __u64 flags; __u64 mask; };
#define KBASE_IOCTL_MEM_FLAGS_CHANGE \
   _IOW(KBASE_IOCTL_TYPE, 10, struct kbase_ioctl_mem_flags_change)

/* r38p0+ MEM_SHARE: returns a dma-buf fd for a locally-allocated BO */
struct kbase_ioctl_mem_share { __u64 gpu_va; __s32 out_fd; __u32 _pad; };
#define KBASE_IOCTL_MEM_SHARE \
   _IOWR(KBASE_IOCTL_TYPE, 0x33, struct kbase_ioctl_mem_share)

struct kbase_ioctl_sync {
   __u64 handle; __u64 user_addr; __u64 size; __u32 type; __u32 _pad;
};
#define KBASE_IOCTL_SYNC \
   _IOW(KBASE_IOCTL_TYPE, 8, struct kbase_ioctl_sync)
#define KBASE_SYNC_TO_DEVICE 0
#define KBASE_SYNC_TO_CPU    1

/* ============================================================
 * Job submission (JM - Job Manager, Bifrost/pre-CSF)
 * ============================================================ */

#define BASE_JD_PRIO_MEDIUM  0
#define BASE_JD_PRIO_HIGH    1
#define BASE_JD_PRIO_LOW     2

#define BASE_JD_REQ_FS                    (1u << 0)
#define BASE_JD_REQ_CS                    (1u << 1)
#define BASE_JD_REQ_T                     (1u << 2)
#define BASE_JD_REQ_CF                    (1u << 3)
#define BASE_JD_REQ_V                     (1u << 4)
#define BASE_JD_REQ_EXTERNAL_RESOURCES    (1u << 8)
#define BASE_JD_REQ_SOFT_JOB              (1u << 9)
#define BASE_JD_REQ_EVENT_ONLY_ON_FAILURE (1u << 12)
#define BASE_JD_REQ_EVENT_NEVER           (1u << 14)

struct base_external_resource { __u64 ext_resource; };

struct base_jd_atom_v2 {
   __u64 jc;
   struct { __u64 ptr; __u32 count; __u32 _padding; } extres_list;
   __u16 nr_extres;
   __u16 compat_core_req;
   struct { __s8 pre_dep[2]; } pre_dep[2];
   __u64 atom_number;
   __s8  prio;
   __u8  device_nr;
   __u16 _padding;
   __u32 core_req;
};

struct kbase_ioctl_job_submit { __u64 addr; __u32 nr_atoms; __u32 stride; };
#define KBASE_IOCTL_JOB_SUBMIT \
   _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)

/* ============================================================
 * Event queue (job completion notifications)
 * ============================================================ */

#define BASE_JD_EVENT_DONE      0x00
#define BASE_JD_EVENT_ERR_MASK  0x40

struct base_jd_event_v2 {
   __u64 event_code;
   __u64 atom_number;
   struct { __u64 status; __u64 udata[2]; } udata;
};
#define KBASE_IOCTL_EVENT_DEQUEUE \
   _IOR(KBASE_IOCTL_TYPE, 53, struct base_jd_event_v2)

/* ============================================================
 * Internal structures
 * ============================================================ */

const struct pan_kmod_ops kbase_kmod_ops;

#define KBASE_MAX_ATOMS 256

struct kbase_atom_slot {
   uint64_t atom_number;      /* 0 = free slot */
   struct pan_kmod_bo *bos[8];
   uint32_t nbo;
   bool completed;
   bool errored;
};

struct kbase_kmod_dev {
   struct pan_kmod_dev base;
   struct { uint32_t product_id; uint32_t major_rev; uint32_t minor_rev;
            uint64_t cycle_freq; } gpu_info;
   simple_mtx_t atoms_lock;
   struct kbase_atom_slot atoms[KBASE_MAX_ATOMS];
   uint64_t next_atom_number;
};

struct kbase_kmod_vm  { struct pan_kmod_vm base; };

struct kbase_kmod_bo {
   struct pan_kmod_bo base;
   uint64_t gpu_va;
   void    *cpu_ptr;   /* MAP_FAILED when unmapped */
   bool     exported;
   int      dmabuf_fd; /* -1 unless exported/imported */
};

/* ============================================================
 * Helpers
 * ============================================================ */

static inline int
kbase_ioctl(int fd, unsigned long req, void *arg)
{
   return ioctl(fd, req, arg);
}

static bool
kbase_parse_gpuprops(const uint8_t *buf, size_t len, uint32_t key, uint64_t *out)
{
   size_t off = 0;
   while (off + 4 <= len) {
      uint32_t kr; memcpy(&kr, buf + off, 4); off += 4;
      uint32_t sc = kr & 3, pid = kr >> 2;
      size_t vs;
      switch (sc) {
      case 0: vs = 1; break; case 1: vs = 2; break;
      case 2: vs = 4; break; default: vs = 8; break;
      }
      if (off + vs > len) return false;
      uint64_t v = 0; memcpy(&v, buf + off, vs); off += vs;
      if (pid == key) { *out = v; return true; }
   }
   return false;
}

static uint64_t
kbase_query_gpuprop(int fd, uint32_t prop_id)
{
   struct kbase_ioctl_get_gpuprops req = {0};
   int ret = kbase_ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
   if (ret < 0) return 0;
   uint8_t *buf = calloc(1, (size_t)ret);
   if (!buf) return 0;
   req.buffer = (uintptr_t)buf; req.size = (uint32_t)ret;
   uint64_t val = 0;
   if (kbase_ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req) >= 0)
      kbase_parse_gpuprops(buf, (size_t)ret, prop_id, &val);
   free(buf); return val;
}

/* ============================================================
 * Atom table
 * ============================================================ */

static struct kbase_atom_slot *
kbase_atom_alloc(struct kbase_kmod_dev *kd, struct pan_kmod_bo **bos, uint32_t nbo)
{
   simple_mtx_lock(&kd->atoms_lock);
   uint64_t num = ++kd->next_atom_number;
   if (!num) num = ++kd->next_atom_number;
   for (int i = 0; i < KBASE_MAX_ATOMS; i++) {
      if (!kd->atoms[i].atom_number) {
         struct kbase_atom_slot *s = &kd->atoms[i];
         s->atom_number = num; s->completed = s->errored = false;
         s->nbo = MIN2(nbo, ARRAY_SIZE(s->bos));
         for (uint32_t j = 0; j < s->nbo; j++) s->bos[j] = bos[j];
         simple_mtx_unlock(&kd->atoms_lock); return s;
      }
   }
   simple_mtx_unlock(&kd->atoms_lock);
   mesa_loge("kbase: atom table full"); return NULL;
}

static void
kbase_atom_process_event_locked(struct kbase_kmod_dev *kd,
                                 const struct base_jd_event_v2 *ev)
{
   for (int i = 0; i < KBASE_MAX_ATOMS; i++) {
      if (kd->atoms[i].atom_number == ev->atom_number) {
         kd->atoms[i].completed = true;
         kd->atoms[i].errored   = !!(ev->event_code & BASE_JD_EVENT_ERR_MASK);
         return;
      }
   }
}

static bool
kbase_wait_atom(struct kbase_kmod_dev *kd, uint64_t atom_num, int64_t timeout_ns)
{
   int fd = kd->base.fd;
   int64_t deadline = 0; bool use_dl = timeout_ns >= 0;
   if (use_dl) {
      struct timespec now; clock_gettime(CLOCK_MONOTONIC_RAW, &now);
      deadline = (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec + timeout_ns;
   }
   for (;;) {
      /* Check completion */
      simple_mtx_lock(&kd->atoms_lock);
      for (int i = 0; i < KBASE_MAX_ATOMS; i++) {
         if (kd->atoms[i].atom_number == atom_num && kd->atoms[i].completed) {
            bool ok = !kd->atoms[i].errored;
            kd->atoms[i].atom_number = 0;
            simple_mtx_unlock(&kd->atoms_lock); return ok;
         }
      }
      simple_mtx_unlock(&kd->atoms_lock);

      int ms = -1;
      if (use_dl) {
         struct timespec now; clock_gettime(CLOCK_MONOTONIC_RAW, &now);
         int64_t rem = deadline - ((int64_t)now.tv_sec * 1000000000ll + now.tv_nsec);
         if (rem <= 0) return false;
         ms = (int)(rem / 1000000); if (!ms) ms = 1;
      }

      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      int r = poll(&pfd, 1, ms);
      if (r < 0) { if (errno == EINTR) continue; return false; }
      if (r == 0) return false;

      struct base_jd_event_v2 ev;
      while (kbase_ioctl(fd, KBASE_IOCTL_EVENT_DEQUEUE, &ev) == 0) {
         simple_mtx_lock(&kd->atoms_lock);
         kbase_atom_process_event_locked(kd, &ev);
         simple_mtx_unlock(&kd->atoms_lock);
      }
   }
}

/* ============================================================
 * Device
 * ============================================================ */

static void
kbase_dev_query_props(struct kbase_kmod_dev *kd)
{
   struct pan_kmod_dev_props *p = &kd->base.props;
   int fd = kd->base.fd;
   memset(p, 0, sizeof(*p));

   uint32_t pid  = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_PRODUCT_ID);
   uint32_t maj  = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_MAJOR_REVISION);
   uint32_t min  = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_MINOR_REVISION);
   p->gpu_id = (pid << 16) | ((maj & 0xff) << 8) | (min & 0xff);

   uint32_t slo  = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_SHADER_PRESENT_LO);
   uint32_t shi  = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_SHADER_PRESENT_HI);
   p->shader_present = ((uint64_t)shi << 32) | slo;

   p->tiler_features = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_TILER_FEATURES);
   p->mem_features   = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_MEM_FEATURES);
   p->mmu_features   = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_MMU_FEATURES);
   for (unsigned i = 0; i < ARRAY_SIZE(p->texture_features); i++)
      p->texture_features[i] =
         (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_TEX_FEATURES_0 + i);

   p->max_threads_per_core =
      (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_THREAD_MAX_THREADS);
   if (!p->max_threads_per_core) p->max_threads_per_core = 256;
   p->max_threads_per_wg =
      (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_THREAD_MAX_WORKGROUP_SIZE);
   if (!p->max_threads_per_wg) p->max_threads_per_wg = p->max_threads_per_core;
   uint32_t tf = (uint32_t)kbase_query_gpuprop(fd, KBASE_GPUPROP_THREAD_FEATURES);
   p->max_tasks_per_core     = MAX2(tf >> 24, 1);
   p->num_registers_per_core = tf & 0xffff;
   if (!p->num_registers_per_core)
      p->num_registers_per_core = p->max_threads_per_core * 32;
   p->max_tls_instance_per_core = p->max_threads_per_core;

   kd->gpu_info.cycle_freq =
      kbase_query_gpuprop(fd, KBASE_GPUPROP_CYCLE_COUNTER_FREQUENCY);
   if (kd->gpu_info.cycle_freq) {
      p->gpu_can_query_timestamp  = true;
      p->timestamp_frequency      = kd->gpu_info.cycle_freq;
      p->timestamp_device_coherent = true;
   }

   p->supported_bo_flags =
      PAN_KMOD_BO_FLAG_EXECUTABLE | PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT |
      PAN_KMOD_BO_FLAG_NO_MMAP    | PAN_KMOD_BO_FLAG_GPU_UNCACHED;
   p->is_io_coherent = false;
   p->allowed_group_priorities_mask =
      PAN_KMOD_GROUP_ALLOW_PRIORITY_LOW    |
      PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM |
      PAN_KMOD_GROUP_ALLOW_PRIORITY_HIGH;

   kd->gpu_info.product_id = pid;
   kd->gpu_info.major_rev  = maj;
   kd->gpu_info.minor_rev  = min;
}

static struct pan_kmod_dev *
kbase_kmod_dev_create(int fd, uint32_t flags, drmVersionPtr version,
                      const struct pan_kmod_allocator *allocator)
{
   struct kbase_ioctl_version_check vc = { .major = 11, .minor = 0 };
   if (kbase_ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc) < 0) {
      mesa_loge("kbase: VERSION_CHECK failed err=%d", errno); return NULL;
   }
   mesa_logi("kbase: DDK %u.%u", vc.major, vc.minor);
   struct kbase_ioctl_set_flags sf = {0};
   if (kbase_ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) < 0)
      mesa_logw("kbase: SET_FLAGS failed err=%d (continuing)", errno);

   struct kbase_kmod_dev *kd = pan_kmod_alloc(allocator, sizeof(*kd));
   if (!kd) { mesa_loge("kbase: OOM"); return NULL; }

   drmVersion fv = { .version_major = vc.major, .version_minor = vc.minor };
   pan_kmod_dev_init(&kd->base, fd, flags, &fv, &kbase_kmod_ops, allocator);
   simple_mtx_init(&kd->atoms_lock, mtx_plain);
   memset(kd->atoms, 0, sizeof(kd->atoms));
   kd->next_atom_number = 0;
   kbase_dev_query_props(kd);
   return &kd->base;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   struct kbase_kmod_dev *kd = container_of(dev, struct kbase_kmod_dev, base);
   simple_mtx_destroy(&kd->atoms_lock);
   pan_kmod_dev_cleanup(dev);
   pan_kmod_free(dev->allocator, kd);
}

static struct pan_kmod_va_range
kbase_kmod_dev_query_user_va_range(const struct pan_kmod_dev *dev)
{
   (void)dev;
   return (struct pan_kmod_va_range){
      .start = 32ull << 20,
      .size  = (1ull << 48) - (32ull << 20),
   };
}

/* ============================================================
 * Buffer Objects
 * ============================================================ */

static uint64_t
pan_flags_to_kbase(uint32_t f)
{
   uint64_t k = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
   if (f & PAN_KMOD_BO_FLAG_EXECUTABLE) k |= BASE_MEM_PROT_GPU_EX;
   if (f & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) k |= BASE_MEM_GROW_ON_GPF;
   return k;
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *dev, struct pan_kmod_vm *vm,
                    uint64_t size, uint32_t flags)
{
   struct kbase_kmod_bo *kbo = pan_kmod_dev_alloc(dev, sizeof(*kbo));
   if (!kbo) return NULL;

   uint64_t pages = DIV_ROUND_UP(size, 4096);
   union kbase_ioctl_mem_alloc a = { .in = {
      .va_pages     = pages,
      .commit_pages = (flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) ? 0 : pages,
      .extent       = (flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) ? pages : 0,
      .flags        = pan_flags_to_kbase(flags),
   }};
   if (kbase_ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC, &a) < 0) {
      mesa_loge("kbase: MEM_ALLOC size=%"PRIu64" err=%d", size, errno);
      pan_kmod_dev_free(dev, kbo); return NULL;
   }
   kbo->gpu_va    = a.out.gpu_va;
   kbo->cpu_ptr   = MAP_FAILED;
   kbo->exported  = false;
   kbo->dmabuf_fd = -1;

   if (!(flags & PAN_KMOD_BO_FLAG_NO_MMAP)) {
      void *p = mmap((void *)(uintptr_t)kbo->gpu_va, pages * 4096,
                     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                     dev->fd, (off_t)kbo->gpu_va);
      if (p != MAP_FAILED) kbo->cpu_ptr = p;
      else mesa_logw("kbase: mmap 0x%"PRIx64" failed err=%d", kbo->gpu_va, errno);
   }
   pan_kmod_bo_init(&kbo->base, dev, vm, pages * 4096, flags,
                    (uint32_t)(kbo->gpu_va & 0xFFFFFFFF));
   return &kbo->base;
}

static void
kbase_kmod_bo_free(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   if (kbo->dmabuf_fd >= 0) close(kbo->dmabuf_fd);
   if (kbo->cpu_ptr != MAP_FAILED) munmap(kbo->cpu_ptr, bo->size);
   struct kbase_ioctl_mem_free mf = { .gpu_addr = kbo->gpu_va };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FREE, &mf) < 0)
      mesa_logw("kbase: MEM_FREE 0x%"PRIx64" err=%d", kbo->gpu_va, errno);
   pan_kmod_dev_free(bo->dev, kbo);
}

static struct pan_kmod_bo *
kbase_kmod_bo_import(struct pan_kmod_dev *dev, uint32_t handle,
                     uint64_t size, uint32_t flags)
{
   int dfd = (int)handle;
   struct kbase_kmod_bo *kbo = pan_kmod_dev_alloc(dev, sizeof(*kbo));
   if (!kbo) return NULL;
   union kbase_ioctl_mem_import mi = { .in = {
      .phandle = (uint64_t)(uintptr_t)&dfd,
      .type    = KBASE_MEM_TYPE_IMPORTED_UMM,
      .flags   = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                 BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA,
   }};
   if (kbase_ioctl(dev->fd, KBASE_IOCTL_MEM_IMPORT, &mi) < 0) {
      mesa_loge("kbase: MEM_IMPORT err=%d", errno);
      pan_kmod_dev_free(dev, kbo); return NULL;
   }
   kbo->gpu_va    = mi.out.gpu_va;
   kbo->cpu_ptr   = MAP_FAILED;
   kbo->exported  = false;
   kbo->dmabuf_fd = dup(dfd);
   pan_kmod_bo_init(&kbo->base, dev, NULL, mi.out.va_pages * 4096,
                    flags | PAN_KMOD_BO_FLAG_IMPORTED,
                    (uint32_t)(kbo->gpu_va & 0xFFFFFFFF));
   return &kbo->base;
}

/*
 * bo_export (part 3)
 *
 * For imported BOs: return the saved dmabuf fd.
 * For locally-allocated BOs:
 *   - Try KBASE_IOCTL_MEM_SHARE (r38p0+) which returns a dma-buf fd directly.
 *   - Before calling MEM_SHARE, mark the BO shareable via MEM_FLAGS_CHANGE.
 *   - On older DDKs this will fail; we log and return -1.
 */
static int
kbase_kmod_bo_export(struct pan_kmod_bo *bo, int unused_fd)
{
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   (void)unused_fd;

   if (kbo->exported && kbo->dmabuf_fd >= 0) return kbo->dmabuf_fd;
   if ((bo->flags & PAN_KMOD_BO_FLAG_IMPORTED) && kbo->dmabuf_fd >= 0) {
      kbo->exported = true; return kbo->dmabuf_fd;
   }

   /* Mark shareable */
   struct kbase_ioctl_mem_flags_change fc = {
      .gpu_va = kbo->gpu_va,
      .flags  = BASE_MEM_IMPORT_SHARED,
      .mask   = BASE_MEM_IMPORT_SHARED,
   };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc) < 0)
      mesa_logw("kbase: MEM_FLAGS_CHANGE for export err=%d", errno);

   /* Try MEM_SHARE (r38p0+) */
   struct kbase_ioctl_mem_share ms = { .gpu_va = kbo->gpu_va, .out_fd = -1 };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_SHARE, &ms) == 0 && ms.out_fd >= 0) {
      kbo->dmabuf_fd = ms.out_fd;
      kbo->exported  = true;
      bo->flags |= PAN_KMOD_BO_FLAG_EXPORTED;
      return kbo->dmabuf_fd;
   }

   mesa_loge("kbase: bo_export unsupported on this DDK (need r38p0+ for MEM_SHARE)");
   return -1;
}

static off_t
kbase_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   return (off_t)kbo->gpu_va; /* SAME_VA: mmap offset == gpu_va */
}

static int
kbase_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   util_dynarray_foreach(&dev->pending_bo_syncs.array,
                         struct pan_kmod_deferred_bo_sync, sync) {
      struct kbase_kmod_bo *kbo = container_of(sync->bo, struct kbase_kmod_bo, base);
      if (kbo->cpu_ptr == MAP_FAILED) continue;
      struct kbase_ioctl_sync ks = {
         .handle    = kbo->gpu_va,
         .user_addr = (uint64_t)(uintptr_t)kbo->cpu_ptr + sync->start,
         .size      = sync->size,
         .type      = (sync->type == PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH)
                        ? KBASE_SYNC_TO_DEVICE : KBASE_SYNC_TO_CPU,
      };
      if (kbase_ioctl(dev->fd, KBASE_IOCTL_SYNC, &ks) < 0)
         mesa_logw("kbase: SYNC err=%d", errno);
   }
   return 0;
}

/*
 * bo_wait (part 1)
 *
 * Find all pending atoms that reference this BO and wait for each.
 */
static bool
kbase_kmod_bo_wait(struct pan_kmod_bo *bo, int64_t timeout_ns,
                   bool for_read_only_access)
{
   (void)for_read_only_access;
   struct kbase_kmod_dev *kd = container_of(bo->dev, struct kbase_kmod_dev, base);

   uint64_t pending[KBASE_MAX_ATOMS];
   uint32_t np = 0;
   simple_mtx_lock(&kd->atoms_lock);
   for (int i = 0; i < KBASE_MAX_ATOMS; i++) {
      if (!kd->atoms[i].atom_number) continue;
      for (uint32_t j = 0; j < kd->atoms[i].nbo; j++) {
         if (kd->atoms[i].bos[j] == bo) {
            pending[np++] = kd->atoms[i].atom_number; break;
         }
      }
   }
   simple_mtx_unlock(&kd->atoms_lock);

   for (uint32_t i = 0; i < np; i++)
      if (!kbase_wait_atom(kd, pending[i], timeout_ns)) return false;
   return true;
}

/*
 * Eviction hints (part 4)
 *
 * kbase exposes eviction via BASE_MEM_DONT_NEED flag through MEM_FLAGS_CHANGE.
 * Setting it signals the kernel that the BO pages can be reclaimed.
 * Clearing it pins them back.
 */
static void
kbase_kmod_bo_make_evictable(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   struct kbase_ioctl_mem_flags_change fc = {
      .gpu_va = kbo->gpu_va,
      .flags  = BASE_MEM_DONT_NEED,
      .mask   = BASE_MEM_DONT_NEED,
   };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc) < 0)
      mesa_logw("kbase: make_evictable err=%d", errno);
}

static bool
kbase_kmod_bo_make_unevictable(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   struct kbase_ioctl_mem_flags_change fc = {
      .gpu_va = kbo->gpu_va,
      .flags  = 0,
      .mask   = BASE_MEM_DONT_NEED,
   };
   if (kbase_ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc) < 0) {
      mesa_logw("kbase: make_unevictable err=%d", errno); return false;
   }
   return true;
}

/* ============================================================
 * VM
 * ============================================================ */

static struct pan_kmod_vm *
kbase_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                     uint64_t va_start, uint64_t va_range)
{
   (void)va_start; (void)va_range;
   struct kbase_kmod_vm *vm = pan_kmod_dev_alloc(dev, sizeof(*vm));
   if (!vm) return NULL;
   pan_kmod_vm_init(&vm->base, dev, 0, flags | PAN_KMOD_VM_FLAG_AUTO_VA, PAN_PGSIZE_4K);
   return &vm->base;
}

static void kbase_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   pan_kmod_dev_free(vm->dev, vm);
}

static int
kbase_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                   struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   (void)vm; (void)mode;
   for (uint32_t i = 0; i < op_count; i++) {
      if (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP) {
         struct kbase_kmod_bo *kbo = container_of(ops[i].map.bo, struct kbase_kmod_bo, base);
         if (ops[i].va.start == PAN_KMOD_VM_MAP_AUTO_VA)
            ops[i].va.start = kbo->gpu_va + ops[i].map.bo_offset;
      } else if (ops[i].type != PAN_KMOD_VM_OP_TYPE_UNMAP &&
                 ops[i].type != PAN_KMOD_VM_OP_TYPE_SYNC_ONLY) {
         mesa_loge("kbase: unknown vm_bind op %d", ops[i].type); return -1;
      }
   }
   return 0;
}

/* ============================================================
 * Job submission helper (part 2)
 * Exposed publicly so pan_jm / panfrost_job can call it on kbase devices.
 * ============================================================ */

uint64_t
kbase_kmod_job_submit(struct pan_kmod_dev *dev,
                      uint64_t jc, uint32_t core_req,
                      struct pan_kmod_bo **bos, uint32_t nbo,
                      struct base_external_resource *ext_res, uint32_t next_res)
{
   struct kbase_kmod_dev *kd = container_of(dev, struct kbase_kmod_dev, base);
   struct kbase_atom_slot *slot = kbase_atom_alloc(kd, bos, nbo);
   if (!slot) return 0;

   struct base_jd_atom_v2 atom = {
      .jc          = jc,
      .core_req    = core_req,
      .atom_number = slot->atom_number,
      .prio        = BASE_JD_PRIO_MEDIUM,
      .device_nr   = 0,
   };
   if (ext_res && next_res) {
      atom.extres_list.ptr   = (uint64_t)(uintptr_t)ext_res;
      atom.extres_list.count = next_res;
      atom.nr_extres         = (uint16_t)next_res;
      atom.core_req         |= BASE_JD_REQ_EXTERNAL_RESOURCES;
   }
   for (int i = 0; i < 2; i++) {
      atom.pre_dep[i].pre_dep[0] = -1;
      atom.pre_dep[i].pre_dep[1] = -1;
   }

   struct kbase_ioctl_job_submit sub = {
      .addr     = (uint64_t)(uintptr_t)&atom,
      .nr_atoms = 1,
      .stride   = sizeof(atom),
   };
   if (kbase_ioctl(dev->fd, KBASE_IOCTL_JOB_SUBMIT, &sub) < 0) {
      mesa_loge("kbase: JOB_SUBMIT err=%d", errno);
      simple_mtx_lock(&kd->atoms_lock);
      slot->atom_number = 0;
      simple_mtx_unlock(&kd->atoms_lock);
      return 0;
   }
   return slot->atom_number;
}

/* ============================================================
 * Timestamp
 * ============================================================ */

static uint64_t
kbase_kmod_query_timestamp(const struct pan_kmod_dev *dev)
{
   (void)dev;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ============================================================
 * Ops table
 * ============================================================ */

const struct pan_kmod_ops kbase_kmod_ops = {
   .dev_create              = kbase_kmod_dev_create,
   .dev_destroy             = kbase_kmod_dev_destroy,
   .dev_query_user_va_range = kbase_kmod_dev_query_user_va_range,

   .bo_alloc                = kbase_kmod_bo_alloc,
   .bo_free                 = kbase_kmod_bo_free,
   .bo_import               = kbase_kmod_bo_import,
   .bo_export               = kbase_kmod_bo_export,       /* part 3 */
   .bo_get_mmap_offset      = kbase_kmod_bo_get_mmap_offset,
   .flush_bo_map_syncs      = kbase_kmod_flush_bo_map_syncs,
   .bo_wait                 = kbase_kmod_bo_wait,         /* part 1 */

   .bo_make_evictable       = kbase_kmod_bo_make_evictable,   /* part 4 */
   .bo_make_unevictable     = kbase_kmod_bo_make_unevictable, /* part 4 */

   .vm_create               = kbase_kmod_vm_create,
   .vm_destroy              = kbase_kmod_vm_destroy,
   .vm_bind                 = kbase_kmod_vm_bind,

   .query_timestamp         = kbase_kmod_query_timestamp,
   .bo_set_label            = NULL,
};
