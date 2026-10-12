// smoke_test.cpp -- bootstrap smoke test, built once per store variant.
//
// It asserts only properties that hold for an EMPTY store in every milestone
// (lookups miss, erase of an absent key fails, handles construct/move/destroy),
// plus the build wiring of the synchronization variant. It deliberately makes
// no assertion about put()/commit() results: storage semantics are tested by
// kvstore_test.cpp from milestone A1 onward.
#include "kvstore.h"

#include "build_info.h"
#include "test_util.h"

#include <cstring>
#include <utility>

#ifndef NOVA_EXPECTED_SYNC_MODE
#error "NOVA_EXPECTED_SYNC_MODE must be defined by the build"
#endif

int main() {
    NOVA_CHECK(std::strcmp(nova_kv_sync_mode_name(), NOVA_EXPECTED_SYNC_MODE) == 0);

    KVStore store;
    const KVStore& cstore = store;

    // Point lookups on an empty store miss and leave the caller's buffer alone.
    unsigned char buf[16];
    std::memset(buf, 0xAB, sizeof(buf));
    NOVA_CHECK(!cstore.get(42, buf, sizeof(buf)));
    NOVA_CHECK(!cstore.get(-1, buf, sizeof(buf)));
    NOVA_CHECK(!cstore.get(0, nullptr, 0));
    bool untouched = true;
    for (unsigned char b : buf) untouched = untouched && (b == 0xAB);
    NOVA_CHECK(untouched);

    NOVA_CHECK(!store.erase(42));

    long keys_out[4] = {7, 7, 7, 7};
    NOVA_CHECK(cstore.index_get("SYM001", 8, keys_out, 4) == 0);
    NOVA_CHECK(keys_out[0] == 7);

    store.set_reclaim_batch(64);
    store.set_reclaim_batch(0);
    store.set_reclaim_batch(-5);

    // Snapshot handle lifecycle: acquire, move-construct, move-assign, destroy.
    {
        KVStore::Snapshot a = cstore.acquire_snapshot();
        KVStore::Snapshot b = std::move(a);
        KVStore::Snapshot c = cstore.acquire_snapshot();
        c = std::move(b);
        NOVA_CHECK(!cstore.get(c, 42, buf, sizeof(buf)));
        NOVA_CHECK(cstore.index_get(c, "SYM001", 8, keys_out, 4) == 0);
        NOVA_CHECK(cstore.keys(c, keys_out, 4) == 0);
        NOVA_CHECK(keys_out[0] == 7);
    }

    // A Transaction destroyed without commit() must be harmless.
    {
        Transaction txn(store);
        const long v = 1;
        txn.put(1, &v, sizeof(v));
    }
    NOVA_CHECK(!cstore.get(1, buf, sizeof(buf)));

    return nova_test::finish("smoke_test[" NOVA_EXPECTED_SYNC_MODE "]");
}
