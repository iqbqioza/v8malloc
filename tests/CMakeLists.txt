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
