# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

project(v8malloc
	VERSION 0.1.0
	DESCRIPTION "High-performance memory allocator targeting jemalloc/mimalloc/tcmalloc"
	HOMEPAGE_URL "https://github.com/iqbqioza/v8malloc"
	LANGUAGES C
)

# --- Platform & toolchain gates -----------------------------------------------

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR
		"v8malloc supports Linux only (got ${CMAKE_SYSTEM_NAME})")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
	if(CMAKE_C_COMPILER_VERSION VERSION_LESS 13)
		message(FATAL_ERROR
			"GCC >= 13 required (got ${CMAKE_C_COMPILER_VERSION})")
	endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
	if(CMAKE_C_COMPILER_VERSION VERSION_LESS 16)
		message(FATAL_ERROR
			"Clang >= 16 required (got ${CMAKE_C_COMPILER_VERSION})")
	endif()
else()
	message(FATAL_ERROR
		"Unsupported compiler ${CMAKE_C_COMPILER_ID}; use GCC or Clang")
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- Options ------------------------------------------------------------------

option(V8MALLOC_BUILD_SHARED "Build the shared library (libv8malloc.so)" ON)
option(V8MALLOC_BUILD_STATIC "Build the static library (libv8malloc.a)" ON)
option(V8MALLOC_BUILD_TESTS "Build unit tests" ON)
option(V8MALLOC_BUILD_BENCH "Build benchmark harness" OFF)

