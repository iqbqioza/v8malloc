/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pprof Profile-message encoder. The wire format is documented at
 * https://protobuf.dev/programming-guides/encoding/ — every field is
 * a (varint tag, payload) pair where the tag packs (field_number <<
 * 3) | wire_type. Length-delimited submessages prefix their payload
 * with a varint length, which forces a two-pass strategy: emit each
 * inner message into a small stack scratch first, then write
 * (tag, length, bytes) into the outer buffer.
 *
 * We hold the entire emitted Profile in a single BSS scratch
 * (V8M_PPROF_BUF_BYTES) so the encoder never calls malloc — it must
 * be safe to invoke from the library destructor after the
 * dispatcher has been torn down. The buffer is sized for the v0
 * scope (per-size-class samples), where the upper bound is well
 * under a few KiB; the cap leaves head-room for the future
 * call-stack expansion.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "v8m_pprof.h"
#include "v8malloc/v8malloc.h"

/* Static BSS scratch sized for the v0 emit. 41 size classes plus
 * the Huge overflow row; per class the encoded footprint is on the
 * order of (1 Function ~10 B) + (1 Location ~10 B) + (1 Sample
 * ~16 B) + a string-table entry of "size_class_NN" (~14 B) ≈ 50 B.
 * 64 KiB leaves three orders of magnitude of head-room for the
 * future per-call-stack expansion. */
#define V8M_PPROF_BUF_BYTES (64U * 1024U)
static uint8_t g_pprof_buf[V8M_PPROF_BUF_BYTES];

/* --- low-level wire helpers -------------------------------------- */
/*
 * All writers return the new offset on success or SIZE_MAX on
 * buffer overflow. Callers propagate SIZE_MAX up to the top-level
 * encoder, which surfaces it as a -1 / EOVERFLOW return.
 */

static size_t pb_write_varint(uint8_t *buf, size_t cap, size_t off,
			      uint64_t value)
{
	while (value >= 0x80U) {
		if (off >= cap) {
			return SIZE_MAX;
		}
		buf[off++] = (uint8_t)((value & 0x7FU) | 0x80U);
		value >>= 7;
	}
	if (off >= cap) {
		return SIZE_MAX;
	}
	buf[off++] = (uint8_t)(value & 0x7FU);
	return off;
}

/* All wire-helper signatures group buffer / cap / off / field /
 * value parameters in a fixed order; the linter's swappable-args
 * heuristic flags the convertible-type adjacencies, but the order
 * is mechanical and consistent throughout the encoder. */
/* NOLINTBEGIN(bugprone-easily-swappable-parameters) */
static size_t pb_write_tag(uint8_t *buf, size_t cap, size_t off, uint32_t field,
			   uint32_t wire)
{
	return pb_write_varint(buf, cap, off,
			       ((uint64_t)field << 3U) | (uint64_t)wire);
}

static size_t pb_write_uint64_field(uint8_t *buf, size_t cap, size_t off,
				    uint32_t field, uint64_t value)
{
	off = pb_write_tag(buf, cap, off, field, 0U);
	if (off == SIZE_MAX) {
		return off;
	}
	return pb_write_varint(buf, cap, off, value);
}

static size_t pb_write_int64_field(uint8_t *buf, size_t cap, size_t off,
				   uint32_t field, int64_t value)
{
	return pb_write_uint64_field(buf, cap, off, field, (uint64_t)value);
}

static size_t pb_write_bytes_field(uint8_t *buf, size_t cap, size_t off,
				   uint32_t field, const uint8_t *data,
				   size_t len)
{
	off = pb_write_tag(buf, cap, off, field, 2U);
	if (off == SIZE_MAX) {
		return off;
	}
	off = pb_write_varint(buf, cap, off, (uint64_t)len);
	if (off == SIZE_MAX) {
		return off;
	}
	if (off + len > cap) {
		return SIZE_MAX;
	}
	(void)memcpy(buf + off, data, len);
	return off + len;
}

static size_t pb_write_string_field(uint8_t *buf, size_t cap, size_t off,
				    uint32_t field, const char *str)
{
	return pb_write_bytes_field(buf, cap, off, field, (const uint8_t *)str,
				    strlen(str));
}

