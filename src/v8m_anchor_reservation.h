/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Anchor reservation (huge-pages.md §6.2 — VMA minimization).
 * Reserves one large virtual-address range up front via
 * `mmap(PROT_NONE | MAP_NORESERVE)` and carves sub-regions out of
 * it via `mprotect(PROT_READ | PROT_WRITE)`. Each carve commits its
 * pages to the existing PROT_NONE mapping rather than minting a new
 * mmap; from the kernel's perspective the entire reservation is
 * still one VMA, so the kernel-side cost of mmap / fork / page-fault
 * scales with the anchor count rather than the per-allocation count.
 *
 * **Bump-only allocation**: this primitive does not maintain a
 * free-list of holes within the reservation. Each carve advances
 * the bump pointer; release() applies MADV_DONTNEED to drop the
 * physical pages but leaves the virtual slot dead until the
 * reservation is destroyed. The simpler semantics keep the carve
 * fast-path branch-free and the bookkeeping minimal — the
 * allocator's existing per-arena and per-region free paths handle
 * within-region reuse, and the anchor's whole-reservation lifetime
 * is what wins the VMA-count battle.
 *
 * **v0 status**: this is the standalone primitive. The page-heap
 * integration that consumes it on the alloc / free path is a
 * follow-on cycle — wiring requires routing decisions about which
 * size classes opt in (Huge is the largest VMA contributor; Large
 * and below carry the existing region-table cost), and those
 * decisions are simpler to land on top of a frozen primitive.
 */

#ifndef V8M_ANCHOR_RESERVATION_H
#define V8M_ANCHOR_RESERVATION_H

#include <pthread.h> /* IWYU pragma: keep — pthread_mutex_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Default reservation cap when the caller does not specify one
 * explicitly. 256 MiB is large enough to absorb a couple hundred
 * Huge allocations into a single VMA on x86_64 (2 MiB Huge × 128 =
 * 256 MiB) without committing any physical memory up front
 * (PROT_NONE + MAP_NORESERVE), and small enough that it does not
 * collide with mmap's address-space picker on the realistic 47-bit
 * user address space every Tier 1/2 arch ships with.
 */
#define V8M_ANCHOR_DEFAULT_CAP_BYTES ((size_t)256U * 1024U * 1024U)

/*
 * Reservation descriptor. Single instance is the typical pattern;
 * a future cycle that wants per-NUMA reservations will instance
 * one per node. Concurrency: the carve path takes the embedded
 * mutex; the predicate / accessor surface is lock-free where the
 * underlying field is atomic-equivalent (single 8-byte read).
 */
struct v8m_anchor_reservation {
	void *base; /* mmap return; reservation start */
	size_t cap; /* reservation size in bytes */
	size_t bump_offset; /* next-carve byte offset within base */
	uint64_t carve_calls; /* count of successful carves */
	uint64_t
	    carve_failures; /* count of carves rejected (full / mprotect) */
	uint64_t release_calls; /* count of release() calls */
	/* clang-tidy's IWYU rule prefers bits/pthreadtypes.h for
	 * pthread_mutex_t; pthread.h is the canonical provider. */
	pthread_mutex_t lock; /* NOLINT(misc-include-cleaner) */
};

/*
 * Initialize the reservation with `cap` bytes (rounded up to the OS
 * page size; pass 0 to use V8M_ANCHOR_DEFAULT_CAP_BYTES). Returns 0
 * on success or a negative errno-style code on failure
 * (`-ENOMEM` if mmap fails). On failure the descriptor is left
 * zeroed and `destroy()` is a safe no-op. Tolerates NULL.
 */
int v8m_anchor_reservation_init(struct v8m_anchor_reservation *res, size_t cap);

/*
 * Tear down the reservation: munmaps the entire range and zeroes
 * the descriptor. Safe to call on a never-initialized descriptor
 * (the `base == NULL` guard makes it a no-op). Tolerates NULL.
 */
void v8m_anchor_reservation_destroy(struct v8m_anchor_reservation *res);

/*
 * Carve `bytes` from the reservation, aligned to `alignment`
 * (must be a power of two ≥ 1; 0 is treated as 1). Returns the
 * carved address (already mprotect'd PROT_READ | PROT_WRITE) or
 * NULL on:
 *   - reservation full (the bump pointer + alignment + bytes
 *     exceeds cap),
 *   - the carve's mprotect call failed (rare; counts as a
 *     failure for stats purposes).
 * Increments `carve_calls` on success and `carve_failures` on
 * either failure mode.
 */
/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
void *v8m_anchor_reservation_carve(struct v8m_anchor_reservation *res,
				   size_t bytes, size_t alignment);
/* NOLINTEND(bugprone-easily-swappable-parameters) */

/*
 * Release `bytes` starting at `ptr`. Applies MADV_DONTNEED to drop
 * the physical pages back to the OS, then `mprotect(PROT_NONE)` so
 * the slot faults on access (catches use-after-free attempts). The
 * virtual slot is NOT reclaimable — the bump pointer never
 * regresses; this primitive is bump-only by design (see header
 * preamble). Returns true on success, false if `ptr / bytes` is
 * not a recognised carve from this reservation.
 */
bool v8m_anchor_reservation_release(struct v8m_anchor_reservation *res,
				    void *ptr, size_t bytes);

/*
 * Predicate: true iff `ptr` lies within the reservation's address
 * range. Lock-free: reads `base` and `cap` once each. Tolerates
 * NULL (returns false).
 */
bool v8m_anchor_reservation_owns(const struct v8m_anchor_reservation *res,
				 const void *ptr);

/*
 * Bytes of reservation still available for carve (cap minus
 * bump_offset). Tolerates NULL (returns 0). Useful for the
 * page-heap routing decision in the future integration cycle:
 * fall back to discrete mmap when the anchor's remaining capacity
 * cannot satisfy the request.
 */
size_t
v8m_anchor_reservation_remaining(const struct v8m_anchor_reservation *res);

#endif /* V8M_ANCHOR_RESERVATION_H */