# Sanitizer build variants. AddressSanitizer is intentionally
# omitted: its malloc interceptor takes precedence at load time,
# which means ASan-instrumented binaries bypass v8malloc entirely
# (making the sanitizer run invalid for our purposes). UBSan and
# TSan both coexist with a custom allocator and cover distinct
# bug classes (undefined behaviour, data races).
option(V8MALLOC_BUILD_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(V8MALLOC_BUILD_TSAN "Build with ThreadSanitizer" OFF)

# Coverage instrumentation. Adds --coverage (i.e. -fprofile-arcs
# -ftest-coverage on GCC, equivalent on Clang) to compile and link
# flags so a follow-on `lcov` / `genhtml` run can produce the
# usual HTML report. Only one sanitizer or coverage variant
# should be enabled at once.
option(V8MALLOC_BUILD_COVERAGE "Instrument for gcov / lcov coverage" OFF)

# libFuzzer-based randomized allocator driver. Requires clang;
# adds `-fsanitize=fuzzer` to the fuzz target only and combines
# cleanly with V8MALLOC_BUILD_UBSAN (the typical configuration is
# both flags on at once so the fuzzer catches UB on misshapen
# alloc/free sequences).
option(V8MALLOC_BUILD_FUZZ "Build the libFuzzer driver (requires clang)" OFF)

if(NOT V8MALLOC_BUILD_SHARED AND NOT V8MALLOC_BUILD_STATIC)
	message(FATAL_ERROR
		"At least one of V8MALLOC_BUILD_SHARED / V8MALLOC_BUILD_STATIC must be ON")
endif()

if(V8MALLOC_BUILD_UBSAN AND V8MALLOC_BUILD_TSAN)
	message(FATAL_ERROR
		"V8MALLOC_BUILD_UBSAN and V8MALLOC_BUILD_TSAN are mutually exclusive")
endif()

set(V8MALLOC_SANITIZER_FLAGS)
if(V8MALLOC_BUILD_UBSAN)
	# -fno-sanitize-recover makes UB fatal so CI turns red on the
	# first violation instead of logging and continuing.
	list(APPEND V8MALLOC_SANITIZER_FLAGS
		-fsanitize=undefined
		-fno-sanitize-recover=undefined)
endif()
if(V8MALLOC_BUILD_TSAN)
	list(APPEND V8MALLOC_SANITIZER_FLAGS -fsanitize=thread)
endif()
if(V8MALLOC_SANITIZER_FLAGS)
	add_compile_options(${V8MALLOC_SANITIZER_FLAGS})
	add_link_options(${V8MALLOC_SANITIZER_FLAGS})
endif()
# When a sanitizer is on, libv8malloc.so legitimately carries
# unresolved `__ubsan_handle_*` (or `__tsan_*`) references that
# the executable's own sanitizer runtime resolves at load time.
# Clang's driver refuses to link the static runtime into a
# `-shared` build (the static archive's `.preinit_array` is
# illegal in a DSO), and `-static-libsan` / `-shared-libsan` are
# both no-ops on a -shared link line. The supported pattern is
# therefore: leave the references unresolved in the SO and rely
# on the loader to fill them when the LD_PRELOAD'd binary is
# itself sanitizer-instrumented (which is the only case where
# they actually fire). Test executables, which link the static
# library, get the runtime via clang's normal -fsanitize handling
# on their own non-`-shared` link line. The follow-on cycle that
# ships an instrumented LD_PRELOAD-ready SO would link
# libclang_rt.ubsan_standalone-<arch>.so via RPATH.
set(V8MALLOC_SHARED_LIBRARY_STRICT_LINK ON)
if(V8MALLOC_SANITIZER_FLAGS)
	set(V8MALLOC_SHARED_LIBRARY_STRICT_LINK OFF)
endif()

if(V8MALLOC_BUILD_COVERAGE)
	if(V8MALLOC_BUILD_UBSAN OR V8MALLOC_BUILD_TSAN)
		message(FATAL_ERROR
			"V8MALLOC_BUILD_COVERAGE is mutually exclusive with the sanitizers")
	endif()
	# `--coverage` is the portable flag spelling shared by GCC and
	# Clang; CMake propagates it through to both compile and link.
	add_compile_options(--coverage -O0 -g)
	add_link_options(--coverage)
endif()

if(V8MALLOC_BUILD_FUZZ)
	if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
		message(FATAL_ERROR
			"V8MALLOC_BUILD_FUZZ requires clang (libfuzzer is clang-only)")
	endif()
	if(V8MALLOC_BUILD_TSAN)
		message(FATAL_ERROR
			"V8MALLOC_BUILD_FUZZ is incompatible with V8MALLOC_BUILD_TSAN")
	endif()
endif()

# --- Per-architecture compile flags (platform-abstraction.md §5.2) ------------

set(V8MALLOC_ARCH_FLAGS)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
	set(V8MALLOC_ARCH_FLAGS -march=x86-64-v2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
	# AArch64 Tier 1. -moutline-atomics ships GCC-generated
	# stubs that dispatch each atomic op between LL/SC and LSE
	# (CAS / SWP / LDADD-family) at runtime via the
	# `__aarch64_have_lse_atomics` flag the runtime sets from
	# HWCAP_ATOMICS. GCC 10+ defaults to outline atomics; making
	# the flag explicit documents the choice and keeps the
	# behaviour pinned across toolchain upgrades. The runtime
	# probe `v8m_arch_has_lse()` reports which path the dispatch
	# selected on the current host. TPIDR_EL0-relative TLS is
	# emitted by GCC / Clang for `__thread` automatically.
	set(V8MALLOC_ARCH_FLAGS -moutline-atomics)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le|powerpc64le")
	set(V8MALLOC_ARCH_FLAGS -mcpu=power9)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "s390x")
	set(V8MALLOC_ARCH_FLAGS -march=z14)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
	# RISC-V 64. Tier 2. No fixed -march override here so the
	# build picks up whatever the cross toolchain was configured
	# with (typically rv64gc on Debian / Ubuntu cross images).
	# When the target advertises the Zbb bit-manipulation
	# extension (-march=rv64gc_zbb or richer), GCC >= 13 / Clang
	# >= 16 emit CLZ.D / CTZ.D / CPOP.D for __builtin_*; the
	# fallback when Zbb is absent is the documented software
	# emulation. Atomics work the same way: stdatomic emits
	# LR/SC sequences on baseline rv64gc and AMOCAS instructions
	# when the target advertises the Zacas extension. The
	# spec'd runtime hwprobe ISA query (numa.md / RISC-V
	# `__NR_riscv_hwprobe`) is documented as out-of-scope for
	# v0 — the codegen choice is already locked in at compile
	# time, so a runtime probe is purely diagnostic.
	set(V8MALLOC_ARCH_FLAGS)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "loongarch64")
	# LoongArch: no per-arch flag override needed. The toolchain
	# (GCC >= 13 / Clang >= 16, already gated above) emits DBAR
	# barriers via stdatomic memory_order, LL.D/SC.D atomics
	# through the C11 _Atomic primitives, CLZ.D / CTZ.D / CPOP.D
	# through __builtin_*, and tp-relative TLS through __thread.
	# Listed explicitly so a future cycle that needs to add a
	# loongarch-specific tuning flag has a place to drop it.
	set(V8MALLOC_ARCH_FLAGS)
