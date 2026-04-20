/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Version reporting. The numeric components come from the same macro
 * set the public header exposes, so consumers can rely on a runtime
 * v8m_version_*() call returning the same value as the compile-time
 * V8M_VERSION_* constants they were built against.
 */

#include "v8malloc/v8malloc.h"

const char *v8m_version(void)
{
	return V8M_VERSION_STRING;
}

int v8m_version_major(void)
{
	return V8M_VERSION_MAJOR;
}

int v8m_version_minor(void)
{
	return V8M_VERSION_MINOR;
}

int v8m_version_patch(void)
{
	return V8M_VERSION_PATCH;
}
