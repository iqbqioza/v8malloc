/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Internal API surface — exposed for the api split. The v8m_api.c
 * core (lifecycle, malloc/free, aligned-alloc, diagnostics) keeps
 * dispatch_ready() and the live-stats collector. The companion TUs
 * `v8m_api_libc_compat.c` (mallinfo / malloc_info / mallopt / POSIX
 * symbol overrides / __libc_* aliases) and `v8m_api_cxx.c` (C++
 * Itanium operator new/delete) reach in via this header instead of
 * duplicating the helpers.
 *
 * Not part of the public ABI — header is internal-only and the
 * symbols are NOT listed in `v8malloc.map`.
 */

#ifndef V8M_API_INTERNAL_H
#define V8M_API_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Shared snapshot used by every reporter (mallinfo / mallinfo2 /
 * malloc_info / malloc_stats / v8m_get_frag_metrics). Built from the
 * page-heap counters so the reporters agree. Counters are monotonic;
 * pre-init / post-shutdown returns all zeroes.
 */
struct v8m_live_stats {
	uint64_t live_regions;
	uint64_t live_bytes;
	uint64_t bytes_mapped;
	uint64_t bytes_unmapped;
	uint64_t mmap_calls;
	uint64_t munmap_calls;
	uint64_t advise_calls;
};

void v8m_api_collect_live_stats(struct v8m_live_stats *out);

/* True iff the dispatcher has finished initialising and has not yet
 * been torn down. Reporters consult this before walking the page
 * heap or any other dispatcher-owned state. */
bool v8m_api_dispatch_ready(void);

#endif /* V8M_API_INTERNAL_H */
