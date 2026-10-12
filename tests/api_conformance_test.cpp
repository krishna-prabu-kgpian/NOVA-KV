// api_conformance_test.cpp -- compile-time pin of the assignment's Appendix A.1
// API. If kvstore.h drifts from the mandated signatures this file stops
// compiling. Nothing here depends on how the store is implemented.
#include "kvstore.h"

#include "test_util.h"

#include <type_traits>
#include <utility>

namespace {

// ---- KVStore: special members ----------------------------------------------
static_assert(std::is_default_constructible<KVStore>::value, "KVStore()");
static_assert(std::is_destructible<KVStore>::value, "~KVStore()");
static_assert(!std::is_copy_constructible<KVStore>::value, "KVStore copy ctor deleted");
static_assert(!std::is_copy_assignable<KVStore>::value, "KVStore copy assign deleted");
static_assert(!std::is_move_constructible<KVStore>::value, "KVStore not movable");
static_assert(!std::is_move_assignable<KVStore>::value, "KVStore not movable");

// ---- KVStore: Part A / Part B (non-overloaded names: exact type match) -----
static_assert(std::is_same<decltype(&KVStore::put),
                           bool (KVStore::*)(long, const void*, size_t)>::value,
              "bool put(long, const void*, size_t)");
static_assert(std::is_same<decltype(&KVStore::erase), bool (KVStore::*)(long)>::value,
              "bool erase(long)");
static_assert(std::is_same<decltype(&KVStore::set_reclaim_batch),
                           void (KVStore::*)(int)>::value,
              "void set_reclaim_batch(int)");
static_assert(std::is_same<decltype(&KVStore::create_index),
                           bool (KVStore::*)(size_t, size_t)>::value,
              "bool create_index(size_t, size_t)");
static_assert(std::is_same<decltype(&KVStore::acquire_snapshot),
                           KVStore::Snapshot (KVStore::*)() const>::value,
              "Snapshot acquire_snapshot() const");
static_assert(std::is_same<decltype(&KVStore::keys),
                           size_t (KVStore::*)(const KVStore::Snapshot&, long*, size_t) const>::value,
              "size_t keys(const Snapshot&, long*, size_t) const");

// ---- KVStore: overloaded names (each exact overload must exist) ------------
// In C++17 noexcept is part of the function type, and selecting an overload by
// address requires an exact match, so these also reject added qualifiers.
constexpr bool (KVStore::*kGet)(long, void*, size_t) const = &KVStore::get;
constexpr bool (KVStore::*kGetSnap)(const KVStore::Snapshot&, long, void*, size_t) const =
    &KVStore::get;
constexpr int (KVStore::*kIndexGet)(const void*, size_t, long*, int) const = &KVStore::index_get;
constexpr int (KVStore::*kIndexGetSnap)(const KVStore::Snapshot&, const void*, size_t, long*,
                                        int) const = &KVStore::index_get;

// ---- KVStore::Snapshot: move-only ------------------------------------------
static_assert(std::is_nothrow_move_constructible<KVStore::Snapshot>::value,
              "Snapshot(Snapshot&&) noexcept");
static_assert(std::is_nothrow_move_assignable<KVStore::Snapshot>::value,
              "Snapshot& operator=(Snapshot&&) noexcept");
static_assert(std::is_destructible<KVStore::Snapshot>::value, "~Snapshot()");
static_assert(!std::is_copy_constructible<KVStore::Snapshot>::value, "Snapshot copy deleted");
static_assert(!std::is_copy_assignable<KVStore::Snapshot>::value, "Snapshot copy deleted");

// ---- Transaction ------------------------------------------------------------
static_assert(std::is_constructible<Transaction, KVStore&>::value, "Transaction(KVStore&)");
static_assert(!std::is_convertible<KVStore&, Transaction>::value, "Transaction ctor is explicit");
static_assert(!std::is_constructible<Transaction, const KVStore&>::value,
              "Transaction takes a mutable store reference");
static_assert(!std::is_default_constructible<Transaction>::value, "no Transaction()");
static_assert(std::is_destructible<Transaction>::value, "~Transaction()");
static_assert(!std::is_copy_constructible<Transaction>::value, "Transaction copy deleted");
static_assert(!std::is_copy_assignable<Transaction>::value, "Transaction copy deleted");
static_assert(!std::is_move_constructible<Transaction>::value, "Transaction not movable");
static_assert(!std::is_move_assignable<Transaction>::value, "Transaction not movable");
static_assert(std::is_same<decltype(&Transaction::put),
                           void (Transaction::*)(long, const void*, size_t)>::value,
              "void Transaction::put(long, const void*, size_t)");
static_assert(std::is_same<decltype(&Transaction::commit), bool (Transaction::*)()>::value,
              "bool Transaction::commit()");

}  // namespace

int main() {
    // The overload constants must be odr-used so an unresolved overload is a
    // link error rather than silently discarded.
    NOVA_CHECK(kGet != nullptr);
    NOVA_CHECK(kGetSnap != nullptr);
    NOVA_CHECK(kIndexGet != nullptr);
    NOVA_CHECK(kIndexGetSnap != nullptr);
    return nova_test::finish("api_conformance_test");
}
