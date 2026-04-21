# SPDX-License-Identifier: Apache-2.0

set(V8MALLOC_TEST_LIBRARY v8malloc_static)
if(NOT V8MALLOC_BUILD_STATIC)
	set(V8MALLOC_TEST_LIBRARY v8malloc_shared)
endif()

# TSan is experimental under v8malloc: its shadow-memory scheme
# clashes with our early-init mmaps from the library constructor
# ("FATAL: ThreadSanitizer: unexpected memory mapping"). The
# V8MALLOC_BUILD_TSAN option at the top-level CMakeLists turns on
# the sanitizer flags for the library build, but running the full
# test suite under TSan currently fails in our environment and is
# tracked as a known issue. UBSan is the supported sanitizer in v0.
set(V8MALLOC_TEST_PIE_FLAGS)

function(v8malloc_add_test name)
	add_executable(${name} ${name}.c)
	target_link_libraries(${name} PRIVATE ${V8MALLOC_TEST_LIBRARY})
	# Tests may reach internal headers (e.g. src/v8m_size_class.h)
	# to exercise machinery that is not part of the public ABI.
	target_include_directories(${name} PRIVATE
		${CMAKE_SOURCE_DIR}/src
	)
	# Match the library's PRIVATE _GNU_SOURCE so tests can reach
	# POSIX-extension prototypes (setenv, mmap helpers, ...) without
	# each .c file having to redeclare it.
	target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
	target_compile_options(${name} PRIVATE
		-Wall -Wextra -Wpedantic -Wshadow
		-Wstrict-prototypes -Wmissing-prototypes
		-Wold-style-definition
	)
	add_test(NAME ${name} COMMAND ${name})
endfunction()

v8malloc_add_test(test_version)
v8malloc_add_test(test_size_class)
v8malloc_add_test(test_page_meta)
v8malloc_add_test(test_bootstrap)
v8malloc_add_test(test_remote_free)
v8malloc_add_test(test_page_heap)
v8malloc_add_test(test_slab_tiny)
v8malloc_add_test(test_slab_small)
v8malloc_add_test(test_large)
v8malloc_add_test(test_buddy)
v8malloc_add_test(test_config)
v8malloc_add_test(test_slab_pool)
v8malloc_add_test(test_buddy_pool)
v8malloc_add_test(test_dispatch)
v8malloc_add_test(test_api)
v8malloc_add_test(test_alignment)
v8malloc_add_test(test_realloc)
v8malloc_add_test(test_calloc)
v8malloc_add_test(test_usable_size)
v8malloc_add_test(test_fork_stress)
v8malloc_add_test(test_soak)
v8malloc_add_test(test_libc_fallback)
v8malloc_add_test(test_fork)
v8malloc_add_test(test_numa)
v8malloc_add_test(test_threading)
v8malloc_add_test(test_thread_churn)
v8malloc_add_test(test_memory_pressure)

# libFuzzer driver. clang-only; the top-level CMakeLists guards
# the option with a fatal error on non-clang compilers, so we just
# add the target when the option is on. The fuzzer flag goes on
# both the compile and link line of the driver only — the rest of
# the test suite uses regular compilation.
if(V8MALLOC_BUILD_FUZZ)
	add_executable(fuzz_alloc fuzz_alloc.c)
	target_link_libraries(fuzz_alloc PRIVATE ${V8MALLOC_TEST_LIBRARY})
	target_compile_options(fuzz_alloc PRIVATE
		-Wall -Wextra -Wpedantic -Wshadow
		-fsanitize=fuzzer
	)
	target_link_options(fuzz_alloc PRIVATE -fsanitize=fuzzer)
endif()

# LD_PRELOAD interposition test. preload_target is a standalone
# program with no compile-time link to v8malloc; the test runner
# injects libv8malloc.so via LD_PRELOAD and the program verifies
# our public symbols are visible (dlsym of v8m_version) and that
# malloc/realloc/free have been transparently redirected.
if(V8MALLOC_BUILD_SHARED)
	add_executable(preload_target preload_target.c)
	target_link_libraries(preload_target PRIVATE dl)
	target_compile_options(preload_target PRIVATE
		-Wall -Wextra -Wpedantic -Wshadow
		-Wstrict-prototypes -Wmissing-prototypes
		-Wold-style-definition
	)
	add_test(NAME test_ld_preload COMMAND preload_target)
	set_tests_properties(test_ld_preload PROPERTIES
		ENVIRONMENT
		"LD_PRELOAD=$<TARGET_FILE:v8malloc_shared>")
endif()
