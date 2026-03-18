/*
 * Copyright © 2024 Mesa kbase backend contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "pan_kmod.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * kbase_kmod_ops - pan_kmod backend for Arm mali_kbase kernel driver.
 *
 * Use when the kernel exposes /dev/mali0 via mali_kbase (Android OEM/MTK/Samsung)
 * instead of upstream Panfrost/Panthor DRM.
 */
extern const struct pan_kmod_ops kbase_kmod_ops;

/**
 * struct base_external_resource - external dma-buf resource for job submission.
 */
struct base_external_resource {
   uint64_t ext_resource; /* gpu_va | access (bit 0: 0=non-exclusive, 1=exclusive) */
};

/**
 * kbase_kmod_job_submit - Submit a JM job chain directly via mali_kbase.
 *
 * @dev:       pan_kmod_dev created with kbase_kmod_ops
 * @jc:        GPU VA of the first job descriptor in the chain
 * @core_req:  BASE_JD_REQ_* bitmask (FS=fragment, CS=compute, T=tiler, V=vertex...)
 * @bos:       array of BOs touched by this job (for bo_wait() tracking)
 * @nbo:       number of BOs in the array
 * @ext_res:   external dma-buf resource descriptors (may be NULL)
 * @next_res:  number of external resources
 *
 * Returns the atom_number assigned (non-zero) on success, 0 on failure.
 * The caller can later call pan_kmod_bo_wait() on any BO in @bos and it will
 * block until this atom (and all others touching that BO) have completed.
 */
uint64_t kbase_kmod_job_submit(struct pan_kmod_dev *dev,
                               uint64_t jc, uint32_t core_req,
                               struct pan_kmod_bo **bos, uint32_t nbo,
                               struct base_external_resource *ext_res,
                               uint32_t next_res);

#ifdef __cplusplus
} /* extern "C" */
#endif
