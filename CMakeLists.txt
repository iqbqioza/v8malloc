cmake_minimum_required(VERSION 3.25)
project(c23-dev C)

file(GLOB_RECURSE SOURCES src/*.c)
file(GLOB_RECURSE HEADERS src/*.h)
set(ALL_SOURCES ${SOURCES} ${HEADERS})

add_executable(main ${SOURCES})

add_custom_target(format
    COMMAND clang-format -i ${ALL_SOURCES}
    COMMENT "Formatting sources"
    VERBATIM
)

add_custom_target(format-check
    COMMAND clang-format --dry-run -Werror ${ALL_SOURCES}
    COMMENT "Checking format"
    VERBATIM
)

add_custom_target(tidy
    COMMAND clang-tidy -p ${CMAKE_BINARY_DIR} ${SOURCES}
    COMMENT "Running clang-tidy"
    VERBATIM
)

add_custom_target(cppcheck
    COMMAND cppcheck --std=c23 --enable=all --quiet --suppress=missingIncludeSystem ${SOURCES}
    COMMENT "Running cppcheck"
    VERBATIM
)
