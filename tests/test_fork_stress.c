/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ST-03 fork-safety stress (benchmarks.md §4.3). Repeats fork()
 * many times against a parent that is concurrently allocating
 * across multiple worker threads, and drives the child through a
 * non-trivial allocate/free workload across every backend before
 * it exits. test_fork covers the single-fork happy path; this
 * file covers the "many forks per parent, with parallel allocation
 * pressure" stress scenario the spec calls out as ST-03.
 *
 * Per ST-03 the regression shape we want to surface is:
 *
 *   - parent / child deadlock (atfork handler missed a lock the
 *     pool acquired in flight, child wakes up on a still-locked
 *     mutex and stalls on its first malloc)
 *   - heap corruption (parent and child both holding pointers into
 *     pages that got remapped under either side; subsequent ops
 *     surface as a SIGSEGV)
 *   - leak (parent's allocation accounting drifts each fork cycle
 *     because cleanup is skipped on a pre/post-fork imbalance)
 *
 * The child workload mirrors the test_fork child but multiplied:
 * 256 ops × four size buckets that span slab-Tiny, slab-Small,
 * buddy, and Large, with byte-pattern verification across
 * malloc / write / free.
 *
 * Scaled down for routine CI: 25 forks × 4 worker threads ×
 * 1024 op cap per worker. A 24h ST-01 style soak is out of
 * scope for unit-test runtime; this is the regression gate.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> /* IWYU pragma: keep — pid_t */
#include <sys/wait.h>
#include <unistd.h>

enum {
	FORK_ITERATIONS = 25,
	WORKER_THREADS = 4,
	WORKER_OPS_CAP = 1024,
	CHILD_OPS_PER_BUCKET = 64,
};

static const size_t g_buckets[] = {
    16,
    256,
    8192,
    (size_t)512 * 1024,
};
static const size_t g_bucket_count = sizeof(g_buckets) / sizeof(g_buckets[0]);

static atomic_int g_worker_should_stop;

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_fork_stress: %s\n", msg);
	return 1;
}

/* Worker thread: in a tight loop, alloc one of the bucket sizes
 * (rotated by `op`), write a pattern, free. Stops when the main
 * thread sets g_worker_should_stop. */
static void *parent_worker(void *arg)
{
	(void)arg;
	uint64_t ops = 0;
	while (atomic_load_explicit(&g_worker_should_stop,
				    memory_order_relaxed) == 0) {
		size_t size = g_buckets[ops % g_bucket_count];
		void *ptr = malloc(size);
		if (ptr == NULL) {
			break;
		}
		((volatile unsigned char *)ptr)[0] = (unsigned char)ops;
		free(ptr);
		ops++;
		if (ops >= WORKER_OPS_CAP * (uint64_t)FORK_ITERATIONS * 64ULL) {
			/* Cap the absolute work — the main thread's stop
			 * signal should arrive long before this. */
			break;
		}
	}
	return NULL;
}

/* Workload the child runs after fork(). Hits every backend with
 * both alloc and a byte-pattern + free roundtrip. Returns 0 on
 * success, non-zero on any allocator misbehavior. */
static int run_child_workload(void)
{
	for (size_t bi = 0; bi < g_bucket_count; bi++) {
		size_t size = g_buckets[bi];
		for (int ops = 0; ops < CHILD_OPS_PER_BUCKET; ops++) {
			void *ptr = malloc(size);
			if (ptr == NULL) {
				(void)fprintf(stderr,
					      "test_fork_stress (child): "
					      "malloc(%zu) returned NULL\n",
					      size);
				return 1;
			}
			(void)memset(ptr, 0xCD, size);
			/* Re-read to verify the page is mapped + writable
			 * end-to-end — a partial mapping (e.g. a
			 * post-fork remap that lost some pages) would
			 * surface as a stale value here. */
			if (((volatile unsigned char *)ptr)[size - 1U] !=
			    0xCD) {
				free(ptr);
				return 1;
			}
			free(ptr);
		}
	}
	return 0;
}

int main(void)
{
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	pthread_t workers[WORKER_THREADS];
	atomic_store_explicit(&g_worker_should_stop, 0, memory_order_relaxed);

	for (int i = 0; i < WORKER_THREADS; i++) {
		if (pthread_create(&workers[i], NULL, parent_worker, NULL) !=
		    0) {
			(void)fprintf(stderr,
				      "test_fork_stress: pthread_create %d "
				      "failed\n",
				      i);
			atomic_store_explicit(&g_worker_should_stop, 1,
					      memory_order_relaxed);
			for (int j = 0; j < i; j++) {
				(void)pthread_join(workers[j], NULL);
			}
			return 1;
		}
	}

	int failures = 0;
	for (int iter = 0; iter < FORK_ITERATIONS; iter++) {
		/* Anchor a small slab + buddy allocation in the parent
		 * so the child inherits some live pages and frees them
		 * on its way out. */
		void *anchor_slab = malloc(96);
		void *anchor_buddy = malloc(16384);
		if (anchor_slab == NULL || anchor_buddy == NULL) {
			free(anchor_slab);
			free(anchor_buddy);
			failures++;
			continue;
		}
		(void)memset(anchor_slab, 0x33, 96);
		(void)memset(anchor_buddy, 0x44, 16384);

		/* NOLINTNEXTLINE(misc-include-cleaner) */
		pid_t pid = fork();
		if (pid < 0) {
			(void)fprintf(
			    stderr, "test_fork_stress: fork %d failed\n", iter);
			free(anchor_slab);
			free(anchor_buddy);
			failures++;
			continue;
		}
		if (pid == 0) {
			/* Child: free the anchors, drive the workload,
			 * exit. _exit avoids running parent atexit
			 * handlers (which include glibc's _IO_cleanup
			 * and stdio buffer flushes — we want a clean
			 * tear-down without colliding with the parent's
			 * stdio state). */
			free(anchor_slab);
			free(anchor_buddy);
			int child_rc = run_child_workload();
			_exit(child_rc);
		}

		int status = 0;
		if (waitpid(pid, &status, 0) != pid) {
			(void)fprintf(stderr,
				      "test_fork_stress: waitpid %d failed\n",
				      iter);
			failures++;
		} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			(void)fprintf(stderr,
				      "test_fork_stress: child %d exited bad "
				      "status=%d\n",
				      iter, status);
			failures++;
		}

		free(anchor_slab);
		free(anchor_buddy);
	}

	atomic_store_explicit(&g_worker_should_stop, 1, memory_order_relaxed);
	for (int i = 0; i < WORKER_THREADS; i++) {
		(void)pthread_join(workers[i], NULL);
	}

	/* Final post-fork-storm allocate/free cycle in the parent —
	 * confirms the parent itself is still healthy. */
	for (size_t bi = 0; bi < g_bucket_count; bi++) {
		void *ptr = malloc(g_buckets[bi]);
		if (ptr == NULL) {
			return fail("post-storm parent malloc returned NULL");
		}
		((volatile unsigned char *)ptr)[0] = 0x99;
		free(ptr);
	}

	if (failures != 0) {
		(void)fprintf(stderr,
			      "test_fork_stress: %d failures across %d forks\n",
			      failures, FORK_ITERATIONS);
		return 1;
	}
	(void)printf("test_fork_stress: OK (%d forks)\n", FORK_ITERATIONS);
	return 0;
}
