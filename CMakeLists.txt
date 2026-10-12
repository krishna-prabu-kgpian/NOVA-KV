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
set(NOVA_STORE_SOURCES
    src/kvstore.cpp
    src/hash_segment.cpp
    src/arena.cpp
    src/alloc.cpp
    src/inspect.cpp)
# Part B engine: compiled into the rcu variant only, so the Part A libraries
# stay exactly the Part A code.
set(NOVA_RCU_SOURCES
    src/versioned_store.cpp
    src/cow_segment.cpp
    src/pinned_arena.cpp)
set(NOVA_LOCK_VARIANTS single global_lock rwlock)
set(NOVA_VARIANTS ${NOVA_LOCK_VARIANTS} rcu)

foreach(variant IN LISTS NOVA_VARIANTS)
    string(TOUPPER ${variant} variant_upper)
    set(variant_sources ${NOVA_STORE_SOURCES})
    if(variant STREQUAL "rcu")
        list(APPEND variant_sources ${NOVA_RCU_SOURCES})
    endif()
    add_library(nova_kv_${variant} STATIC ${variant_sources})
    target_include_directories(nova_kv_${variant} PUBLIC include PRIVATE src)
    target_compile_definitions(nova_kv_${variant}
        PRIVATE NOVA_KV_SYNC_MODE=NOVA_KV_SYNC_${variant_upper})
    target_link_libraries(nova_kv_${variant} PUBLIC nova_build_flags PRIVATE nova_warnings)
endforeach()

# Test-only build of the store with allocation-failure injection compiled in
# (src/alloc.h). The production variants above contain no hook code.
add_library(nova_kv_single_testhooks STATIC ${NOVA_STORE_SOURCES})
target_include_directories(nova_kv_single_testhooks PUBLIC include PRIVATE src)
target_compile_definitions(nova_kv_single_testhooks
    PRIVATE NOVA_KV_SYNC_MODE=NOVA_KV_SYNC_SINGLE
    PUBLIC NOVA_KV_TEST_HOOKS)
target_link_libraries(nova_kv_single_testhooks PUBLIC nova_build_flags PRIVATE nova_warnings)

# The same for the Part B store: allocation-failure injection plus the pause
# points used by the deterministic interleaving tests (src/test_points.h).
add_library(nova_kv_rcu_testhooks STATIC ${NOVA_STORE_SOURCES} ${NOVA_RCU_SOURCES})
target_include_directories(nova_kv_rcu_testhooks PUBLIC include PRIVATE src)
target_compile_definitions(nova_kv_rcu_testhooks
    PRIVATE NOVA_KV_SYNC_MODE=NOVA_KV_SYNC_RCU
    PUBLIC NOVA_KV_TEST_HOOKS)
target_link_libraries(nova_kv_rcu_testhooks PUBLIC nova_build_flags PRIVATE nova_warnings)

# ---- Supplied reference workload (compiled unmodified, no -Werror) ----------
# It depends only on kvstore.h, so one object library serves every variant.
add_library(nova_workload OBJECT supplied/workload_gen.cpp)
target_include_directories(nova_workload PUBLIC supplied include)
target_link_libraries(nova_workload PUBLIC nova_build_flags)

# ---- Drivers (Part A: three lock policies; Part B: rcu) -----------------------
# driver source name -> store variant it is linked with
set(NOVA_DRIVER_single_thread single)
set(NOVA_DRIVER_global_lock global_lock)
set(NOVA_DRIVER_rwlock rwlock)
set(NOVA_DRIVER_rcu rcu)
set(NOVA_DRIVERS single_thread global_lock rwlock rcu)
foreach(driver IN LISTS NOVA_DRIVERS)
    add_executable(${driver} drivers/${driver}.cpp $<TARGET_OBJECTS:nova_workload>)
    target_include_directories(${driver} PRIVATE supplied src)
    target_link_libraries(${driver} PRIVATE nova_kv_${NOVA_DRIVER_${driver}} nova_warnings)
endforeach()

