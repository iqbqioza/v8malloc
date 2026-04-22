/* SPDX-License-Identifier: Apache-2.0 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_config.h"
#include "v8m_debug.h"
#include "v8malloc/v8malloc.h" /* V8M_OPT_DEBUG */

/*
 * UAF detector activation. Latched ONCE at first call instead of
 * read on every poison / verify, because the verify side is
 * intrinsically incompatible with a runtime DEBUG flip: pages
 * initialized while DEBUG was off were never pre-poisoned, so the
 * first alloc after a DEBUG=on flip would fail the verify on
 * uninitialized bytes. The one-shot latch matches the contract
 * "set V8M_DEBUG=1 in the env at process start to enable the UAF
 * detector — runtime v8m_set_option toggles do not". Other DEBUG
 * features (double-free ring, guard pages, red zones) keep their
 * runtime gating because they are not state-dependent across the
 * flip.
 *
 * 0 = not yet latched; 1 = latched off; 2 = latched on. Latched
 * via a single atomic CAS so concurrent first-callers observe the
 * same outcome.
 */
static atomic_int g_uaf_active = 0;

static bool uaf_active(void)
{
	int latched = atomic_load_explicit(&g_uaf_active, memory_order_relaxed);
	if (latched != 0) {
		return latched == 2;
	}
	int initial = (v8m_config_get(V8M_OPT_DEBUG) != 0) ? 2 : 1;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(
	    &g_uaf_active, &expected, initial, memory_order_relaxed,
	    memory_order_relaxed);
	latched = atomic_load_explicit(&g_uaf_active, memory_order_relaxed);
	return latched == 2;
}

void v8m_debug_uaf_poison(void *ptr, size_t bytes)
{
	if (bytes == 0U) {
		return;
	}
	if (!uaf_active()) {
		return;
	}
	(void)memset(ptr, V8M_DEBUG_POISON_BYTE, bytes);
}

void v8m_debug_uaf_verify(const void *ptr, size_t bytes, const char *backend)
{
	if (bytes == 0U) {
		return;
	}
	if (!uaf_active()) {
		return;
	}
	const unsigned char *bytes_ptr = (const unsigned char *)ptr;
	for (size_t i = 0; i < bytes; i++) {
		if (bytes_ptr[i] != (unsigned char)V8M_DEBUG_POISON_BYTE) {
			(void)fprintf(stderr,
				      "v8malloc DEBUG: use-after-free WRITE "
				      "detected at offset %zu of %s slot %p "
				      "(byte=0x%02x, expected=0x%02x)\n",
				      i, backend, ptr,
				      (unsigned int)bytes_ptr[i],
				      (unsigned int)V8M_DEBUG_POISON_BYTE);
			abort();
		}
	}
}

/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
void v8m_debug_uaf_poison_data_area(void *data_base, size_t object_size,
				    size_t capacity, size_t link_bytes)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
{
	if (capacity == 0U || object_size == 0U) {
		return;
	}
	if (!uaf_active()) {
		return;
	}
	if (link_bytes >= object_size) {
		return;
	}
	size_t bytes_per_slot = object_size - link_bytes;
	unsigned char *base = (unsigned char *)data_base;
	for (size_t i = 0; i < capacity; i++) {
		unsigned char *slot = base + (i * object_size);
		(void)memset(slot + link_bytes, V8M_DEBUG_POISON_BYTE,
			     bytes_per_slot);
	}
}
