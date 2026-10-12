#include "alloc.h"

#include <cstdlib>

namespace nova {

#ifdef NOVA_KV_TEST_HOOKS
namespace {
long g_fail_at = -1;
long g_calls = 0;
bool g_fired = false;
}  // namespace

namespace test_hooks {
void fail_nth_allocation(long n) noexcept {
    g_fail_at = n;
    g_calls = 0;
    g_fired = false;
}
bool allocation_failure_fired() noexcept { return g_fired; }
long allocations_since_armed() noexcept { return g_calls; }
}  // namespace test_hooks
#endif

void* alloc_bytes(std::size_t size, std::size_t align) noexcept {
#ifdef NOVA_KV_TEST_HOOKS
    const long call = g_calls++;
    if (g_fail_at >= 0 && call == g_fail_at) {
        g_fired = true;
        return nullptr;
    }
#endif
    if (size == 0) size = 1;
    if (align <= alignof(std::max_align_t)) return std::malloc(size);
    // aligned_alloc requires the size to be a multiple of the alignment.
    const std::size_t rounded = (size + align - 1) / align * align;
    if (rounded < size) return nullptr;
    return std::aligned_alloc(align, rounded);
}

void free_bytes(void* p) noexcept { std::free(p); }

}  // namespace nova
