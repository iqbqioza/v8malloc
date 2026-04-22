/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pprof emit smoke test. Drives v8m_pprof_dump_heap_to_path against
 * a tmpfile and verifies:
 *   - the dump returns success
 *   - the file is non-empty
 *   - the bytes start with the protobuf tag for sample_type
 *     (field 1, wire type 2 → tag byte 0x0a) so we don't silently
 *     emit garbage that no pprof tool would parse
 *   - the string_table contains "alloc_objects" verbatim (no
 *     gzip / no compression, plain protobuf — pprof reads both)
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "v8m_pprof.h"

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_pprof: %s\n", msg);
	return 1;
}

int main(void)
{
	char path[] = "/tmp/v8m-pprof-test-XXXXXX.pb";
	int tmp_fd = mkstemps(path, 3 /* ".pb" suffix length */);
	if (tmp_fd < 0) {
		return fail("mkstemps failed");
	}
	(void)close(tmp_fd);

	if (v8m_pprof_dump_heap_to_path(path) != 0) {
		(void)unlink(path);
		return fail("dump returned non-zero");
	}

	struct stat info;
	if (stat(path, &info) != 0) {
		(void)unlink(path);
		return fail("stat failed");
	}
	if (info.st_size <= 0) {
		(void)unlink(path);
		return fail("dump file is empty");
	}

	int read_fd = open(path, O_RDONLY);
	if (read_fd < 0) {
		(void)unlink(path);
		return fail("open for read failed");
	}
	unsigned char buf[4096];
	ssize_t got = read(read_fd, buf, sizeof(buf));
	(void)close(read_fd);
	if (got < 16) {
		(void)unlink(path);
		return fail("read got fewer than 16 bytes");
	}

	/* The first byte MUST be the wire tag for `sample_type`
	 * (field 1, wire type 2 = length-delimited). */
	if (buf[0] != 0x0a) {
		(void)unlink(path);
		return fail("first byte is not the sample_type tag (0x0a)");
	}

	bool found_alloc_objects = false;
	for (ssize_t i = 0; i + 13 <= got; i++) {
		if (memcmp(&buf[i], "alloc_objects", 13) == 0) {
			found_alloc_objects = true;
			break;
		}
	}
	if (!found_alloc_objects) {
		(void)unlink(path);
		return fail("string_table missing 'alloc_objects'");
	}

	(void)unlink(path);
	return 0;
}
