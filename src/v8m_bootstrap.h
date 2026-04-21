/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bootstrap allocator — satisfies malloc() calls that arrive before
 * the main allocator has finished initializing. A fixed 64 KiB
 * page-aligned buffer with an atomic bump pointer; lock-free, and
 * impossible to free (free() on a bootstrap pointer is a no-op once
 * the main allocator routes it here).
 *
 * The bootstrap is only active on the cold path; once the main
 * allocator is up, new allocations flow through it. Bootstrap-issued
 * pointers survive that transition — v8m_ptr_is_bootstrap() lets the
 * main free() path detect them before reaching for page metadata.
 *
 * See architecture.md §5.
 */

#ifndef V8M_BOOTSTRAP_H
#define V8M_BOOTSTRAP_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Minimum alignment of every returned pointer. Matches max_align_t on
 * the ABIs v8malloc targets (16 bytes on x86_64 and aarch64).
 */
#define V8M_BOOTSTRAP_ALIGN 16

/*
 * Allocate `size` bytes from the bootstrap buffer. Returns a pointer
 * aligned to V8M_BOOTSTRAP_ALIGN.
 *
 * Aborts the process on exhaustion — by design; the bootstrap buffer
 * is sized for the handful of allocations expected during early init,
 * not general use.
 */
void *v8m_bootstrap_alloc(size_t size);

/*
 * Test whether `ptr` was issued by v8m_bootstrap_alloc(). Hot path:
 * free() calls this first so that pre-init pointers are recognized
 * before touching the page-metadata machinery.
 */
bool v8m_ptr_is_bootstrap(const void *ptr);

/*
 * Bytes from `ptr` to the end of the bootstrap buffer (a safe upper
 * bound on what realloc may read when copying out of a bootstrap
 * allocation — the per-allocation size is not tracked). Returns 0 if
 * `ptr` is not a bootstrap pointer.
 */
size_t v8m_bootstrap_remaining(const void *ptr);

#endif /* V8M_BOOTSTRAP_H */
