// kvstore.h -- NOVA-KV public API.
//
// Every public signature below is taken from the assignment's Appendix A.1 and
// is a compatibility contract: no extra mandatory arguments, no changed return
// types, no added qualifiers (tests/api_conformance_test.cpp pins them at
// compile time). The complete Part A + B + C surface is declared from the first
// milestone because the supplied workload_gen.cpp needs a complete Transaction
// type even when it is built against a Part A store.
//
// Implementation status is tracked in README.md; a declared method is not a
// claim that the corresponding assignment part is implemented.
#pragma once

#include <cstddef>
#include <stddef.h>

class Transaction;

class KVStore {
public:
    KVStore();
    ~KVStore();
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;
    // move constructor/assignment: left undeclared (Appendix A.1 note)

    // ---- Part A ----
    bool put(long key, const void* value, size_t value_len);
    bool get(long key, void* value_out, size_t value_out_len) const;
    // returns true if found (value copied into value_out)
    bool erase(long key);
    // returns true if the key was present and is now removed

    // ---- Part B ----
    void set_reclaim_batch(int n);
    // sets how many retired versions one reclamation pass may free

    // ---- Part C ----
    bool create_index(size_t field_offset, size_t field_len);
    // builds a secondary index over a byte range within each stored value

    int index_get(const void* field_value, size_t field_len,
                  long* keys_out, int max_keys) const;
    // returns the number of matches (up to max_keys), primary keys in keys_out

    // An opaque, move-only handle on one published version. Appendix A.1 lists
    // it under "added to KVStore", so it is the nested type KVStore::Snapshot.
    class Snapshot {
    public:
        Snapshot(Snapshot&&) noexcept;
        Snapshot& operator=(Snapshot&&) noexcept;
        ~Snapshot();
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;

    private:
        friend class KVStore;
        Snapshot(const KVStore* owner, void* pin) noexcept;
        void release() noexcept;

        const KVStore* owner_;  // store the handle was acquired from; null when moved-from
        void* pin_;             // opaque reclamation pin token; null when inert
    };

    Snapshot acquire_snapshot() const;
    // hands back a Snapshot pinned to the current version

    bool get(const Snapshot& snap, long key,
             void* value_out, size_t value_out_len) const;
    int index_get(const Snapshot& snap, const void* field_value, size_t field_len,
                  long* keys_out, int max_keys) const;
    size_t keys(const Snapshot& snap, long* keys_out, size_t max_keys) const;
    // enumerates up to max_keys primary keys present in the table under snap;
    // returns the number written to keys_out

private:
    friend class Transaction;
    struct Impl;
    Impl* impl_;
};

class Transaction {
public:
    explicit Transaction(KVStore& store);
    ~Transaction();  // discards any uncommitted writes
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void put(long key, const void* value, size_t value_len);
    bool commit();
    // true if committed; all staged writes become visible in one publish

private:
    struct Impl;
    KVStore& store_;
    Impl* impl_;
};
