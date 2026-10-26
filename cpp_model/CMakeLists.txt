cmake_minimum_required(VERSION 3.17)
project(cache_sim VERSION 1.0.0 LANGUAGES CXX)

# ── C++17 standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ─────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Wshadow
        -Wconversion
        -Wdouble-promotion
    )
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

# ── Include directories ───────────────────────────────────────────────────────
include_directories(include)

# ── Source files ──────────────────────────────────────────────────────────────
set(SOURCES
    src/cache.cpp
    src/trace_parser.cpp
    src/cosim_checker.cpp
    src/main.cpp
)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(cache_sim ${SOURCES})

target_include_directories(cache_sim PRIVATE include)

# ── Optional: enable address sanitizer in Debug builds ────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(cache_sim PRIVATE -fsanitize=address,undefined)
    target_link_options(cache_sim PRIVATE -fsanitize=address,undefined)
endif()

# ── Install rule ──────────────────────────────────────────────────────────────
install(TARGETS cache_sim DESTINATION bin)

# ── Tests (optional, requires CTest) ─────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()

    # Simple self-test: run on sample trace and check exit code
    add_test(NAME sample_trace_test
        COMMAND cache_sim
            --trace ${CMAKE_SOURCE_DIR}/../traces/sample_trace.trace
            --sets 64 --ways 4 --line-size 64 --policy lru
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    add_test(NAME write_buffer_sync_test
        COMMAND cache_sim
            --trace ${CMAKE_SOURCE_DIR}/../traces/failing/write_buffer_sync.trace
            --sets 16 --ways 2 --line-size 32 --policy lru
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
