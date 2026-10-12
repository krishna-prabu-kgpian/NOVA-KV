// staged_writes.h -- the private write set of a Transaction (Part C).
//
// Transaction::put() copies the caller's bytes here at once; the caller's
// buffer is never referenced again. Nothing in this object is visible to any
// reader or to the store until Transaction::commit() hands the whole set to the
// store as ONE logical write. Destroying it discards the set.
//
// Memory comes from nova::alloc_bytes, so an allocation failure is an ordinary,
// testable return value: put() returns false and leaves the set as it was.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nova {

struct StagedOp {
    long key;
    std::size_t offset;    // of the value's bytes in the set's buffer
    std::uint32_t length;
};

class StagedWrites {
public:
    StagedWrites() noexcept = default;
    ~StagedWrites();
    StagedWrites(const StagedWrites&) = delete;
    StagedWrites& operator=(const StagedWrites&) = delete;

    // Appends one write, copying `length` bytes. `value` may be null only if
    // `length` is 0. False on allocation failure (the set is unchanged).
    bool put(long key, const void* value, std::uint32_t length) noexcept;

    std::size_t count() const noexcept { return count_; }
    const StagedOp& op(std::size_t i) const noexcept { return ops_[i]; }
    // The copied bytes of a staged write; valid until the next put() or clear().
    const unsigned char* bytes(const StagedOp& o) const noexcept { return buffer_ + o.offset; }
    std::size_t value_bytes() const noexcept { return used_; }

    // Discards every staged write and frees the memory.
    void clear() noexcept;

private:
    StagedOp* ops_ = nullptr;
    std::size_t count_ = 0;
    std::size_t capacity_ = 0;
    unsigned char* buffer_ = nullptr;
    std::size_t used_ = 0;
    std::size_t buffer_capacity_ = 0;
};

}  // namespace nova
