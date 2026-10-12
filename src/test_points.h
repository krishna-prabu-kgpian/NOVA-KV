// test_points.h -- named pause points for deterministic interleaving tests.
//
// With NOVA_KV_TEST_HOOKS defined (only the *_testhooks libraries), each
// NOVA_TEST_POINT(p) calls a test-installed function, which a test uses to park
// a thread at an exact step and let another thread run. Without the macro the
// points compile to nothing: production variants contain no hook code.
#pragma once

namespace nova {

enum class TestPoint : int {
    // ---- reader (get / pin) ----
    ReaderBeforeAnnounce = 1,    // epoch read, slot not yet written
    ReaderAfterAnnounce = 2,     // slot written, epoch not yet re-read
    ReaderAfterValidate = 3,     // announcement validated, root not yet loaded
    ReaderAfterRootLoad = 4,     // the one load of `current` is done
    ReaderBeforeCopyOut = 5,     // entry found, bytes not yet copied
    // ---- writer (commit) ----
    WriterAfterAppends = 10,           // values appended, no ownership taken yet
    WriterEndCommitBeforeRelease = 11, // new segments own their chunks; commit references not yet dropped
    WriterBeforePublish = 12,          // new version fully built, not yet visible
    WriterAfterPublish = 13,           // root swapped, old version not yet tagged or queued
    WriterAfterRetireTag = 14,         // retirement epoch sampled, record not yet queued
    // ---- cleaner (reclamation pass) ----
    CleanerBeforeScan = 20,      // epoch read, no slot examined yet
    CleanerMidScan = 21,         // after examining each slot
    CleanerBeforeAdvance = 22,   // scan passed, epoch not yet advanced
    CleanerMidVersion = 23,      // between two directory entries of a version being released
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
