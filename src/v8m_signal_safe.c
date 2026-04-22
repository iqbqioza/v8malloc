/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Async-signal-safe emergency allocator. Same shape as bootstrap
 * (atomic bump over a static BSS buffer, leaks for the process
 * lifetime by design) but a separate buffer so signal-handler
 * traffic does not contend with the constructor-chain budget that
 * bootstrap reserves for early init. Returns NULL on exhaustion
 * instead of aborting because a SIGSEGV inside a signal handler is
 * worse than a NULL the handler can check.
 */

#include "v8m_signal_safe.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h> /* size_t, NULL */
#include <stdint.h>

#include "v8m_internal.h"
#include "v8malloc/v8malloc.h" /* V8M_EXPORT */

/*
 * 64 KiB of BSS for the emergency budget. Sized to fit a handful
 * of medium allocations (e.g. a backtrace symbolizer's scratch +
 * a fixed-size logging buffer) without tipping the cost into the
 * megabytes a misguided signal handler could otherwise spend.
 * Page-aligned so the address range check is cheap and the buffer
 * never straddles a page that the kernel might decide to reclaim
 * via THP coalescing while a signal is in flight.
 */
#define V8M_SIGNAL_SAFE_SIZE V8M_PAGE_SIZE

alignas(V8M_PAGE_SIZE) static unsigned char v8m_signal_safe_buffer
    [V8M_SIGNAL_SAFE_SIZE];
static atomic_size_t v8m_signal_safe_offset = 0;

V8M_EXPORT void *v8m_signal_safe_alloc(size_t size)
{
	size_t aligned = (size + (V8M_SIGNAL_SAFE_ALIGN - 1U)) &
			 ~(size_t)(V8M_SIGNAL_SAFE_ALIGN - 1U);
	if (aligned == 0) {
		aligned = V8M_SIGNAL_SAFE_ALIGN;
	}

	size_t off = atomic_fetch_add_explicit(&v8m_signal_safe_offset, aligned,
					       memory_order_relaxed);
	if (off > V8M_SIGNAL_SAFE_SIZE ||
	    aligned > V8M_SIGNAL_SAFE_SIZE - off) {
		/* Out of emergency budget. The handler must check the
		 * NULL — there is no way to abort safely from this
		 * context (write + abort are both signal-safe but
		 * losing the signal handler's work to abort is
		 * surprising; NULL lets the handler choose). */
		return NULL;
	}
	return &v8m_signal_safe_buffer[off];
}

bool v8m_ptr_is_signal_safe(const void *ptr)
{
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t start = (uintptr_t)v8m_signal_safe_buffer;
	return addr >= start && addr < start + V8M_SIGNAL_SAFE_SIZE;
}

size_t v8m_signal_safe_remaining(const void *ptr)
{
	if (!v8m_ptr_is_signal_safe(ptr)) {
		return 0;
	}
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t end =
	    (uintptr_t)v8m_signal_safe_buffer + V8M_SIGNAL_SAFE_SIZE;
	return (size_t)(end - addr);
}