endif()

# --- Sources ------------------------------------------------------------------

set(V8MALLOC_SOURCES
	src/v8m_anchor_reservation.c
	src/v8m_api.c
	src/v8m_arch.c
	src/v8m_bg_purge.c
	src/v8m_bootstrap.c
	src/v8m_buddy.c
	src/v8m_buddy_pool.c
	src/v8m_config.c
	src/v8m_core_cache.c
	src/v8m_dispatch.c
	src/v8m_huge_slab.c
	src/v8m_large.c
	src/v8m_libc_fallback.c
	src/v8m_numa.c
	src/v8m_numa_pool.c
	src/v8m_page.c
	src/v8m_page_heap.c
	src/v8m_pprof.c
	src/v8m_refill_controller.c
	src/v8m_signal_safe.c
	src/v8m_size_class.c
	src/v8m_slab_pool.c
	src/v8m_slab_small.c
	src/v8m_slab_tiny.c
	src/v8m_thread_cache.c
	src/v8m_version.c
)

set(V8MALLOC_PUBLIC_HEADERS
	include/v8malloc/v8malloc.h
)

set(V8MALLOC_VERSION_SCRIPT
	"${CMAKE_CURRENT_SOURCE_DIR}/src/v8malloc.map")

# --- Common target configuration ----------------------------------------------

function(v8malloc_configure_target tgt)
	target_include_directories(${tgt}
		PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
			$<INSTALL_INTERFACE:include>
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src
	)
	target_compile_options(${tgt} PRIVATE
		-Wall
		-Wextra
		-Wpedantic
		-Wshadow
		-Wstrict-prototypes
		-Wmissing-prototypes
		-Wold-style-definition
		-Wundef
		-Wcast-align
		-Wpointer-arith
		-Wwrite-strings
		${V8MALLOC_ARCH_FLAGS}
	)
	target_compile_definitions(${tgt} PRIVATE
		_GNU_SOURCE
		V8MALLOC_BUILDING
	)
	set_target_properties(${tgt} PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		C_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON
	)
	target_link_libraries(${tgt} PUBLIC pthread dl)
endfunction()

# --- Library targets ----------------------------------------------------------

set(V8MALLOC_INSTALL_TARGETS)

if(V8MALLOC_BUILD_SHARED)
	add_library(v8malloc_shared SHARED ${V8MALLOC_SOURCES})
	v8malloc_configure_target(v8malloc_shared)
	set_target_properties(v8malloc_shared PROPERTIES
		OUTPUT_NAME v8malloc
		VERSION ${PROJECT_VERSION}
		SOVERSION ${PROJECT_VERSION_MAJOR}
	)
	target_link_options(v8malloc_shared PRIVATE
		"LINKER:--version-script=${V8MALLOC_VERSION_SCRIPT}"
		"LINKER:-z,now"
		"LINKER:-z,relro"
	)
	if(V8MALLOC_SHARED_LIBRARY_STRICT_LINK)
		# Sanitizer builds carry unresolved __ubsan_handle_* /
		# __tsan_* references the host executable resolves at
		# LD_PRELOAD time; --no-undefined would reject them at
		# link time. Off in those builds.
		target_link_options(v8malloc_shared PRIVATE
			"LINKER:--no-undefined")
	endif()
	set_target_properties(v8malloc_shared PROPERTIES
		LINK_DEPENDS "${V8MALLOC_VERSION_SCRIPT}"
	)
	list(APPEND V8MALLOC_INSTALL_TARGETS v8malloc_shared)
endif()

