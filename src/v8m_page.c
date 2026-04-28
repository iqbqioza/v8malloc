/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-page metadata: layout invariants pinned at compile time. Most
 * of the metadata machinery is inlined from v8m_page.h; this file
 * exists so the static_asserts below get exactly one translation
 * unit to live in, ensuring a build failure the moment a layout
 * regresses (common-prefix offsets, struct sizes, page geometry).
 */

#include <assert.h>
#include <stddef.h>

#include "v8m_internal.h"
#include "v8m_page.h"

/*
 * The Tiny metadata layout extends the common one: every leading
 * field must live at the same offset so that generic code can read
 * magic, size_class, etc. via a v8m_page_meta pointer regardless of
 * the slab's actual type.
 */
static_assert(offsetof(struct v8m_tiny_page_meta, magic) ==
		  offsetof(struct v8m_page_meta, magic),
	      "Tiny meta: magic offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, size_class) ==
		  offsetof(struct v8m_page_meta, size_class),
	      "Tiny meta: size_class offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, object_size) ==
		  offsetof(struct v8m_page_meta, object_size),
	      "Tiny meta: object_size offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, capacity) ==
		  offsetof(struct v8m_page_meta, capacity),
	      "Tiny meta: capacity offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, used_count) ==
		  offsetof(struct v8m_page_meta, used_count),
	      "Tiny meta: used_count offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, owner_thread) ==
		  offsetof(struct v8m_page_meta, owner_thread),
	      "Tiny meta: owner_thread offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, free_list_head) ==
		  offsetof(struct v8m_page_meta, free_list_head),
	      "Tiny meta: free_list_head offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, next) ==
		  offsetof(struct v8m_page_meta, next),
	      "Tiny meta: next offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, arena_id) ==
		  offsetof(struct v8m_page_meta, arena_id),
	      "Tiny meta: arena_id offset diverges from common layout");
static_assert(offsetof(struct v8m_tiny_page_meta, owner_cpu) ==
		  offsetof(struct v8m_page_meta, owner_cpu),
	      "Tiny meta: owner_cpu offset diverges from common layout");

/*
 * Both metadata structures must fit within the per-page metadata
 * reservation (~2 KiB, see size-classes.md §3). The tiny metadata is
 * the larger of the two because of its in-page bitmap.
 */
static_assert(sizeof(struct v8m_page_meta) <= 2048,
	      "v8m_page_meta exceeds the per-page metadata reservation");
static_assert(sizeof(struct v8m_tiny_page_meta) <= 2048,
	      "v8m_tiny_page_meta exceeds the per-page metadata reservation");

/*
 * The page-base mask in v8m_ptr_to_meta assumes V8M_PAGE_SIZE is a
 * power of two. A mistake here would silently misplace metadata.
 */
static_assert((V8M_PAGE_SIZE & (V8M_PAGE_SIZE - 1)) == 0,
	      "V8M_PAGE_SIZE must be a power of two");
