/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Verify the runtime version reporting matches the compile-time
 * version macros and the documented "MAJOR.MINOR.PATCH" format.
 */

#include <stdio.h>
#include <string.h>

#include "v8malloc/v8malloc.h"

enum {
	/* Weights of the V8M_VERSION integer encoding. Pulled into named
	 * constants so the test is not flagged by readability-magic-numbers.
	 */
	VERSION_WEIGHT_MAJOR = 10000,
	VERSION_WEIGHT_MINOR = 100,
};

static int fail(const char *msg)
{
	(void)fprintf(stderr, "test_version: %s\n", msg);
	return 1;
}

int main(void)
{
	const char *ver = v8m_version();

	if (ver == NULL) {
		return fail("v8m_version() returned NULL");
	}

	if (strcmp(ver, V8M_VERSION_STRING) != 0) {
		return fail("runtime version string disagrees with header");
	}

	if (v8m_version_major() != V8M_VERSION_MAJOR) {
		return fail("major mismatch");
	}
	if (v8m_version_minor() != V8M_VERSION_MINOR) {
		return fail("minor mismatch");
	}
	if (v8m_version_patch() != V8M_VERSION_PATCH) {
		return fail("patch mismatch");
	}

	int expected = (V8M_VERSION_MAJOR * VERSION_WEIGHT_MAJOR) +
		       (V8M_VERSION_MINOR * VERSION_WEIGHT_MINOR) +
		       V8M_VERSION_PATCH;
	if (V8M_VERSION != expected) {
		return fail("V8M_VERSION computation drifted");
	}

	return 0;
}
