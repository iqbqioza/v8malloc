/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Libc fallback path. v8malloc lives behind LD_PRELOAD-style
 * interposition: every call to malloc/free/calloc/realloc in the
 * process is routed through our public API. But two situations leave
 * us holding a pointer the dispatcher cannot identify:
 *
 *  1. Pre-init allocations made by libraries whose constructors run
 *     before ours (rare given our priority(101) constructor, but still
 *     possible for code that calls malloc from inside dlopen()).
 *  2. Pointers produced by the libc allocator itself when v8malloc is
 *     loaded into a process that already had outstanding allocations
 *     (less of a concern — v8malloc is loaded at process start in the
 *     LD_PRELOAD case).
 *
 * Rather than silently dropping these pointers (the v0 behavior, which
 * leaks memory), we resolve the next "free" symbol on the dynamic
 * search path via dlsym(RTLD_NEXT) and forward the foreign pointer
 * there. dlsym itself may allocate; our pre-init malloc override
 * serves those requests from the bootstrap allocator, so the
 * resolution is safe to run from inside the constructor.
 *
 * If RTLD_NEXT does not resolve (we're not preloaded — the user
 * statically linked us into the binary, say), the captured pointers
 * stay NULL and the foreign-pointer branch falls back to the
 * original drop-on-the-floor behavior. That's still better than
 * crashing.
 */

#ifndef V8M_LIBC_FALLBACK_H
#define V8M_LIBC_FALLBACK_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Resolve the libc-side malloc/free/calloc/realloc symbols via
 * dlsym(RTLD_NEXT, ...) and cache them. Idempotent — safe to call
 * more than once but the second call is a no-op. Must be invoked
 * before the first foreign-pointer free; in practice the library
 * constructor calls it before v8m_dispatch_init runs.
 */
void v8m_libc_fallback_init(void);

/*
 * True iff v8m_libc_fallback_init successfully captured the libc
 * free pointer. Tests use this to decide whether the foreign-pointer
 * fallback can be exercised.
 */
bool v8m_libc_fallback_ready(void);

/*
 * Forward `ptr` to the captured libc free. Tolerates NULL. If the
 * fallback was not captured (dlsym returned NULL), the call is a
 * no-op — better to leak than to crash.
 */
void v8m_libc_free(void *ptr);

/*
 * Allocate `size` bytes via the captured libc malloc. Returns NULL
 * if the fallback was not captured or if the libc allocator itself
 * failed. Used by tests that want to fabricate a foreign pointer
 * without going through v8malloc's own malloc.
 */
void *v8m_libc_malloc(size_t size);

#endif /* V8M_LIBC_FALLBACK_H */