# ---- Benchmark harnesses (project-owned; the supplied generator is untouched) --
foreach(variant IN LISTS NOVA_VARIANTS)
    add_executable(concurrent_bench_${variant} benchmarks/concurrent_bench.cpp)
    target_include_directories(concurrent_bench_${variant} PRIVATE src benchmarks)
    target_link_libraries(concurrent_bench_${variant} PRIVATE nova_kv_${variant} nova_warnings)
endforeach()

add_executable(layout_bench benchmarks/layout_bench.cpp)
target_include_directories(layout_bench PRIVATE benchmarks)
target_link_libraries(layout_bench PRIVATE nova_build_flags nova_warnings)

# ---- Tests ------------------------------------------------------------------
enable_testing()

set(NOVA_TEST_ENV
    "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1"
    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
    "TSAN_OPTIONS=halt_on_error=1")

function(nova_add_test name)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${NOVA_TEST_ENV}" TIMEOUT 600)
endfunction()

# The public header is identical for every variant, so one build pins the API.
add_executable(api_conformance_test tests/api_conformance_test.cpp)
target_link_libraries(api_conformance_test PRIVATE nova_kv_single nova_warnings)
nova_add_test(api_conformance_test)

# Storage-engine unit tests (independent of the synchronization policy).
add_executable(hash_segment_test tests/hash_segment_test.cpp src/hash_segment.cpp src/alloc.cpp)
target_include_directories(hash_segment_test PRIVATE src)
target_link_libraries(hash_segment_test PRIVATE nova_build_flags nova_warnings)
nova_add_test(hash_segment_test)

add_executable(arena_test tests/arena_test.cpp src/arena.cpp src/alloc.cpp)
target_include_directories(arena_test PRIVATE src)
target_link_libraries(arena_test PRIVATE nova_build_flags nova_warnings)
nova_add_test(arena_test)

# Allocation-failure injection against the hook-enabled store build.
add_executable(alloc_failure_test tests/alloc_failure_test.cpp)
target_include_directories(alloc_failure_test PRIVATE src)
target_link_libraries(alloc_failure_test PRIVATE nova_kv_single_testhooks nova_warnings)
nova_add_test(alloc_failure_test)

# ---- Part B engine tests (hook-enabled build of the engine sources) -----------
set(NOVA_RCU_ENGINE_SOURCES ${NOVA_RCU_SOURCES} src/hash_segment.cpp src/alloc.cpp)
foreach(test pinned_arena_test cow_segment_test versioned_store_test
             versioned_alloc_failure_test versioned_interleaving_test)
    add_executable(${test} tests/${test}.cpp ${NOVA_RCU_ENGINE_SOURCES})
    target_include_directories(${test} PRIVATE src)
    target_compile_definitions(${test} PRIVATE NOVA_KV_TEST_HOOKS)
    target_link_libraries(${test} PRIVATE nova_build_flags nova_warnings)
    nova_add_test(${test})
endforeach()

foreach(variant IN LISTS NOVA_VARIANTS)
    # The Part A sanity test, against every variant.
    add_executable(kvstore_test_${variant} tests/kvstore_test.cpp)
    target_include_directories(kvstore_test_${variant} PRIVATE src)
    target_link_libraries(kvstore_test_${variant} PRIVATE nova_kv_${variant} nova_warnings)
    nova_add_test(kvstore_test_${variant})

    # Concurrent use is legal only for the locked variants.
    if(NOT variant STREQUAL "single")
        add_executable(concurrent_lock_test_${variant} tests/concurrent_lock_test.cpp)
        target_include_directories(concurrent_lock_test_${variant} PRIVATE src)
        target_link_libraries(concurrent_lock_test_${variant}
            PRIVATE nova_kv_${variant} nova_warnings)
        nova_add_test(concurrent_lock_test_${variant})
    endif()

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

# Each driver runs end to end with a short duration (2 readers, 1 second;
# single_thread pins itself to 1 reader). These are functional checks of the
# drivers, not measurements.
foreach(driver IN LISTS NOVA_DRIVERS)
    set(driver_args 2 1)
    if(driver STREQUAL "rcu")
        # B1 retains every replaced version: load 10,000 trades, not 100,000.
        set(driver_args 2 1 100 100)
    endif()
    add_test(NAME driver_${driver} COMMAND ${driver} ${driver_args})
    set_tests_properties(driver_${driver} PROPERTIES
        ENVIRONMENT "${NOVA_TEST_ENV}" TIMEOUT 600
        PASS_REGULAR_EXPRESSION "ops=[1-9]")
