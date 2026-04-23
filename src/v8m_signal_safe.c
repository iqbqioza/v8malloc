/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Async-signal-safe emergency allocator. Wraps the shared bump-
 * pointer primitive (v8m_bump.{h,c}) with a separate buffer so
 * signal-handler traffic does not contend with the constructor-
 * chain budget that bootstrap reserves for early init. Returns
 * NULL on exhaustion (handlers must check) instead of aborting,
 * because a SIGSEGV inside a signal handler is worse than a NULL
 * the handler can branch on.
 */

#include "v8m_signal_safe.h"

#include <stdalign.h>
#include <stddef.h>

#include "v8m_bump.h"
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
static struct v8m_bump v8m_signal_safe_bump = {
    .buffer = v8m_signal_safe_buffer,
    .size = V8M_SIGNAL_SAFE_SIZE,
    .align = V8M_SIGNAL_SAFE_ALIGN,
    .offset = 0,
};

V8M_EXPORT void *v8m_signal_safe_alloc(size_t size)
{
	return v8m_bump_alloc(&v8m_signal_safe_bump, size);
}

bool v8m_ptr_is_signal_safe(const void *ptr)
{
	return v8m_bump_owns(&v8m_signal_safe_bump, ptr);
}

size_t v8m_signal_safe_remaining(const void *ptr)
{
	return v8m_bump_remaining(&v8m_signal_safe_bump, ptr);
}