if(V8MALLOC_BUILD_STATIC)
	add_library(v8malloc_static STATIC ${V8MALLOC_SOURCES})
	v8malloc_configure_target(v8malloc_static)
	set_target_properties(v8malloc_static PROPERTIES
		OUTPUT_NAME v8malloc
	)
	list(APPEND V8MALLOC_INSTALL_TARGETS v8malloc_static)
endif()

if(V8MALLOC_BUILD_SHARED)
	add_library(v8malloc::v8malloc ALIAS v8malloc_shared)
elseif(V8MALLOC_BUILD_STATIC)
	add_library(v8malloc::v8malloc ALIAS v8malloc_static)
endif()

# --- Tests --------------------------------------------------------------------

if(V8MALLOC_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

if(V8MALLOC_BUILD_BENCH)
	add_subdirectory(bench)
endif()

# --- Install ------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/v8malloc
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
	FILES_MATCHING PATTERN "*.h"
)

install(TARGETS ${V8MALLOC_INSTALL_TARGETS}
	EXPORT v8mallocTargets
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT v8mallocTargets
	FILE v8mallocTargets.cmake
	NAMESPACE v8malloc::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/v8malloc
)

configure_package_config_file(
	cmake/v8mallocConfig.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/v8mallocConfig.cmake"
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/v8malloc
)

write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/v8mallocConfigVersion.cmake"
	COMPATIBILITY SameMajorVersion
)

install(FILES
		"${CMAKE_CURRENT_BINARY_DIR}/v8mallocConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/v8mallocConfigVersion.cmake"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/v8malloc
)

set(V8MALLOC_PC_PREFIX     "${CMAKE_INSTALL_PREFIX}")
set(V8MALLOC_PC_LIBDIR     "${CMAKE_INSTALL_FULL_LIBDIR}")
set(V8MALLOC_PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
configure_file(cmake/v8malloc.pc.in
	"${CMAKE_CURRENT_BINARY_DIR}/v8malloc.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/v8malloc.pc"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

install(FILES man/v8malloc.3
	DESTINATION ${CMAKE_INSTALL_MANDIR}/man3)

# --- Tooling targets (format / tidy / cppcheck) -------------------------------

file(GLOB_RECURSE V8MALLOC_ALL_SOURCES
	LIST_DIRECTORIES false
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/bench/*.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/bench/*.h"
)

file(GLOB_RECURSE V8MALLOC_C_SOURCES
	LIST_DIRECTORIES false
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/bench/*.c"
)

# fuzz_alloc.c only compiles under -fsanitize=fuzzer, which is
# off in the dev/tidy build, so it's absent from
# compile_commands.json — clang-tidy reports it as "file not
# found". The format / format-check targets still cover it via
# V8MALLOC_ALL_SOURCES; only the tidy entry needs to skip.
list(REMOVE_ITEM V8MALLOC_C_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_alloc.c"
)

add_custom_target(format
	COMMAND clang-format -i ${V8MALLOC_ALL_SOURCES}
	COMMENT "Formatting sources"
	VERBATIM
)

add_custom_target(format-check
	COMMAND clang-format --dry-run -Werror ${V8MALLOC_ALL_SOURCES}
	COMMENT "Checking format"
	VERBATIM
)

add_custom_target(tidy
	COMMAND clang-tidy -p ${CMAKE_BINARY_DIR} ${V8MALLOC_C_SOURCES}
	COMMENT "Running clang-tidy"
	VERBATIM
)

add_custom_target(cppcheck
	COMMAND cppcheck
		--std=c23
		--platform=unix64
		--enable=warning,style,performance,portability,unusedFunction
		--quiet
		--inline-suppr
		--error-exitcode=1
		-D__linux__=1
		-D__x86_64__=1
		--suppress=missingIncludeSystem
		--suppress=unusedFunction
		--suppress=unmatchedSuppression
		--suppress=checkersReport
		--suppress=normalCheckLevelMaxBranches
		-I ${CMAKE_CURRENT_SOURCE_DIR}/include
		-I ${CMAKE_CURRENT_SOURCE_DIR}/src
		${V8MALLOC_C_SOURCES}
	COMMENT "Running cppcheck"
	VERBATIM
)
