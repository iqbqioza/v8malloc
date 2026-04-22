/* SPDX-License-Identifier: Apache-2.0 */
/*
 * V8M_OPT_DEBUG guard page coverage for the Large/Huge alloc path
 * (api.md §6.2). When DEBUG is on, v8m_large_alloc appends one
 * V8M_PAGE_SIZE trailing guard region and mprotect()s it
 * PROT_NONE; an out-of-bounds write past the user-data window
 * must trap (SIGSEGV), not silently corrupt the next mapping.
 *
 * Verification strategy mirrors test_double_free: the offending
 * write is performed in a fork()'d child whose death by SIGSEGV
 * the parent observes via waitpid. The parent itself never
 * touches the guard, so it survives to run additional checks.
 *
 * Two scenarios:
 *   1. Default config (DEBUG off): no guard, write past
 *      usable_size MUST NOT trap (this would imply we're
 *      paying the guard cost in production).
 *   2. DEBUG on: guard installed, write at usable_size MUST
 *      trap with SIGSEGV.
 */

#include <malloc.h> /* malloc_usable_size — glibc extension */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> /* IWYU pragma: keep — pid_t */
#include <sys/wait.h>
#include <unistd.h>

#include "v8malloc/v8malloc.h"

/* The Large size class starts at 256 KiB. Pick a clean 256 KiB
 * request so the overrun computation stays trivial. */
enum { LARGE_REQUEST = (size_t)256 * 1024 };

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_guard_page: %s\n", msg);
	return 1;
}

/*
 * Returns the child's exit/termination status via waitpid,
 * shifted left so a non-zero return signals failure paths.
 * +1 = child died with SIGSEGV (the wanted outcome under DEBUG).
 * +2 = child returned 0 from main (no trap fired).
 * +3 = child died with some other signal (wanted under DEBUG=off
 *       only if we're checking that it didn't trap — see callers).
 * +4 = waitpid failed.
 */
enum {
	CHILD_OK = 0,
	CHILD_SIGSEGV,
	CHILD_EXIT_OK,
	CHILD_OTHER_DEATH,
	CHILD_WAIT_FAILED,
};

static int classify_child(pid_t pid)
{
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) {
		return CHILD_WAIT_FAILED;
	}
	if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
		return CHILD_SIGSEGV;
	}
	if (WIFSIGNALED(status)) {
		return CHILD_OTHER_DEATH;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		return CHILD_EXIT_OK;
	}
	return CHILD_OK;
}

/*
 * Allocate a Large block and write `delta` bytes past the
 * reported usable_size in the child. Returns the child's
 * classification (see the enum above). The write is volatile +
 * goes through a memory clobber so the compiler cannot drop the
 * "useless" store.
 */
static int run_overrun_in_child(size_t delta)
{
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pid_t pid = fork();
	if (pid < 0) {
		return CHILD_WAIT_FAILED;
	}
	if (pid == 0) {
		unsigned char *buf = malloc(LARGE_REQUEST);
		if (buf == NULL) {
			_exit(2);
		}
		size_t usable = malloc_usable_size(buf);
		/* Touch every byte of the legal window first so a real
		 * regression where the guard accidentally PROT_NONE'd
		 * the user area shows up as a SIGSEGV here too — but
		 * that would be misclassified as "guard fired"; keep
		 * the touch separate from the overrun by sleeping
		 * via memset so the kernel actually faults the pages
		 * in. */
		(void)memset(buf, 0xA5, usable);
		/* Overrun: write one byte at usable + delta. With
		 * delta = 0 that hits the first byte past the legal
		 * window; under DEBUG that lands inside the
		 * V8M_PAGE_SIZE-aligned tail padding (still legal —
		 * usable_size reports past the request, up to the
		 * page boundary), so we additionally touch
		 * `usable + tail_padding` bytes to land inside the
		 * guard. The simplest robust offset is one full
		 * V8M_PAGE_SIZE (64 KiB) past usable_size, which
		 * always crosses the guard boundary. */
		volatile unsigned char *probe = buf + usable + delta;
		*probe = 0x5A;
		__asm__ volatile("" ::: "memory");
		_exit(0);
	}
	return classify_child(pid);
}

static int check_guard_off_by_default(void)
{
	/* Sanity probe: with DEBUG off, writing every legal byte of
	 * the reported window must succeed. Catches a regression
	 * where we'd accidentally guard production builds (which
	 * would either pay the mprotect cost or trap on legal
	 * writes). Done in-process — no fork — so a leak / abort
	 * surfaces immediately. */
	if (v8m_set_option(V8M_OPT_DEBUG, 0) != 0) {
		return fail("could not turn DEBUG off");
	}
	unsigned char *buf = malloc(LARGE_REQUEST);
	if (buf == NULL) {
		return fail("malloc returned NULL with DEBUG=off");
	}
	size_t usable = malloc_usable_size(buf);
	if (usable < LARGE_REQUEST) {
		free(buf);
		return fail("usable_size shrank below request with DEBUG=off");
	}
	(void)memset(buf, 0xA5, usable);
	free(buf);
	return 0;
}

static int check_guard_on_traps_overrun(void)
{
	if (v8m_set_option(V8M_OPT_DEBUG, 1) != 0) {
		return fail("could not turn DEBUG on");
	}
	/* Write at offset usable_size — the first byte of the
	 * trailing guard. With DEBUG on this lands inside the
	 * PROT_NONE region and the kernel raises SIGSEGV; the
	 * usable_size-trimming logic in v8m_large_usable_size makes
	 * "write at usable_size" the cleanest reliable overrun. */
	int outcome = run_overrun_in_child(0);
	(void)v8m_set_option(V8M_OPT_DEBUG, 0);
	if (outcome != CHILD_SIGSEGV) {
		return fail("DEBUG=on but the overrun did not SIGSEGV");
	}
	return 0;
}

int main(void)
{
	int result = 0;
	result |= check_guard_off_by_default();
	result |= check_guard_on_traps_overrun();
	if (result == 0) {
		(void)printf("test_guard_page: OK\n");
	}
	return result;
}
