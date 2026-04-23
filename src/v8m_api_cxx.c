/* SPDX-License-Identifier: Apache-2.0 */
/*
 * C++ operator new / delete (non-throwing variants), Itanium C++ ABI
 * mangled names. Split out of v8m_api.c so that the C-ABI surface and
 * the C++ ABI surface can evolve independently — and so the
 * sizable NOLINT block does not pollute the malloc/realloc fast
 * paths.
 *
 * The throwing forms (`_Znwm` operator new(size_t) and friends)
 * stay in libstdc++ because we can't construct the std::bad_alloc
 * instance from C; libstdc++'s shipped implementations call malloc
 * internally, so they pick up v8malloc transparently through the
 * LD_PRELOAD chain or static link.
 *
 * Naming notes:
 *   _Zdl  → operator delete
 *   _Zda  → operator delete[]
 *   _Znw  → operator new
 *   _Zna  → operator new[]
 *   Pv    → (void *)
 *   m     → size_t
 *   St11align_val_t → std::align_val_t (size_t-typed enum class)
 *   RKSt9nothrow_t  → const std::nothrow_t&
 *
 * `std::nothrow_t` is an empty struct passed by const-reference;
 * in the C ABI it shows up as a `const void *` parameter we
 * receive but never deref. `std::align_val_t` is a `size_t`-typed
 * scoped enum, indistinguishable from `size_t` at the C ABI
 * level.
 */

#include <stddef.h>

#include "v8malloc/v8malloc.h"

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 *             readability-identifier-naming,bugprone-easily-swappable-parameters)
 */

/* Forward declarations satisfy -Wmissing-prototypes for the
 * mangled C++ symbols below. They have no callers in C — the
 * mangled names exist only so a C++ program's call sites resolve
 * directly to v8malloc — but the warning fires on any visible
 * non-static definition without a prior prototype. */
V8M_EXPORT void _ZdlPv(void *ptr);
V8M_EXPORT void _ZdaPv(void *ptr);
V8M_EXPORT void _ZdlPvm(void *ptr, size_t size);
V8M_EXPORT void _ZdaPvm(void *ptr, size_t size);
V8M_EXPORT void _ZdlPvSt11align_val_t(void *ptr, size_t alignment);
V8M_EXPORT void _ZdaPvSt11align_val_t(void *ptr, size_t alignment);
V8M_EXPORT void _ZdlPvmSt11align_val_t(void *ptr, size_t size,
				       size_t alignment);
V8M_EXPORT void _ZdaPvmSt11align_val_t(void *ptr, size_t size,
				       size_t alignment);
V8M_EXPORT void *_ZnwmRKSt9nothrow_t(size_t size, const void *nothrow);
V8M_EXPORT void *_ZnamRKSt9nothrow_t(size_t size, const void *nothrow);
V8M_EXPORT void _ZdlPvRKSt9nothrow_t(void *ptr, const void *nothrow);
V8M_EXPORT void _ZdaPvRKSt9nothrow_t(void *ptr, const void *nothrow);
V8M_EXPORT void *_ZnwmSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow);
V8M_EXPORT void *_ZnamSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow);
V8M_EXPORT void _ZdlPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow);
V8M_EXPORT void _ZdaPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow);

/* operator delete(void*) and operator delete[](void*). Defined as
 * thin wrappers (not __attribute__((alias))) because alias must
 * point to a same-TU definition and v8m_free lives in v8m_api.c. */
V8M_EXPORT void _ZdlPv(void *ptr)
{
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPv(void *ptr)
{
	v8m_free(ptr);
}

/* Sized delete (C++14): the size hint comes from the caller and
 * we ignore it — v8m_free already recovers the size from meta. */
V8M_EXPORT void _ZdlPvm(void *ptr, size_t size)
{
	(void)size;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvm(void *ptr, size_t size)
{
	(void)size;
	v8m_free(ptr);
}

/* Aligned delete (C++17): the alignment hint is similarly
 * recoverable — we routed through v8m_aligned_alloc which placed
 * the pointer at the right offset, so v8m_free is enough. */
V8M_EXPORT void _ZdlPvSt11align_val_t(void *ptr, size_t alignment)
{
	(void)alignment;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvSt11align_val_t(void *ptr, size_t alignment)
{
	(void)alignment;
	v8m_free(ptr);
}

/* Sized + aligned delete (C++17). */
V8M_EXPORT void _ZdlPvmSt11align_val_t(void *ptr, size_t size, size_t alignment)
{
	(void)size;
	(void)alignment;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvmSt11align_val_t(void *ptr, size_t size, size_t alignment)
{
	(void)size;
	(void)alignment;
	v8m_free(ptr);
}

/* Nothrow new / delete: nothrow_t is an empty class, so the const
 * reference shows up as an unused `const void *`. v8m_malloc /
 * v8m_free already match the no-throw, return-NULL-on-failure
 * contract. */
V8M_EXPORT void *_ZnwmRKSt9nothrow_t(size_t size, const void *nothrow)
{
	(void)nothrow;
	return v8m_malloc(size);
}
V8M_EXPORT void *_ZnamRKSt9nothrow_t(size_t size, const void *nothrow)
{
	(void)nothrow;
	return v8m_malloc(size);
}
V8M_EXPORT void _ZdlPvRKSt9nothrow_t(void *ptr, const void *nothrow)
{
	(void)nothrow;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvRKSt9nothrow_t(void *ptr, const void *nothrow)
{
	(void)nothrow;
	v8m_free(ptr);
}

/* Nothrow aligned new (C++17). The alignment is the second
 * positional argument and the standard guarantees alignment is a
 * power of two ≥ alignof(std::max_align_t). */
V8M_EXPORT void *_ZnwmSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow)
{
	(void)nothrow;
	return v8m_aligned_alloc(alignment, size);
}
V8M_EXPORT void *_ZnamSt11align_val_tRKSt9nothrow_t(size_t size,
						    size_t alignment,
						    const void *nothrow)
{
	(void)nothrow;
	return v8m_aligned_alloc(alignment, size);
}

/* Aligned + nothrow delete (C++17). */
V8M_EXPORT void _ZdlPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow)
{
	(void)alignment;
	(void)nothrow;
	v8m_free(ptr);
}
V8M_EXPORT void _ZdaPvSt11align_val_tRKSt9nothrow_t(void *ptr, size_t alignment,
						    const void *nothrow)
{
	(void)alignment;
	(void)nothrow;
	v8m_free(ptr);
}

/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,
 *           readability-identifier-naming,bugprone-easily-swappable-parameters)
 */
