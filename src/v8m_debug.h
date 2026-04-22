/* SPDX-License-Identifier: Apache-2.0 */
/*
 * DEBUG-mode helpers shared between slab and buddy backends. The
 * existing per-allocation guard pages + red zones live on the
 * Large/Huge path (see src/v8m_large.c); slab and buddy can't
 * cleanly host trailing-overflow guards (slab metadata co-locates
 * with data, buddy arenas are multi-tenant), so this module ships
 * a complementary use-after-free WRITE detector: every freed slot
 * gets stamped with a poison pattern, and any subsequent dereference
 * of the freed memory either returns the poison bytes (often
 * crashing the caller's pointer chase deterministically) or, if the
 * caller writes to the freed slot, gets caught when the slot is
 * next allocated and the alloc path verifies the poison is intact.
 *
 * All helpers gate on `V8M_OPT_DEBUG` internally — release builds
 * (the default) pay zero cost.
 */

#ifndef V8M_DEBUG_H
#define V8M_DEBUG_H

#include <stddef.h>

/*
 * Poison byte. Distinct from the Large red-zone byte (0xCD) so a
 * dump of process memory can tell trailing-overflow canary bytes
 * (0xCD) apart from use-after-free poison (0xDF). The pattern is
 * also rare enough in real workloads that an accidental match is
 * unlikely to mask a true overflow.
 */
#define V8M_DEBUG_POISON_BYTE 0xDFU

/*
 * Stamp `bytes` of `ptr` with the poison pattern. No-op when
 * `V8M_OPT_DEBUG` is 0 or `bytes == 0`. Caller passes the bytes
 * AFTER any backend-internal free-list link area (slab small uses
 * the slot's first sizeof(void *) bytes for the next-pointer; the
 * link bytes are not poisoned).
 */
void v8m_debug_uaf_poison(void *ptr, size_t bytes);

/*
 * Verify that `bytes` of `ptr` are still the poison pattern from a
 * prior `v8m_debug_uaf_poison`. On mismatch, prints a diagnostic
 * naming `backend` (e.g. "tiny slab", "small slab") and the offset
 * of the corruption, then `abort()`s. No-op when `V8M_OPT_DEBUG` is
 * 0 or `bytes == 0`.
 */
void v8m_debug_uaf_verify(const void *ptr, size_t bytes, const char *backend);

/*
 * Convenience: poison every slot in a slab page's data area on
 * fresh-page init so the first-alloc path can verify uniformly
 * (the kernel hands us mmap-zero bytes, not poison bytes, so
 * without this the first verify would falsely fire). Caller
 * provides the data start, slot size, and capacity; the poison
 * runs slot-by-slot to preserve the small-slab free-list link
 * layout (link bytes excluded for the small backend per
 * `link_bytes`; pass 0 for tiny). No-op when DEBUG is off.
 */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void v8m_debug_uaf_poison_data_area(void *data_base, size_t object_size,
				    size_t capacity, size_t link_bytes);

#endif /* V8M_DEBUG_H */
