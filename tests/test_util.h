// test_util.h -- minimal dependency-free test support.
//
// Checks are always evaluated (they do not depend on NDEBUG), every failure is
// reported with its location, and the process exit status is the number of
// failed checks clamped to 1, so CTest sees a failure in every build mode.
#pragma once

#include <atomic>
#include <cstdio>

namespace nova_test {

// Atomic: several tests call NOVA_CHECK from more than one thread.
inline std::atomic<int>& failure_count() {
    static std::atomic<int> failures{0};
    return failures;
}

inline std::atomic<int>& check_count() {
    static std::atomic<int> checks{0};
    return checks;
}

inline void record(bool ok, const char* expr, const char* file, int line) {
    check_count().fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
        failure_count().fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, file, line);
    }
}

inline int finish(const char* test_name) {
    std::printf("%s: %d checks, %d failed\n", test_name, check_count().load(), failure_count().load());
    return failure_count().load() == 0 ? 0 : 1;
}

}  // namespace nova_test

#define NOVA_CHECK(cond) ::nova_test::record(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
