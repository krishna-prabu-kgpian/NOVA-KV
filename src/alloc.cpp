#include "alloc.h"

#include "test_points.h"

#include <atomic>
#include <cstdlib>

namespace nova {

#ifdef NOVA_KV_TEST_HOOKS
namespace {
// Atomic: since Part C a Transaction stages its writes (and so allocates) on
// whatever thread uses it, concurrently with the writer.
std::atomic<long> g_fail_at{-1};
std::atomic<long> g_calls{0};
std::atomic<bool> g_fired{false};
}  // namespace

namespace test_hooks {
void fail_nth_allocation(long n) noexcept {
    g_fail_at.store(n);
    g_calls.store(0);
    g_fired.store(false);
}
bool allocation_failure_fired() noexcept { return g_fired.load(); }
long allocations_since_armed() noexcept { return g_calls.load(); }
}  // namespace test_hooks
#endif

#ifdef NOVA_KV_TEST_HOOKS
namespace test_hooks {
namespace {
std::atomic<PointFn> g_point_fn{nullptr};
std::atomic<void*> g_point_context{nullptr};
std::atomic<long> g_live[4] = {};
}  // namespace
void set_point_hook(PointFn fn, void* context) noexcept {
    g_point_context.store(context);
    g_point_fn.store(fn);
}
void at_point(TestPoint point) noexcept {
    const PointFn fn = g_point_fn.load();
    if (fn != nullptr) fn(point, g_point_context.load());
}
long live_objects(LiveObject kind) noexcept { return g_live[static_cast<int>(kind)].load(); }
void count_object(LiveObject kind, long delta) noexcept {
    g_live[static_cast<int>(kind)].fetch_add(delta);
}
}  // namespace test_hooks
#endif

void* alloc_bytes(std::size_t size, std::size_t align) noexcept {
#ifdef NOVA_KV_TEST_HOOKS
    const long call = g_calls.fetch_add(1);
    const long fail_at = g_fail_at.load();
    if (fail_at >= 0 && call == fail_at) {
        g_fired.store(true);
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
