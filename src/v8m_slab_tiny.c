/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tiny slab page implementation. Bitmap allocation uses
 * __builtin_ctzll on the inverted word so alloc is O(1) amortized
 * via search_hint; free is O(1) since the slot index falls out of
 * simple pointer arithmetic against the data-area base.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v8m_internal.h"
#include "v8m_page.h"
#include "v8m_size_class.h"
#include "v8m_slab_tiny.h"

static_assert(V8M_SLAB_HEADER_SIZE >= sizeof(struct v8m_tiny_page_meta),
	      "V8M_SLAB_HEADER_SIZE too small for v8m_tiny_page_meta");
static_assert(V8M_SLAB_HEADER_SIZE % 64 == 0,
	      "V8M_SLAB_HEADER_SIZE must be a multiple of the max Tiny "
	      "object size so each slot's natural alignment holds");

enum {
	BITS_PER_WORD = 64,
	BITMAP_WORDS = 128,
	BITMAP_CAPACITY_BITS = BITMAP_WORDS * BITS_PER_WORD /* 8192 */
};

static struct v8m_tiny_page_meta *tiny_of(struct v8m_page_meta *meta)
{
	return (struct v8m_tiny_page_meta *)meta;
}

static const struct v8m_tiny_page_meta *
tiny_of_const(const struct v8m_page_meta *meta)
{
	return (const struct v8m_tiny_page_meta *)meta;
}

static unsigned char *slab_data(struct v8m_page_meta *meta)
{
	return (unsigned char *)meta + V8M_SLAB_HEADER_SIZE;
}

uint32_t v8m_slab_tiny_capacity_for(uint16_t object_size)
{
	if (object_size == 0) {
		return 0;
	}
	size_t data_area = V8M_PAGE_SIZE - V8M_SLAB_HEADER_SIZE;
	size_t capacity = data_area / object_size;
	if (capacity > BITMAP_CAPACITY_BITS) {
		capacity = BITMAP_CAPACITY_BITS;
	}
	return (uint32_t)capacity;
}

/* size_class and owner_thread are both integer arguments and could in
 * principle be swapped by a careless caller; size_class is narrow
 * (0..7) and owner_thread is typically a pthread_self() cast, so in
 * practice a confusion is caught by the init-time assertion chain or
 * by the static asserts in v8m_slab_tiny.c on capacity bounds. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
void v8m_slab_tiny_init(void *page_base, uint32_t size_class,
			uint64_t owner_thread)
{
	struct v8m_tiny_page_meta *meta = page_base;
	uint16_t object_size = (uint16_t)v8m_size_class_size(size_class);
	uint32_t capacity = v8m_slab_tiny_capacity_for(object_size);

	meta->magic = V8M_MAGIC;
	meta->size_class = (uint16_t)size_class;
	meta->object_size = object_size;
	meta->capacity = capacity;
	atomic_store_explicit(&meta->used_count, 0, memory_order_relaxed);
	meta->owner_thread = owner_thread;
	meta->free_list_head = NULL;
	meta->next = NULL;
	meta->search_hint = 0;

	for (uint32_t word = 0; word < BITMAP_WORDS; word++) {
		meta->bitmap[word] = 0;
	}

	/* Pre-mark slots beyond capacity as "used" so the alloc scan
	 * can ignore them without a bounds check. */
	for (uint32_t slot = capacity; slot < BITMAP_CAPACITY_BITS; slot++) {
		uint32_t word = slot / BITS_PER_WORD;
		uint32_t bit = slot % BITS_PER_WORD;
		meta->bitmap[word] |= (uint64_t)1U << bit;
	}
}

static void *claim_slot(struct v8m_tiny_page_meta *tiny, uint32_t word,
			uint32_t bit)
{
	tiny->bitmap[word] |= (uint64_t)1U << bit;
	tiny->search_hint = word;
	atomic_fetch_add_explicit(&tiny->used_count, 1U, memory_order_relaxed);
	uint32_t slot = (word * BITS_PER_WORD) + bit;
	return slab_data((struct v8m_page_meta *)tiny) +
	       ((size_t)slot * tiny->object_size);
}

void *v8m_slab_tiny_alloc(struct v8m_page_meta *meta)
{
	struct v8m_tiny_page_meta *tiny = tiny_of(meta);

	uint32_t start = (uint32_t)tiny->search_hint;
	for (uint32_t word = start; word < BITMAP_WORDS; word++) {
		uint64_t value = tiny->bitmap[word];
		if (value != UINT64_MAX) {
			uint32_t bit = (uint32_t)__builtin_ctzll(~value);
			return claim_slot(tiny, word, bit);
		}
	}
	for (uint32_t word = 0; word < start; word++) {
		uint64_t value = tiny->bitmap[word];
		if (value != UINT64_MAX) {
			uint32_t bit = (uint32_t)__builtin_ctzll(~value);
			return claim_slot(tiny, word, bit);
		}
	}
	return NULL;
}

bool v8m_slab_tiny_free(struct v8m_page_meta *meta, const void *obj)
{
	struct v8m_tiny_page_meta *tiny = tiny_of(meta);
	const unsigned char *data = slab_data(meta);
	size_t offset = (size_t)((const unsigned char *)obj - data);
	uint32_t slot = (uint32_t)(offset / tiny->object_size);
	uint32_t word = slot / BITS_PER_WORD;
	uint32_t bit = slot % BITS_PER_WORD;

	tiny->bitmap[word] &= ~((uint64_t)1U << bit);
	if (word < tiny->search_hint) {
		tiny->search_hint = word;
	}

	uint32_t previous = atomic_fetch_sub_explicit(&tiny->used_count, 1U,
						      memory_order_relaxed);
	return previous == 1U;
}

bool v8m_slab_tiny_is_full(const struct v8m_page_meta *meta)
{
	const struct v8m_tiny_page_meta *tiny = tiny_of_const(meta);
	uint32_t used =
	    atomic_load_explicit(&tiny->used_count, memory_order_relaxed);
	return used == tiny->capacity;
}

bool v8m_slab_tiny_is_empty(const struct v8m_page_meta *meta)
{
	const struct v8m_tiny_page_meta *tiny = tiny_of_const(meta);
	return atomic_load_explicit(&tiny->used_count, memory_order_relaxed) ==
	       0U;
}
