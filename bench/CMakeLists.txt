# SPDX-License-Identifier: Apache-2.0

# Benchmark targets. Built only when V8MALLOC_BUILD_BENCH=ON. Each
# benchmark links against libv8malloc the same way a real consumer
# would, so the perf numbers reflect what users actually see.
#
# Run a single benchmark:
#   cmake -S . -B build/bench -DV8MALLOC_BUILD_BENCH=ON \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/bench --target mb_01_throughput
#   ./build/bench/bench/mb_01_throughput
#
# Or via LD_PRELOAD against any program (mb_01_throughput is just
# the in-tree comparison vehicle):
#   LD_PRELOAD=./build/bench/libv8malloc.so ./other_program

set(V8MALLOC_BENCH_LIBRARY v8malloc_static)
if(NOT V8MALLOC_BUILD_STATIC)
	set(V8MALLOC_BENCH_LIBRARY v8malloc_shared)
endif()

function(v8malloc_add_bench name)
	add_executable(${name} ${name}.c)
	target_link_libraries(${name} PRIVATE ${V8MALLOC_BENCH_LIBRARY})
	# Benchmarks reach internal headers for V8M_PAGE_SIZE etc.
	target_include_directories(${name} PRIVATE
		${CMAKE_SOURCE_DIR}/src
	)
	target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
	target_compile_options(${name} PRIVATE
		-Wall -Wextra -Wpedantic -Wshadow
		-Wstrict-prototypes -Wmissing-prototypes
		-Wold-style-definition
		-O2
	)
endfunction()

v8malloc_add_bench(mb_01_throughput)
v8malloc_add_bench(mb_02_scalability)
target_link_libraries(mb_02_scalability PRIVATE pthread)
v8malloc_add_bench(mb_06_large_latency)
