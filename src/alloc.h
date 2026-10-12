// alloc.h -- the single allocation entry point of the storage engine.
//
// Every index and arena allocation goes through alloc_bytes(), which reports
// failure by returning nullptr (never by throwing), so each caller has an
// explicit, testable failure path.
#pragma once

#include <cstddef>

namespace nova {

// Returns nullptr on failure. `align` must be a power of two.
void* alloc_bytes(std::size_t size, std::size_t align = alignof(std::max_align_t)) noexcept;
void free_bytes(void* p) noexcept;

#ifdef NOVA_KV_TEST_HOOKS
// Compiled only into the *_testhooks library; production variants contain none
// of this. Not thread-safe: for single-threaded failure-injection tests.
namespace test_hooks {
// Makes the n-th alloc_bytes() call from now (0 = the very next one) return
// nullptr, once. A negative n disarms.
void fail_nth_allocation(long n) noexcept;
// True if the armed failure has been delivered since the last arming.
bool allocation_failure_fired() noexcept;
// Number of alloc_bytes() calls since the last arming.
long allocations_since_armed() noexcept;
}  // namespace test_hooks
#endif

}  // namespace nova
