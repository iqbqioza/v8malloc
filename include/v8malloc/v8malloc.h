/* SPDX-License-Identifier: Apache-2.0 */
/*
 * v8malloc — high-performance memory allocator for Linux.
 *
 * This header is the sole public entry point. Consumers include it as
 *
 *	#include <v8malloc/v8malloc.h>
 *
 * The library may be used in three ways:
 *
 *  1. LD_PRELOAD interposition — every malloc/free in the process is
 *     transparently routed through v8malloc with no source changes.
 *  2. Direct linking against libv8malloc.so / libv8malloc.a — replaces
 *     the libc allocator at link time.
 *  3. Side-by-side use via the namespaced v8m_* API below — useful when
 *     mixing allocators is required (the default libc allocator stays
 *     in place for everything else).
 *
 * See man v8malloc(3) for the full surface; the design is documented in
 * the project's .claude/docs/ tree.
 */

#ifndef V8MALLOC_V8MALLOC_H
#define V8MALLOC_V8MALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Compile-time version ------------------------------------------------ */

#define V8M_VERSION_MAJOR 0
#define V8M_VERSION_MINOR 1
#define V8M_VERSION_PATCH 0

#define V8M_VERSION_STRING "0.1.0"

/*
 * Numeric form: (major * 10000) + (minor * 100) + patch. Useful for
 * preprocessor comparisons such as `#if V8M_VERSION >= 10200`.
 */
#define V8M_VERSION                                                            \
	((V8M_VERSION_MAJOR * 10000) + (V8M_VERSION_MINOR * 100) +             \
	 V8M_VERSION_PATCH)

/* --- Symbol visibility --------------------------------------------------- */

#if defined(V8MALLOC_BUILDING)
#define V8M_EXPORT __attribute__((visibility("default")))
#else
#define V8M_EXPORT
#endif

/* --- Runtime version API ------------------------------------------------- */

/*
 * Return the version string the library was compiled with, e.g. "0.1.0".
 * The returned pointer has static storage duration; callers must not
 * free or modify it.
 */
V8M_EXPORT const char *v8m_version(void);

V8M_EXPORT int v8m_version_major(void);
V8M_EXPORT int v8m_version_minor(void);
V8M_EXPORT int v8m_version_patch(void);

/* --- Allocation API (v8m_-prefixed, namespaced) ------------------- */

#include <stddef.h>

/*
 * Namespaced allocation API. The standard `malloc`, `free`,
 * `calloc`, `realloc`, `reallocarray`, and `malloc_usable_size`
 * symbols are also exported by the library and route to the same
 * implementations; consumers get those prototypes from <stdlib.h>
 * and <malloc.h> as usual. The v8m_* names exist so a program can
 * call into v8malloc explicitly even when the standard symbols are
 * resolved to a different allocator.
 */
V8M_EXPORT void *v8m_malloc(size_t size);
V8M_EXPORT void v8m_free(void *ptr);
V8M_EXPORT void *v8m_calloc(size_t nmemb, size_t size);
V8M_EXPORT void *v8m_realloc(void *ptr, size_t size);
V8M_EXPORT void *v8m_reallocarray(void *ptr, size_t nmemb, size_t size);
V8M_EXPORT size_t v8m_malloc_usable_size(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* V8MALLOC_V8MALLOC_H */