endforeach()

# Benchmark validation: each harness runs briefly and must pass its own validity
# checks (writer made successful updates inside the measured window, no read
# returned a foreign record, every expected key was found, the three layouts
# read identical data). Functional checks only; the numbers are not results.
add_executable(bench_histogram_test tests/bench_histogram_test.cpp)
target_include_directories(bench_histogram_test PRIVATE benchmarks)
target_link_libraries(bench_histogram_test PRIVATE nova_build_flags nova_warnings)
nova_add_test(bench_histogram_test)

add_test(NAME bench_validate_concurrent_global_lock
    COMMAND concurrent_bench_global_lock --readers 3 --duration 0.4 --warmup 0.1 --pin off)
add_test(NAME bench_validate_concurrent_rwlock
    COMMAND concurrent_bench_rwlock --readers 3 --duration 0.4 --warmup 0.1 --pin off)
add_test(NAME bench_validate_concurrent_rwlock_paced
    COMMAND concurrent_bench_rwlock --readers 2 --duration 0.4 --warmup 0.1 --pin off
            --writer-rate 5000)
add_test(NAME bench_validate_concurrent_reference_keys
    COMMAND concurrent_bench_global_lock --readers 2 --duration 0.3 --warmup 0.1 --pin off
            --keys reference)
add_test(NAME bench_validate_concurrent_single_readonly
    COMMAND concurrent_bench_single --readers 1 --duration 0.3 --warmup 0.1 --pin off --writer off)
add_test(NAME bench_validate_concurrent_single_inline_writer
    COMMAND concurrent_bench_single --readers 1 --duration 0.3 --warmup 0.1 --pin off
            --writer inline:10)
# Part B store: lock-free readers with an active writer (versions are retained,
# so the windows are short), and the single-thread mode with immediate release.
add_test(NAME bench_validate_concurrent_rcu
    COMMAND concurrent_bench_rcu --readers 3 --duration 0.4 --warmup 0.1 --pin off
            --m 100 --n 100)
add_test(NAME bench_validate_concurrent_rcu_paced
    COMMAND concurrent_bench_rcu --readers 2 --duration 0.4 --warmup 0.1 --pin off
            --m 100 --n 100 --writer-rate 5000)
add_test(NAME bench_validate_concurrent_rcu_inline_writer
    COMMAND concurrent_bench_rcu --readers 1 --duration 0.3 --warmup 0.1 --pin off
            --m 100 --n 100 --writer inline:10)
# The memory guard must stop the writer and invalidate the run.
add_test(NAME bench_rcu_retain_limit_invalidates_run
    COMMAND concurrent_bench_rcu --readers 1 --duration 0.5 --warmup 0.1 --pin off
            --m 100 --n 100 --retain-limit-mb 8)
set_tests_properties(bench_rcu_retain_limit_invalidates_run PROPERTIES WILL_FAIL TRUE TIMEOUT 120)
set_tests_properties(
    bench_validate_concurrent_rcu bench_validate_concurrent_rcu_paced
    bench_validate_concurrent_rcu_inline_writer
    PROPERTIES ENVIRONMENT "${NOVA_TEST_ENV}" TIMEOUT 600)

add_test(NAME bench_validate_layout
    COMMAND layout_bench --records 20000 --ops 200000 --reps 2)
set_tests_properties(
    bench_validate_concurrent_global_lock bench_validate_concurrent_rwlock
    bench_validate_concurrent_rwlock_paced bench_validate_concurrent_reference_keys
    bench_validate_concurrent_single_readonly bench_validate_concurrent_single_inline_writer
    bench_validate_layout
    PROPERTIES ENVIRONMENT "${NOVA_TEST_ENV}" TIMEOUT 600)

# The harness must REFUSE an unsynchronized store with a concurrent writer.
add_test(NAME bench_rejects_concurrent_single
    COMMAND concurrent_bench_single --readers 2 --duration 0.2 --pin off)
set_tests_properties(bench_rejects_concurrent_single PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
