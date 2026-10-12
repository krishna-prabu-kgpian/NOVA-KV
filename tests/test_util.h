// test_util.h -- minimal dependency-free test support.
//
// Checks are always evaluated (they do not depend on NDEBUG), every failure is
// reported with its location, and the process exit status is the number of
// failed checks clamped to 1, so CTest sees a failure in every build mode.
#pragma once

#include <cstdio>

namespace nova_test {

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

inline int& check_count() {
    static int checks = 0;
    return checks;
}

inline void record(bool ok, const char* expr, const char* file, int line) {
    ++check_count();
    if (!ok) {
        ++failure_count();
        std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, file, line);
    }
}

inline int finish(const char* test_name) {
    std::printf("%s: %d checks, %d failed\n", test_name, check_count(), failure_count());
    return failure_count() == 0 ? 0 : 1;
}

}  // namespace nova_test

#define NOVA_CHECK(cond) ::nova_test::record(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
