/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Heap-profile dump in pprof's protobuf-encoded Profile message
 * format (https://github.com/google/pprof/blob/main/proto/profile.proto).
 * Resolution of TODO.md open question #5: V8M_PROFILE=1 dumps a
 * pprof-loadable .pb file at process exit so the existing
 * `pprof` / `go tool pprof` tooling can ingest v8malloc's
 * per-size-class allocation footprint without a custom viewer.
 *
 * v0 scope: per-size-class samples — one sample per class with
 * non-zero alloc count, carrying (alloc_objects, alloc_space)
 * value pairs sourced from the per-thread request histogram
 * aggregated by `v8m_get_size_class_histogram`. Each sample
 * references one synthetic `[size_class_N]` Function so the pprof
 * viewer's flame graph shows the per-class breakdown. Real
 * call-stack capture is a future cycle that needs per-allocation
 * PC unwinding.
 *
 * The emitted file is uncompressed protobuf (.pb). The pprof
 * tool reads both .pb and .pb.gz; gzipping is left to a follow-up
 * once we link libz (avoiding the dependency on the malloc
 * fast path).
 */

#ifndef V8M_PPROF_H
#define V8M_PPROF_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Encode the live heap snapshot as a pprof Profile message and
 * write the raw protobuf bytes to `fd`. Returns the number of
 * bytes written on success, or -1 on encoding overflow / write
 * failure (errno set). Safe to call from a destructor: no malloc
 * on the path, the encoder uses a static BSS scratch buffer so
 * the dispatcher's torn-down state cannot affect it.
 */
ssize_t v8m_pprof_dump_heap(int out_fd);

/*
 * Same as `v8m_pprof_dump_heap` but wraps the encoded bytes in a
 * gzip stream (RFC 1952). Uses STORED DEFLATE blocks (BTYPE=00),
 * so the output is gzip-format-compliant but not actually
 * compressed — the trade-off avoids pulling libz onto the
 * malloc-free destructor path. The gzip wrapper exists for tools
 * that look at the file extension or magic bytes (pprof reads
 * both .pb and .pb.gz transparently). Returns the number of
 * bytes written, or -1 on encoder overflow / write failure.
 */
ssize_t v8m_pprof_dump_heap_gz(int out_fd);

/*
 * Open `path` (O_WRONLY | O_CREAT | O_TRUNC, mode 0600), dump the
 * heap profile, close the fd. When `path` ends in `.gz`, routes
 * through `v8m_pprof_dump_heap_gz`; otherwise emits the raw
 * uncompressed protobuf via `v8m_pprof_dump_heap`. Returns 0 on
 * success or -1 on open / dump / close failure (errno preserved
 * from the failing call).
 */
int v8m_pprof_dump_heap_to_path(const char *path);

#endif /* V8M_PPROF_H */
