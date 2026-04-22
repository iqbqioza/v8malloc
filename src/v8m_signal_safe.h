/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Async-signal-safe emergency allocator (architecture.md §6).
 * `malloc`/`free` are not async-signal-safe in general — the slab
 * pool's pthread_mutex would happily deadlock if a signal handler
 * fires on a thread that already holds it. This module is the
 * escape hatch: a 64 KiB BSS bump pool, atomic-only, no locks, no
 * syscalls, safe to call from any signal handler.
 *
 * Usage from inside a signal handler:
 *
 *     void on_signal(int sig)
 *     {
 *         char *buf = v8m_signal_safe_alloc(128);
 *         if (buf != NULL) {
 *             // ... use buf ...
 *         }
 *     }
 *
 * `free()` on a v8m_signal_safe_alloc pointer is a no-op (the
 * dispatcher's free path routes signal-safe pointers the same way
 * it routes bootstrap pointers — by range check, before touching
 * page metadata). The buffer leaks for the lifetime of the process
 * by design; the emergency budget is small enough that the leak is
 * bounded and the alternative (a free list with atomics) would not
 * be signal-safe under contention from non-signal-handler frees.
 *
 * v0 scope: standalone signal-safe path. The architecture.md
 * "L1-cache only inside signal handlers" optimisation depends on
 * the thread cache and lands with that cycle; the emergency buffer
 * is the only signal-safe surface today.
 */

#ifndef V8M_SIGNAL_SAFE_H
#define V8M_SIGNAL_SAFE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Minimum alignment of every returned pointer. Matches max_align_t.
 */
#define V8M_SIGNAL_SAFE_ALIGN 16

/*
 * Allocate `size` bytes from the signal-safe emergency buffer.
 * Returns NULL if the buffer is exhausted (handlers must check —
 * this is the one async-signal-safe failure mode the function
 * advertises). Returns a pointer aligned to V8M_SIGNAL_SAFE_ALIGN.
 *
 * Also declared in <v8malloc/v8malloc.h> as the public API; the
 * internal copy lets `v8m_api.c` see the prototype without
 * pulling the public header into every internal source file.
 * The duplication is intentional, hence the NOLINT.
 */
/* NOLINTNEXTLINE(readability-redundant-declaration) */
void *v8m_signal_safe_alloc(size_t size);

/*
 * True iff `ptr` was issued by v8m_signal_safe_alloc. The
 * dispatcher's free path consults this before reaching for page
 * metadata so a signal-safe pointer is recognised and dropped.
 */
bool v8m_ptr_is_signal_safe(const void *ptr);

/*
 * Bytes from `ptr` to the end of the signal-safe buffer — safe
 * upper bound on what `realloc` may read when copying out of a
 * signal-safe allocation. Returns 0 if `ptr` is not a signal-safe
 * pointer.
 */
size_t v8m_signal_safe_remaining(const void *ptr);

#endif /* V8M_SIGNAL_SAFE_H */
