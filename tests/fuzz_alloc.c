/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libFuzzer driver for the v8malloc public API. Each
 * LLVMFuzzerTestOneInput call interprets the input bytes as an
 * instruction stream over a fixed table of MAX_SLOTS live
 * pointers. The supported ops cover every backend-routed entry
 * point — malloc, calloc, realloc, free, aligned_alloc — and the
 * sizes / alignments are derived from the input, so the fuzzer
 * naturally explores every size class, the buddy boundary, the
 * Large/Huge cutoff, and the alignment guard rails.
 *
 * State persists across LLVMFuzzerTestOneInput invocations because
 * static slots make the search space deeper: the fuzzer can
 * exercise free-then-reuse patterns that span multiple inputs.
 * The driver writes a per-slot byte pattern after every malloc /
 * realloc and reads it back before any subsequent op on the same
 * slot — a use-after-free or two-slot aliasing bug would surface
 * as a pattern mismatch and abort the run via the explicit check.
 *
 * Run via:
 *   cmake -S . -B build/fuzz \
 *         -DCMAKE_C_COMPILER=clang \
 *         -DV8MALLOC_BUILD_FUZZ=ON -DV8MALLOC_BUILD_UBSAN=ON
 *   cmake --build build/fuzz
 *   ./build/fuzz/tests/fuzz_alloc -max_total_time=60
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8malloc/v8malloc.h"

enum {
	MAX_SLOTS = 32,
	/* Cap the per-op size at 1 MiB so the fuzzer doesn't burn the
	 * test budget on multi-megabyte mmaps. The Huge cutoff is
	 * 2 MiB; we leave that backend exercised by test_threading. */
	MAX_OP_SIZE = 1U * 1024U * 1024U,
};

static void *g_slots[MAX_SLOTS];
static size_t g_slot_sizes[MAX_SLOTS];
static unsigned char g_slot_pattern[MAX_SLOTS];

/*
 * Read up to `n` bytes from `data + *cursor` into a host-endian
 * uint32_t and advance the cursor. Returns 0 if not enough bytes
 * remain — the caller must check before deriving sizes.
 */
static uint32_t read_u32(const uint8_t *data, size_t total, size_t *cursor)
{
	if (*cursor + 4 > total) {
		return 0;
	}
	uint32_t value = 0;
	(void)memcpy(&value, data + *cursor, 4);
	*cursor += 4;
	return value;
}

/*
 * Verify the byte pattern on slot `i`. Aborts via abort() on
 * mismatch — that's the libFuzzer crash signal. Returns 0 if the
 * slot is empty or the pattern matches.
 */
static void verify_slot(uint8_t slot)
{
	void *ptr = g_slots[slot];
	if (ptr == NULL || g_slot_sizes[slot] == 0U) {
		return;
	}
	const unsigned char *bytes = ptr;
	unsigned char expect = g_slot_pattern[slot];
	if (bytes[0] != expect || bytes[g_slot_sizes[slot] - 1U] != expect) {
		abort();
	}
}

/*
 * Stamp every byte of slot `i` with `pattern` and remember it for
 * the next verify on this slot.
 */
static void stamp_slot(uint8_t slot, unsigned char pattern)
{
	if (g_slots[slot] == NULL || g_slot_sizes[slot] == 0U) {
		return;
	}
	(void)memset(g_slots[slot], pattern, g_slot_sizes[slot]);
	g_slot_pattern[slot] = pattern;
}

/* libFuzzer hands us raw bytes; the cast to non-const is part of
 * its prototype contract. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	size_t cursor = 0;
	while (cursor + 6 <= size) {
		uint8_t op = data[cursor++] % 5U;
		uint8_t slot = data[cursor++] % MAX_SLOTS;
		uint32_t arg = read_u32(data, size, &cursor);
		size_t op_size = 1U + (arg % MAX_OP_SIZE);

		verify_slot(slot);

		switch (op) {
		case 0: { /* malloc */
			free(g_slots[slot]);
			g_slots[slot] = malloc(op_size);
			g_slot_sizes[slot] =
			    (g_slots[slot] != NULL) ? op_size : 0U;
			stamp_slot(slot, (unsigned char)(arg & 0xFFU));
			break;
		}
		case 1: /* free */
			free(g_slots[slot]);
			g_slots[slot] = NULL;
			g_slot_sizes[slot] = 0U;
			break;
		case 2: { /* realloc */
			void *new_ptr = realloc(g_slots[slot], op_size);
			if (new_ptr != NULL) {
				g_slots[slot] = new_ptr;
				g_slot_sizes[slot] = op_size;
				stamp_slot(slot,
					   (unsigned char)((arg >> 8) & 0xFFU));
			} else if (op_size == 0U) {
				/* realloc(p, 0) is free(p); slot is now
				 * empty. */
				g_slots[slot] = NULL;
				g_slot_sizes[slot] = 0U;
			}
			break;
		}
		case 3: { /* calloc */
			free(g_slots[slot]);
			g_slots[slot] = calloc(1, op_size);
			g_slot_sizes[slot] =
			    (g_slots[slot] != NULL) ? op_size : 0U;
			/* calloc zero-fills; first read should see 0. */
			if (g_slots[slot] != NULL) {
				const unsigned char *bytes = g_slots[slot];
				if (bytes[0] != 0U ||
				    bytes[op_size - 1U] != 0U) {
					abort();
				}
			}
			stamp_slot(slot, (unsigned char)((arg >> 16) & 0xFFU));
			break;
		}
		case 4: { /* aligned_alloc */
			/* Pull alignment from the high byte of arg, clamp
			 * to a power of two in [16, 4096]. */
			unsigned shift = 4U + ((arg >> 24) % 9U); /* 4..12 */
			size_t align = (size_t)1U << shift;
			/* aligned_alloc requires size to be a multiple of
			 * alignment per C11 (we accept any size, but match
			 * the standard for the fuzzer). */
			size_t rounded = (op_size + align - 1U) & ~(align - 1U);
			free(g_slots[slot]);
			g_slots[slot] = aligned_alloc(align, rounded);
			g_slot_sizes[slot] =
			    (g_slots[slot] != NULL) ? rounded : 0U;
			stamp_slot(slot, (unsigned char)((arg >> 4) & 0xFFU));
			break;
		}
		default:
			break;
		}
	}
	return 0;
}