/* --- pprof submessage emitters ----------------------------------- */

/*
 * ValueType { int64 type = 1; int64 unit = 2; }
 */
static size_t emit_value_type(uint8_t *out, size_t cap, size_t off,
			      uint32_t outer_field, int64_t type_idx,
			      int64_t unit_idx)
{
	uint8_t inner[32];
	size_t inner_off = 0;
	inner_off =
	    pb_write_int64_field(inner, sizeof(inner), inner_off, 1, type_idx);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	inner_off =
	    pb_write_int64_field(inner, sizeof(inner), inner_off, 2, unit_idx);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	return pb_write_bytes_field(out, cap, off, outer_field, inner,
				    inner_off);
}

/*
 * Function { uint64 id = 1; int64 name = 2; int64 system_name = 3;
 *            int64 filename = 4; int64 start_line = 5; }
 */
static size_t emit_function(uint8_t *out, size_t cap, size_t off,
			    uint64_t func_id, int64_t name_idx)
{
	uint8_t inner[32];
	size_t inner_off = 0;
	inner_off =
	    pb_write_uint64_field(inner, sizeof(inner), inner_off, 1, func_id);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	inner_off =
	    pb_write_int64_field(inner, sizeof(inner), inner_off, 2, name_idx);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	inner_off =
	    pb_write_int64_field(inner, sizeof(inner), inner_off, 3, name_idx);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	/* filename = 0 ("" in string table), start_line = 0 — both
	 * default values, can be omitted from the wire. */
	return pb_write_bytes_field(out, cap, off, 5U, inner, inner_off);
}

/*
 * Location { uint64 id = 1; uint64 mapping_id = 2; uint64 address = 3;
 *            repeated Line line = 4; }
 * Line { uint64 function_id = 1; int64 line = 2; }
 *
 * One Line per Location, pointing at the same function id. Address /
 * mapping_id stay zero — the v0 emit has no real call-stack PCs.
 */
static size_t emit_location(uint8_t *out, size_t cap, size_t off,
			    uint64_t loc_id, uint64_t function_id)
{
	/* Encode the inner Line first. */
	uint8_t line_buf[16];
	size_t line_off = 0;
	line_off = pb_write_uint64_field(line_buf, sizeof(line_buf), line_off,
					 1, function_id);
	if (line_off == SIZE_MAX) {
		return line_off;
	}

	uint8_t inner[64];
	size_t inner_off = 0;
	inner_off =
	    pb_write_uint64_field(inner, sizeof(inner), inner_off, 1, loc_id);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	/* `line_buf` is the inner Line submessage we just encoded; the
	 * linter's swap heuristic flags it because both args are
	 * uint8_t arrays of similar size. The order is correct. */
	/* NOLINTNEXTLINE(readability-suspicious-call-argument) */
	inner_off = pb_write_bytes_field(inner, sizeof(inner), inner_off, 4U,
					 line_buf, line_off);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	return pb_write_bytes_field(out, cap, off, 4U, inner, inner_off);
}

/*
 * Sample { repeated uint64 location_id = 1 [packed=true];
 *          repeated int64 value = 2 [packed=true]; }
 *
 * One location_id (the synthetic per-class location), two values
 * (alloc_objects count, alloc_space bytes) matching the two
 * sample_type entries we declare in the Profile header.
 */
static size_t emit_sample(uint8_t *out, size_t cap, size_t off,
			  uint64_t location_id, uint64_t alloc_count,
			  uint64_t alloc_bytes)
{
	uint8_t locs[16];
	size_t loc_off = pb_write_varint(locs, sizeof(locs), 0, location_id);
	if (loc_off == SIZE_MAX) {
		return loc_off;
	}

	uint8_t vals[32];
	size_t val_off = 0;
	val_off = pb_write_varint(vals, sizeof(vals), val_off, alloc_count);
	if (val_off == SIZE_MAX) {
		return val_off;
	}
	val_off = pb_write_varint(vals, sizeof(vals), val_off, alloc_bytes);
	if (val_off == SIZE_MAX) {
		return val_off;
	}

	uint8_t inner[64];
	size_t inner_off = 0;
	inner_off = pb_write_bytes_field(inner, sizeof(inner), inner_off, 1U,
					 locs, loc_off);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	inner_off = pb_write_bytes_field(inner, sizeof(inner), inner_off, 2U,
					 vals, val_off);
	if (inner_off == SIZE_MAX) {
		return inner_off;
	}
	return pb_write_bytes_field(out, cap, off, 2U, inner, inner_off);
}
/* NOLINTEND(bugprone-easily-swappable-parameters) */

