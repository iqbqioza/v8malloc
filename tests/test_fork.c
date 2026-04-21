/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fork-safety test. Verifies the pthread_atfork triple registered by
 * v8m_constructor leaves the allocator usable in both parent and
 * child after fork(). The test:
 *
 *   1. Allocates a few buffers in the parent across slab, buddy, and
 *      large size buckets, and writes through them.
 *   2. Spawns a worker thread that hammers malloc/free in a loop —
 *      the prefork handler must serialize against any in-flight pool
 *      operations rather than letting the child inherit a half-locked
 *      mutex.
 *   3. fork()s. The child:
 *        - Frees the inherited pre-fork allocations.
 *        - Performs its own fresh allocate/free cycle across all
 *          three backends.
 *        - Exits 0 on success, non-zero on any allocation failure.
 *   4. The parent waits for the child, verifies its exit status, and
 *      then continues its own allocate/free cycle to confirm it was
 *      not left in a deadlocked state by the fork.
 *   5. Joins the worker thread and frees the original buffers.
 *
 * A regression in the atfork handlers manifests as either the child
 * hanging on its first malloc (lock inherited locked from a parent
 * thread that didn't survive) or the parent hanging on its post-fork
 * allocation (postfork_parent didn't release).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* IWYU disagrees with the conventional POSIX include of
 * <sys/types.h> for pid_t; keep it explicit anyway since
 * <sys/wait.h> only re-exports it via implementation detail. */
#include <sys/types.h> /* IWYU pragma: keep */
#include <sys/wait.h>
#include <unistd.h>

enum {
	WORKER_ITERATIONS = 4096,
	CHILD_ITERATIONS = 256,
	PARENT_POSTFORK_ITERATIONS = 256,
};

static atomic_int g_worker_should_stop;

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_fork: %s\n", msg);
	return 1;
}

static void *worker_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < WORKER_ITERATIONS; i++) {
		if (atomic_load_explicit(&g_worker_should_stop,
					 memory_order_acquire) != 0) {
			break;
		}
		size_t size = 16U + ((size_t)i & 1023U);
		void *ptr = malloc(size);
		if (ptr == NULL) {
			/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
			return (void *)(uintptr_t)1;
		}
		(void)memset(ptr, i & 0xFF, size);
		free(ptr);
	}
	return NULL;
}

static int allocate_set(void **out, const size_t *sizes, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		void *ptr = malloc(sizes[i]);
		if (ptr == NULL) {
			/* Roll back the partial set so the early-return
			 * doesn't leak the pointers we already allocated. */
			for (size_t j = 0; j < i; j++) {
				free(out[j]);
			}
			return 1;
		}
		(void)memset(ptr, 0xA5, sizes[i]);
		out[i] = ptr;
	}
	return 0;
}

static void free_set(void **ptrs, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		free(ptrs[i]);
	}
}

/*
 * Body of the child process. Returns the exit status to use; on any
 * failure the message is written to stderr by `fail`.
 */
static int run_child(void **inherited, size_t count)
{
	free_set(inherited, count);

	void *fresh[6];
	static const size_t fresh_sizes[6] = {
	    24,
	    256,
	    4096,
	    (size_t)32 * 1024,
	    (size_t)200 * 1024,
	    (size_t)1 * 1024 * 1024,
	};
	if (allocate_set(fresh, fresh_sizes, 6) != 0) {
		return fail("child: allocate_set failed");
	}
	for (int i = 0; i < CHILD_ITERATIONS; i++) {
		size_t size = 32U + ((size_t)i & 511U);
		void *ptr = malloc(size);
		if (ptr == NULL) {
			free_set(fresh, 6);
			return fail("child: malloc loop returned NULL");
		}
		(void)memset(ptr, i & 0xFF, size);
		free(ptr);
	}
	free_set(fresh, 6);
	return 0;
}

int main(void)
{
	static const size_t sizes[] = {
	    32, 256, 1024, 4096, 65536, 524288,
	};
	size_t count = sizeof(sizes) / sizeof(sizes[0]);
	void *parent_buffers[6];

	if (allocate_set(parent_buffers, sizes, count) != 0) {
		return fail("parent: pre-fork allocate_set failed");
	}

	/* pthread_t and pid_t are defined deep in libc's header
	 * cluster; the linter's IWYU rule complains that <pthread.h> /
	 * <sys/types.h> aren't the "direct" providers depending on the
	 * glibc version, but this is the conventional way to obtain
	 * them. */
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t worker;
	if (pthread_create(&worker, NULL, worker_thread, NULL) != 0) {
		free_set(parent_buffers, count);
		return fail("pthread_create failed");
	}

	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pid_t pid = fork();
	if (pid < 0) {
		atomic_store_explicit(&g_worker_should_stop, 1,
				      memory_order_release);
		(void)pthread_join(worker, NULL);
		free_set(parent_buffers, count);
		return fail("fork failed");
	}

	if (pid == 0) {
		/* Child: only the calling thread survives. The worker we
		 * created in the parent is gone here. _exit() bypasses
		 * the at-exit handlers and is the right primitive in a
		 * forked child that didn't exec. */
		_exit(run_child(parent_buffers, count));
	}

	/* Parent path. */
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) {
		atomic_store_explicit(&g_worker_should_stop, 1,
				      memory_order_release);
		(void)pthread_join(worker, NULL);
		free_set(parent_buffers, count);
		return fail("waitpid failed");
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		atomic_store_explicit(&g_worker_should_stop, 1,
				      memory_order_release);
		(void)pthread_join(worker, NULL);
		free_set(parent_buffers, count);
		return fail("child exited with non-zero status");
	}

	/* Parent post-fork allocate/free cycle confirms the
	 * postfork_parent handler released the pool mutexes. */
	for (int i = 0; i < PARENT_POSTFORK_ITERATIONS; i++) {
		size_t size = 64U + ((size_t)i & 255U);
		void *ptr = malloc(size);
		if (ptr == NULL) {
			atomic_store_explicit(&g_worker_should_stop, 1,
					      memory_order_release);
			(void)pthread_join(worker, NULL);
			free_set(parent_buffers, count);
			return fail("parent: post-fork malloc returned NULL");
		}
		(void)memset(ptr, i & 0xFF, size);
		free(ptr);
	}

	atomic_store_explicit(&g_worker_should_stop, 1, memory_order_release);
	void *worker_status = NULL;
	(void)pthread_join(worker, &worker_status);
	if ((uintptr_t)worker_status != 0U) {
		free_set(parent_buffers, count);
		return fail("worker thread reported failure");
	}
	free_set(parent_buffers, count);
	return 0;
}
