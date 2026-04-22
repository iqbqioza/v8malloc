/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Double-free detection under V8M_DEBUG (api.md §6.2). The free
 * path consults a 4096-entry ring buffer of recently-freed pointers
 * when V8M_OPT_DEBUG is non-zero; a hit aborts the process with a
 * diagnostic. We can't `assert(abort)` directly without killing the
 * test binary, so we fork: the child performs the double-free and
 * is expected to die with SIGABRT; the parent verifies the exit
 * status and continues.
 *
 * Two scenarios:
 *   1. Double-free WITH V8M_DEBUG set → child must abort
 *   2. Double-free WITHOUT V8M_DEBUG → child is allowed to do
 *      whatever (no contract); we only assert that DEBUG-on
 *      catches it.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h> /* IWYU pragma: keep — pid_t */
#include <sys/wait.h>
#include <unistd.h>

#include "v8malloc/v8malloc.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_double_free: %s\n", msg);
	return 1;
}

static int run_double_free_in_child(void)
{
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pid_t pid = fork();
	if (pid < 0) {
		return fail("fork failed");
	}
	if (pid == 0) {
		/* Child: trigger double-free. Allocate a slab-sized
		 * pointer (small enough to avoid mmap teardown that
		 * might recover the slot before the second free). */
		void *ptr = malloc(64);
		if (ptr == NULL) {
			_exit(2); /* unable to even allocate */
		}
		free(ptr);
		/* The first free recorded ptr in the ring; the second
		 * call should hit the ring entry and abort. _exit(0)
		 * only runs if the abort failed to fire — that's a
		 * test failure. The clang static analyzer / cppcheck
		 * correctly flag the double-free; that's the entire
		 * point of the test, so suppress here. */
		/* cppcheck-suppress doubleFree */
		/* NOLINTNEXTLINE(clang-analyzer-unix.Malloc) */
		free(ptr);
		_exit(0);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) != pid) {
		return fail("waitpid failed");
	}
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
		return fail("child did not abort on double-free");
	}
	return 0;
}

int main(void)
{
	/* Enable DEBUG before the fork. The detector reads the
	 * config on every free, so this is enough. */
	if (v8m_set_option(V8M_OPT_DEBUG, 1) != 0) {
		return fail("v8m_set_option(DEBUG, 1) failed");
	}
	int result = 0;
	result |= run_double_free_in_child();
	if (result == 0) {
		(void)printf("test_double_free: OK\n");
	}
	return result;
}