/* --- top-level Profile emit -------------------------------------- */

/*
 * String table indices. Index 0 must be "" by spec; the rest are
 * fixed-position so the emit code can reference them by name
 * without threading the lookup state.
 */
enum {
	V8M_PPROF_STR_EMPTY = 0,
	V8M_PPROF_STR_ALLOC_OBJECTS = 1,
	V8M_PPROF_STR_COUNT = 2,
	V8M_PPROF_STR_ALLOC_SPACE = 3,
	V8M_PPROF_STR_BYTES = 4,
	V8M_PPROF_STR_FIRST_CLASS = 5
};

/* Format "size_class_NN" into `buf` (cap ≥ 16). Returns the
 * length. We avoid snprintf to keep the encoder malloc-free under
 * paranoid libc instrumentation that pulls allocations into
 * fprintf-style helpers. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static size_t format_class_name(char *buf, size_t cap, uint32_t cls)
{
	const char prefix[] = "size_class_";
	size_t plen = sizeof(prefix) - 1U;
	if (cap < plen + 4U) {
		return 0;
	}
	(void)memcpy(buf, prefix, plen);
	size_t off = plen;
	if (cls >= 100U) {
		buf[off++] = (char)('0' + (cls / 100U));
	}
	if (cls >= 10U) {
		buf[off++] = (char)('0' + ((cls / 10U) % 10U));
	}
	buf[off++] = (char)('0' + (cls % 10U));
	return off;
}

/* Per-section emitters. Each returns the updated offset on success
 * or SIZE_MAX on encoder overflow. Splitting the top-level dump
 * into per-section helpers keeps each function inside the linter's
 * cognitive-complexity budget. */

static size_t emit_sample_types(uint8_t *buf, size_t cap, size_t off)
{
	off = emit_value_type(buf, cap, off, 1U, V8M_PPROF_STR_ALLOC_OBJECTS,
			      V8M_PPROF_STR_COUNT);
	if (off == SIZE_MAX) {
		return off;
	}
	return emit_value_type(buf, cap, off, 1U, V8M_PPROF_STR_ALLOC_SPACE,
			       V8M_PPROF_STR_BYTES);
}

static size_t emit_samples(uint8_t *buf, size_t cap, size_t off,
			   const struct v8m_size_class_histogram *hist,
			   const bool *alive, bool huge_alive)
{
	for (uint32_t cls = 0; cls < V8M_PUBLIC_NUM_SIZE_CLASSES; cls++) {
		if (!alive[cls]) {
			continue;
		}
		off = emit_sample(buf, cap, off, (uint64_t)cls + 1U,
				  hist->request_count[cls],
				  hist->request_bytes[cls]);
		if (off == SIZE_MAX) {
			return off;
		}
	}
	if (huge_alive) {
		off = emit_sample(
		    buf, cap, off, (uint64_t)V8M_PUBLIC_NUM_SIZE_CLASSES + 1U,
		    hist->huge_request_count, hist->huge_request_bytes);
	}
	return off;
}

static size_t emit_locations(uint8_t *buf, size_t cap, size_t off,
			     const bool *alive, bool huge_alive)
{
	for (uint32_t cls = 0; cls < V8M_PUBLIC_NUM_SIZE_CLASSES; cls++) {
		if (!alive[cls]) {
			continue;
		}
		off = emit_location(buf, cap, off, (uint64_t)cls + 1U,
				    (uint64_t)cls + 1U);
		if (off == SIZE_MAX) {
			return off;
		}
	}
	if (huge_alive) {
		uint64_t huge_id = (uint64_t)V8M_PUBLIC_NUM_SIZE_CLASSES + 1U;
		off = emit_location(buf, cap, off, huge_id, huge_id);
	}
	return off;
}

