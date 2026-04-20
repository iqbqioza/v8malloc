/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Internal cross-cutting constants and macros shared by every
 * subsystem of v8malloc. Public API constants live in
 * include/v8malloc/v8malloc.h; everything declared here is hidden
 * from consumers via -fvisibility=hidden plus the V8M_INTERNAL
 * attribute on any symbols that need explicit marking.
 */

#ifndef V8M_INTERNAL_H
#define V8M_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "v8m_arch.h"

#define V8M_INTERNAL __attribute__((visibility("hidden")))

/* --- Page geometry -------------------------------------------------- */
/*
 * v8malloc operates on a fixed 64 KiB internal allocation unit,
 * chosen independently of the OS page size. Every slab, buddy, and
 * extent page is aligned to V8M_PAGE_SIZE so that masking the low
 * bits of any pointer recovers the page-base address — and thus the
 * page metadata header — in O(1) with no table lookup. See
 * architecture.md §4.1.
 */
#define V8M_PAGE_SHIFT 16
#define V8M_PAGE_SIZE ((size_t)1 << V8M_PAGE_SHIFT)
#define V8M_PAGE_MASK (~(uintptr_t)(V8M_PAGE_SIZE - 1U))

#endif /* V8M_INTERNAL_H */
