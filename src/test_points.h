// test_points.h -- named pause points for deterministic interleaving tests.
//
// With NOVA_KV_TEST_HOOKS defined (only the *_testhooks libraries), each
// NOVA_TEST_POINT(p) calls a test-installed function, which a test uses to park
// a thread at an exact step and let another thread run. Without the macro the
// points compile to nothing: production variants contain no hook code.
#pragma once

namespace nova {

enum class TestPoint : int {
    ReaderAfterRootLoad = 1,     // get(): the one load of `current` is done
    ReaderBeforeCopyOut = 2,     // get(): entry found, bytes not yet copied
    WriterBeforePublish = 3,     // commit: new version fully built, not yet visible
    WriterAfterPublish = 4,      // commit: root swapped, old version not yet retired
};

// Kinds of engine objects whose creation and destruction are counted in hook
// builds, so a test can assert that destroying a store released everything
// (independently of a leak checker).
enum class LiveObject : int { Version = 0, Segment = 1, Chunk = 2 };

#ifdef NOVA_KV_TEST_HOOKS
namespace test_hooks {
using PointFn = void (*)(TestPoint point, void* context);
// Installs (or with nullptr removes) the function called at every test point.
void set_point_hook(PointFn fn, void* context) noexcept;
void at_point(TestPoint point) noexcept;
// Process-wide number of objects of a kind that currently exist.
long live_objects(LiveObject kind) noexcept;
void count_object(LiveObject kind, long delta) noexcept;
}  // namespace test_hooks
#define NOVA_TEST_POINT(p) ::nova::test_hooks::at_point(p)
#define NOVA_COUNT_OBJECT(kind, delta) ::nova::test_hooks::count_object(kind, delta)
#else
#define NOVA_TEST_POINT(p) ((void)0)
#define NOVA_COUNT_OBJECT(kind, delta) ((void)0)
#endif

}  // namespace nova