static size_t emit_functions(uint8_t *buf, size_t cap, size_t off,
			     const bool *alive, bool huge_alive)
{
	for (uint32_t cls = 0; cls < V8M_PUBLIC_NUM_SIZE_CLASSES; cls++) {
		if (!alive[cls]) {
			continue;
		}
		off = emit_function(buf, cap, off, (uint64_t)cls + 1U,
				    (int64_t)V8M_PPROF_STR_FIRST_CLASS +
					(int64_t)cls);
		if (off == SIZE_MAX) {
			return off;
		}
	}
	if (huge_alive) {
		uint64_t huge_id = (uint64_t)V8M_PUBLIC_NUM_SIZE_CLASSES + 1U;
		off = emit_function(buf, cap, off, huge_id,
				    (int64_t)V8M_PPROF_STR_FIRST_CLASS +
					(int64_t)V8M_PUBLIC_NUM_SIZE_CLASSES);
	}
	return off;
}

static size_t emit_string_table(uint8_t *buf, size_t cap, size_t off)
{
	const char *fixed[] = {"", "alloc_objects", "count", "alloc_space",
			       "bytes"};
	for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
		off = pb_write_string_field(buf, cap, off, 6U, fixed[i]);
		if (off == SIZE_MAX) {
			return off;
		}
	}
	for (uint32_t cls = 0; cls < V8M_PUBLIC_NUM_SIZE_CLASSES; cls++) {
		char name[16];
		size_t nlen = format_class_name(name, sizeof(name), cls);
		off = pb_write_bytes_field(buf, cap, off, 6U,
					   (const uint8_t *)name, nlen);
		if (off == SIZE_MAX) {
			return off;
		}
	}
	return pb_write_string_field(buf, cap, off, 6U, "huge");
}

/* Stream `len` bytes from `buf` to `out_fd`, tolerating short writes
 * and EINTR. Returns 0 on success or -1 on terminal write failure. */
static int stream_to_fd(int out_fd, const uint8_t *buf, size_t len)
{
	size_t written = 0;
	while (written < len) {
		ssize_t got = write(out_fd, buf + written, len - written);
		if (got > 0) {
			written += (size_t)got;
			continue;
		}
		if (got < 0 && errno == EINTR) {
			continue;
		}
		return -1;
	}
	return 0;
}

/* cppcheck-suppress staticFunction
 * — exposed via src/v8m_pprof.h and consumed by tests/test_pprof.c
 *   in a different translation unit. */
ssize_t v8m_pprof_dump_heap(int out_fd)
{
	struct v8m_size_class_histogram hist = {0};
	v8m_get_size_class_histogram(&hist);

	uint8_t *buf = g_pprof_buf;
	const size_t cap = sizeof(g_pprof_buf);
	size_t off = 0;

	bool alive[V8M_PUBLIC_NUM_SIZE_CLASSES];
	for (uint32_t cls = 0; cls < V8M_PUBLIC_NUM_SIZE_CLASSES; cls++) {
		alive[cls] = hist.request_count[cls] > 0U;
	}
	bool huge_alive = hist.huge_request_count > 0U;

	off = emit_sample_types(buf, cap, off);
	if (off == SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	off = emit_samples(buf, cap, off, &hist, alive, huge_alive);
	if (off == SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	off = emit_locations(buf, cap, off, alive, huge_alive);
	if (off == SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	off = emit_functions(buf, cap, off, alive, huge_alive);
	if (off == SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	off = emit_string_table(buf, cap, off);
	if (off == SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}

	if (stream_to_fd(out_fd, buf, off) != 0) {
		return -1;
	}
	return (ssize_t)off;
}

int v8m_pprof_dump_heap_to_path(const char *path)
{
	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
	int out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out_fd < 0) {
		return -1;
	}
	ssize_t got = v8m_pprof_dump_heap(out_fd);
	int saved = errno;
	if (close(out_fd) != 0 && got >= 0) {
		return -1;
	}
	if (got < 0) {
		errno = saved;
		return -1;
	}
	return 0;
}
