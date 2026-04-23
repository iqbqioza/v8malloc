/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bootstrap allocator implementation. A single static 64 KiB buffer,
 * an atomic bump offset, and a range-check predicate. No locks, no
 * dependencies on anything that could itself need malloc() — this
 * module has to work before the main allocator exists.
 */

#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "v8m_bootstrap.h"
#include "v8m_internal.h"

/*
 * Four 64 KiB pages of BSS carry the bootstrap allocator. 64 KiB
 * was enough for the bare libc-plus-dlsym boot path, but sanitizer
 * runtimes (UBSan, TSan) run their own constructor-chain init
 * ahead of ours and can comfortably eat 128-192 KiB before we
 * switch to the real allocator. 256 KiB is a cheap way to buy
 * headroom without touching the range-check predicate — the buffer
 * is still page-aligned BSS, `v8m_ptr_is_bootstrap` is a plain
 * address-range check, and the extra pages only materialize at
 * runtime if the constructor chain actually consumes them. */
#define V8M_BOOTSTRAP_SIZE (V8M_PAGE_SIZE * 4U)

/* C23 allows `alignas` to appear anywhere in the declaration-
 * specifier list, but clang < 19 rejects it positioned after
 * `static` (parses the paren-expression as an attribute-list).
 * Leading alignas works on every supported compiler. */
alignas(V8M_PAGE_SIZE) static unsigned char v8m_bootstrap_buffer
    [V8M_BOOTSTRAP_SIZE];
static atomic_size_t v8m_bootstrap_offset = 0;

static void v8m_bootstrap_oom(void)
{
	static const char msg[] = "v8malloc: bootstrap OOM\n";
	(void)write(STDERR_FILENO, msg, sizeof(msg) - 1U);
	abort();
}

void *v8m_bootstrap_alloc(size_t size)
{
	/* Reject any size that cannot fit in the bootstrap buffer before
	 * the round-up, so a near-SIZE_MAX request cannot wrap to a small
	 * `aligned` and silently hand the caller a 16-byte slot for a
	 * SIZE_MAX-bytes promise. The buffer is 256 KiB; anything larger
	 * is a programming error and aborts via the existing OOM path. */
	if (size > V8M_BOOTSTRAP_SIZE) {
		v8m_bootstrap_oom();
	}
	size_t aligned = (size + (V8M_BOOTSTRAP_ALIGN - 1U)) &
			 ~(size_t)(V8M_BOOTSTRAP_ALIGN - 1U);
	if (aligned == 0) {
		aligned = V8M_BOOTSTRAP_ALIGN;
	}

	size_t off = atomic_fetch_add_explicit(&v8m_bootstrap_offset, aligned,
					       memory_order_relaxed);
	if (off > V8M_BOOTSTRAP_SIZE || aligned > V8M_BOOTSTRAP_SIZE - off) {
		v8m_bootstrap_oom();
	}
	return &v8m_bootstrap_buffer[off];
}

bool v8m_ptr_is_bootstrap(const void *ptr)
{
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t start = (uintptr_t)v8m_bootstrap_buffer;
	return addr >= start && addr < start + V8M_BOOTSTRAP_SIZE;
}

size_t v8m_bootstrap_remaining(const void *ptr)
{
	if (!v8m_ptr_is_bootstrap(ptr)) {
		return 0;
	}
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t end = (uintptr_t)v8m_bootstrap_buffer + V8M_BOOTSTRAP_SIZE;
	return (size_t)(end - addr);
}
