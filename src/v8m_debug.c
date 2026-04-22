/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8m_config.h"
#include "v8m_debug.h"
#include "v8malloc/v8malloc.h" /* V8M_OPT_DEBUG */

void v8m_debug_uaf_poison(void *ptr, size_t bytes)
{
	if (bytes == 0U) {
		return;
	}
	if (v8m_config_get(V8M_OPT_DEBUG) == 0) {
		return;
	}
	(void)memset(ptr, V8M_DEBUG_POISON_BYTE, bytes);
}

void v8m_debug_uaf_verify(const void *ptr, size_t bytes, const char *backend)
{
	if (bytes == 0U) {
		return;
	}
	if (v8m_config_get(V8M_OPT_DEBUG) == 0) {
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
	if (v8m_config_get(V8M_OPT_DEBUG) == 0) {
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
