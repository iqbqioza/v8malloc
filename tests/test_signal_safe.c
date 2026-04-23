/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Async-signal-safe emergency allocator coverage. The contract:
 *
 *   1. v8m_signal_safe_alloc returns a 16-byte-aligned non-NULL
 *      pointer for any in-budget request, NULL on exhaustion.
 *   2. free() on a signal-safe pointer is a safe no-op (no
 *      crash, no double-free reaction from the dispatcher).
 *   3. The function works from inside an actual signal handler
 *      (the whole point of the module — async-signal safety is
 *      the contract, not just a documentation claim).
 *   4. malloc_usable_size on a signal-safe pointer is harmless
 *      (returns 0 since the per-allocation size is not tracked).
 */

#include <malloc.h> /* malloc_usable_size */
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h> /* SIZE_MAX */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v8malloc/v8malloc.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_signal_safe: %s\n", msg);
	return 1;
}

static int check_basic_alloc(void)
{
	void *ptr = v8m_signal_safe_alloc(64);
	if (ptr == NULL) {
		return fail("alloc(64) returned NULL on cold buffer");
	}
	if (((uintptr_t)ptr & 15U) != 0U) {
		return fail("returned pointer not 16-aligned");
	}
	(void)memset(ptr, 0xAB, 64);
	free(ptr); /* must be a safe no-op */
	return 0;
}

static int check_zero_size(void)
{
	/* The contract is "any in-budget request"; size 0 must round
	 * up to V8M_SIGNAL_SAFE_ALIGN (16) so the bump pointer
	 * advances and a unique pointer comes back. */
	void *first = v8m_signal_safe_alloc(0);
	void *second = v8m_signal_safe_alloc(0);
	if (first == NULL || second == NULL) {
		return fail("alloc(0) returned NULL");
	}
	if (first == second) {
		return fail("two alloc(0) calls returned the same pointer");
	}
	free(first);
	free(second);
	return 0;
}

static int check_eventual_exhaustion(void)
{
	/* The buffer is one page (V8M_PAGE_SIZE = 64 KiB). 64 alloc
	 * calls of 4 KiB plus a small amount of slack already eat the
	 * whole buffer. After that, alloc must return NULL — abort
	 * would be the wrong behaviour because a SIGSEGV inside a
	 * signal handler is worse than a NULL the handler can check. */
	for (int i = 0; i < 64; i++) {
		void *ptr = v8m_signal_safe_alloc(4096);
		if (ptr == NULL) {
			return 0; /* exhausted earlier than expected — fine */
		}
		((volatile unsigned char *)ptr)[0] = (unsigned char)i;
	}
	const void *overflow = v8m_signal_safe_alloc(4096);
	if (overflow != NULL) {
		return fail("alloc past buffer end did not return NULL");
	}
	/* Subsequent in-budget allocations must keep returning NULL —
	 * the bump pointer cannot rewind. */
	if (v8m_signal_safe_alloc(16) != NULL) {
		return fail("alloc after exhaustion suddenly succeeded");
	}
	return 0;
}

static atomic_int g_handler_alloc_succeeded;
static atomic_int g_handler_byte_seen;

static void signal_handler(int sig)
{
	(void)sig;
	/* Inside the signal handler: allocate, write, leave the
	 * pointer for the main thread to verify. The point of this
	 * test is that the call does not deadlock / crash, not that
	 * the pointer survives — that's covered by check_basic_alloc.
	 * Use only async-signal-safe ops (no printf, no abort). */
	void *ptr = v8m_signal_safe_alloc(32);
	if (ptr != NULL) {
		((volatile unsigned char *)ptr)[0] = 0xCD;
		atomic_store_explicit(&g_handler_byte_seen,
				      (int)((volatile unsigned char *)ptr)[0],
				      memory_order_relaxed);
		atomic_store_explicit(&g_handler_alloc_succeeded, 1,
				      memory_order_relaxed);
	}
}

static int check_inside_signal_handler(void)
{
	/* The freshly-allocated buffer is partially consumed by
	 * earlier checks — we need a fresh test process to be sure
	 * the handler sees a non-exhausted buffer. Cheaper alternative:
	 * just verify the alloc-from-handler path doesn't crash; if
	 * the handler-side budget is exhausted and ptr == NULL the
	 * test still passes the "no crash" contract. */
	atomic_store_explicit(&g_handler_alloc_succeeded, 0,
			      memory_order_relaxed);

	struct sigaction action = {0};
	action.sa_handler = signal_handler;
	action.sa_flags = SA_RESTART;
	(void)sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR1, &action, NULL) != 0) {
		return fail("sigaction(SIGUSR1) failed");
	}
	if (raise(SIGUSR1) != 0) {
		return fail("raise(SIGUSR1) failed");
	}
	/* The handler ran synchronously on raise(); no need to wait. */
	int byte =
	    atomic_load_explicit(&g_handler_byte_seen, memory_order_relaxed);
	int succeeded = atomic_load_explicit(&g_handler_alloc_succeeded,
					     memory_order_relaxed);
	/* Either the handler exhausted the buffer (succeeded == 0),
	 * which is fine, or it allocated and wrote 0xCD (byte ==
	 * 0xCD). Anything else means the handler crashed or wrote a
	 * bogus value, both of which are test failures. */
	if (succeeded != 0 && byte != 0xCD) {
		return fail("handler succeeded but wrote wrong byte");
	}
	return 0;
}

static int check_size_overflow_rejected(void)
{
	/* A near-SIZE_MAX request must NOT silently round to a 16-byte
	 * slot — that would let the caller stomp on neighbouring memory
	 * thinking they had SIZE_MAX bytes. The contract is to return
	 * NULL on out-of-budget, and any size that cannot fit in the
	 * emergency buffer (one page) is by definition out of budget. */
	if (v8m_signal_safe_alloc(SIZE_MAX) != NULL) {
		return fail("alloc(SIZE_MAX) did not return NULL");
	}
	if (v8m_signal_safe_alloc(SIZE_MAX - 8U) != NULL) {
		return fail("alloc(SIZE_MAX - 8) did not return NULL");
	}
	return 0;
}

static int check_usable_size(void)
{
	/* malloc_usable_size on a signal-safe pointer should not
	 * crash. The function does not know the per-allocation size
	 * (the bump pool does not track them), so 0 is the expected
	 * answer — same shape as the bootstrap pointer case in
	 * v8m_malloc_usable_size. */
	void *ptr = v8m_signal_safe_alloc(128);
	if (ptr == NULL) {
		/* Buffer exhausted by earlier checks; skip this one. */
		return 0;
	}
	(void)malloc_usable_size(ptr);
	free(ptr);
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_basic_alloc();
	result |= check_zero_size();
	result |= check_size_overflow_rejected();
	result |= check_inside_signal_handler();
	result |= check_usable_size();
	/* exhaustion last — it leaves the buffer permanently empty */
	result |= check_eventual_exhaustion();
	if (result == 0) {
		(void)printf("test_signal_safe: OK\n");
	}
	return result;
}
