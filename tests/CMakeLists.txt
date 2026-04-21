# SPDX-License-Identifier: Apache-2.0

set(V8MALLOC_TEST_LIBRARY v8malloc_static)
if(NOT V8MALLOC_BUILD_STATIC)
	set(V8MALLOC_TEST_LIBRARY v8malloc_shared)
endif()

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
v8malloc_add_test(test_libc_fallback)
v8malloc_add_test(test_fork)
v8malloc_add_test(test_numa)
v8malloc_add_test(test_threading)
