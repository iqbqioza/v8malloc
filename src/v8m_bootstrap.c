/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bootstrap allocator implementation. Wraps the shared bump-pointer
 * primitive (v8m_bump.{h,c}) with the bootstrap-specific buffer
 * size, BSS alignment, and out-of-budget contract (abort — process-
 * fatal is correct before the real allocator exists).
 */

#include <stdalign.h>
#include <stdlib.h>
#include <unistd.h>

#include "v8m_bootstrap.h"
#include "v8m_bump.h"
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
static struct v8m_bump v8m_bootstrap_bump = {
    .buffer = v8m_bootstrap_buffer,
    .size = V8M_BOOTSTRAP_SIZE,
    .align = V8M_BOOTSTRAP_ALIGN,
    .offset = 0,
};

static void v8m_bootstrap_oom(void)
{
	static const char msg[] = "v8malloc: bootstrap OOM\n";
	(void)write(STDERR_FILENO, msg, sizeof(msg) - 1U);
	abort();
}

void *v8m_bootstrap_alloc(size_t size)
{
	void *ptr = v8m_bump_alloc(&v8m_bootstrap_bump, size);
	if (ptr == NULL) {
		v8m_bootstrap_oom();
	}
	return ptr;
}

bool v8m_ptr_is_bootstrap(const void *ptr)
{
	return v8m_bump_owns(&v8m_bootstrap_bump, ptr);
}

size_t v8m_bootstrap_remaining(const void *ptr)
{
	return v8m_bump_remaining(&v8m_bootstrap_bump, ptr);
}
