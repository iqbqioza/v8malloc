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

static int check_uncompressed_dump(void)
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

/*
 * Drives the gzip-wrapped path: writes to a `.gz` suffix and
 * verifies the file starts with the gzip magic bytes (0x1F 0x8B)
 * and that the embedded inner DEFLATE-stored block carries the
 * pprof sample_type tag (0x0a) at the expected offset (10-byte
 * gzip header + 5-byte stored-block header = offset 15).
 */
static int check_gzip_dump(void)
{
	char path[] = "/tmp/v8m-pprof-test-XXXXXX.pb.gz";
	int tmp_fd = mkstemps(path, 6 /* ".pb.gz" suffix length */);
	if (tmp_fd < 0) {
		return fail("mkstemps gz failed");
	}
	(void)close(tmp_fd);

	if (v8m_pprof_dump_heap_to_path(path) != 0) {
		(void)unlink(path);
		return fail("gz dump returned non-zero");
	}

	int read_fd = open(path, O_RDONLY);
	if (read_fd < 0) {
		(void)unlink(path);
		return fail("gz open for read failed");
	}
	unsigned char buf[4096];
	ssize_t got = read(read_fd, buf, sizeof(buf));
	(void)close(read_fd);
	if (got < 24) {
		(void)unlink(path);
		return fail("gz read got fewer than 24 bytes");
	}

	if (buf[0] != 0x1FU || buf[1] != 0x8BU) {
		(void)unlink(path);
		return fail("gz magic bytes (0x1f 0x8b) missing");
	}
	if (buf[2] != 0x08U) {
		(void)unlink(path);
		return fail("gz CM byte is not 0x08 (deflate)");
	}
	/* Stored DEFLATE block header lands at byte 10. The very next
	 * byte (the BFINAL+BTYPE byte) should be 0x01 (final + stored). */
	if (buf[10] != 0x01U) {
		(void)unlink(path);
		return fail("gz stored-block header missing");
	}
	/* The first byte of the embedded protobuf payload sits at
	 * offset 15 (10-byte gzip header + 5-byte stored-block header). */
	if (buf[15] != 0x0aU) {
		(void)unlink(path);
		return fail("gz inner payload first byte is not 0x0a");
	}

	(void)unlink(path);
	return 0;
}

int main(void)
{
	int status = check_uncompressed_dump();
	if (status != 0) {
		return status;
	}
	return check_gzip_dump();
}
