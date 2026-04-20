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

if(NOT V8MALLOC_BUILD_SHARED AND NOT V8MALLOC_BUILD_STATIC)
	message(FATAL_ERROR
		"At least one of V8MALLOC_BUILD_SHARED / V8MALLOC_BUILD_STATIC must be ON")
endif()

# --- Per-architecture compile flags (platform-abstraction.md §5.2) ------------

set(V8MALLOC_ARCH_FLAGS)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
	set(V8MALLOC_ARCH_FLAGS -march=x86-64-v2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le|powerpc64le")
	set(V8MALLOC_ARCH_FLAGS -mcpu=power9)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "s390x")
	set(V8MALLOC_ARCH_FLAGS -march=z14)
endif()

# --- Sources ------------------------------------------------------------------

set(V8MALLOC_SOURCES
	src/v8m_page.c
	src/v8m_size_class.c
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
		"LINKER:--no-undefined"
		"LINKER:-z,now"
		"LINKER:-z,relro"
	)
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
