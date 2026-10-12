cmake_minimum_required(VERSION 3.16)
project(nova_kv LANGUAGES CXX)

# ---- Language and build type -------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Performance builds: blueprint 9.3 mandates -O3 -g. NDEBUG keeps assert() out
# of the measured hot path; tests use always-on checks (tests/test_util.h).
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

option(NOVA_ENABLE_ASAN_UBSAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(NOVA_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(NOVA_WARNINGS_AS_ERRORS "Treat warnings in project sources as errors" ON)

if(NOVA_ENABLE_ASAN_UBSAN AND NOVA_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan cannot be combined; use separate build directories")
endif()

find_package(Threads REQUIRED)

# ---- Flags shared by every target (including the supplied workload) ---------
add_library(nova_build_flags INTERFACE)
# Frame pointers in every mode so perf call graphs / flame graphs are usable.
target_compile_options(nova_build_flags INTERFACE -fno-omit-frame-pointer)
target_link_libraries(nova_build_flags INTERFACE Threads::Threads)
if(NOVA_ENABLE_ASAN_UBSAN)
    set(NOVA_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(NOVA_ENABLE_TSAN)
    set(NOVA_SAN_FLAGS -fsanitize=thread)
endif()
if(NOVA_SAN_FLAGS)
    target_compile_options(nova_build_flags INTERFACE ${NOVA_SAN_FLAGS})
    target_link_options(nova_build_flags INTERFACE ${NOVA_SAN_FLAGS})
endif()

# ---- Warnings: project sources only, never the supplied reference file ------
add_library(nova_warnings INTERFACE)
target_compile_options(nova_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(NOVA_WARNINGS_AS_ERRORS)
    target_compile_options(nova_warnings INTERFACE -Werror)
endif()

# ---- Store library: one data structure, one library per sync policy ---------
# Only NOVA_KV_SYNC_MODE differs between the variants (src/sync_mode.h).
set(NOVA_STORE_SOURCES src/kvstore.cpp)
set(NOVA_VARIANTS single global_lock rwlock)

foreach(variant IN LISTS NOVA_VARIANTS)
    string(TOUPPER ${variant} variant_upper)
    add_library(nova_kv_${variant} STATIC ${NOVA_STORE_SOURCES})
    target_include_directories(nova_kv_${variant} PUBLIC include PRIVATE src)
    target_compile_definitions(nova_kv_${variant}
        PRIVATE NOVA_KV_SYNC_MODE=NOVA_KV_SYNC_${variant_upper})
    target_link_libraries(nova_kv_${variant} PUBLIC nova_build_flags PRIVATE nova_warnings)
endforeach()

# ---- Supplied reference workload (compiled unmodified, no -Werror) ----------
# It depends only on kvstore.h, so one object library serves every variant.
add_library(nova_workload OBJECT supplied/workload_gen.cpp)
target_include_directories(nova_workload PUBLIC supplied include)
target_link_libraries(nova_workload PUBLIC nova_build_flags)

# ---- Tests ------------------------------------------------------------------
enable_testing()

set(NOVA_TEST_ENV
    "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1"
    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
    "TSAN_OPTIONS=halt_on_error=1")

function(nova_add_test name)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${NOVA_TEST_ENV}" TIMEOUT 120)
endfunction()

# The public header is identical for every variant, so one build pins the API.
add_executable(api_conformance_test tests/api_conformance_test.cpp)
target_link_libraries(api_conformance_test PRIVATE nova_kv_single nova_warnings)
nova_add_test(api_conformance_test)

foreach(variant IN LISTS NOVA_VARIANTS)
    add_executable(smoke_test_${variant} tests/smoke_test.cpp)
    target_include_directories(smoke_test_${variant} PRIVATE src)
    target_compile_definitions(smoke_test_${variant}
        PRIVATE NOVA_EXPECTED_SYNC_MODE="${variant}")
    target_link_libraries(smoke_test_${variant} PRIVATE nova_kv_${variant} nova_warnings)
    nova_add_test(smoke_test_${variant})

    add_executable(reference_workload_link_test_${variant}
        tests/reference_workload_link_test.cpp $<TARGET_OBJECTS:nova_workload>)
    target_include_directories(reference_workload_link_test_${variant} PRIVATE supplied)
    target_link_libraries(reference_workload_link_test_${variant}
        PRIVATE nova_kv_${variant} nova_warnings)
    nova_add_test(reference_workload_link_test_${variant})
endforeach()
