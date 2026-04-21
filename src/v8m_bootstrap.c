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
 * The buffer is sized to match v8malloc's internal page granularity
 * so the bootstrap allocator occupies exactly one 64 KiB page, and
 * the matching alignment lets any bootstrap pointer round down to
 * the buffer base under V8M_PAGE_MASK. Callers never rely on that
 * second property — v8m_ptr_is_bootstrap() is a range check — but it
 * keeps the invariants of the free() fast path simple.
 */
#define V8M_BOOTSTRAP_SIZE V8M_PAGE_SIZE

static alignas(
    V8M_PAGE_SIZE) unsigned char v8m_bootstrap_buffer[V8M_BOOTSTRAP_SIZE];
static atomic_size_t v8m_bootstrap_offset = 0;

static void v8m_bootstrap_oom(void)
{
	static const char msg[] = "v8malloc: bootstrap OOM\n";
	(void)write(STDERR_FILENO, msg, sizeof(msg) - 1U);
	abort();
}

void *v8m_bootstrap_alloc(size_t size)
{
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
