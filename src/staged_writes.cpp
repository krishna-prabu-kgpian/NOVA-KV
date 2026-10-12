#include "staged_writes.h"

#include "alloc.h"

#include <cstring>

namespace nova {

namespace {
constexpr std::size_t kInitialOps = 16;
constexpr std::size_t kInitialBytes = 1024;
}  // namespace

StagedWrites::~StagedWrites() { clear(); }

void StagedWrites::clear() noexcept {
    free_bytes(ops_);
    free_bytes(buffer_);
    ops_ = nullptr;
    buffer_ = nullptr;
    count_ = capacity_ = 0;
    used_ = buffer_capacity_ = 0;
}

bool StagedWrites::put(long key, const void* value, std::uint32_t length) noexcept {
    // Both arrays are grown (if needed) before either is changed, so a failure
    // leaves the set exactly as it was.
    StagedOp* new_ops = nullptr;
    std::size_t new_capacity = capacity_;
    if (count_ == capacity_) {
        new_capacity = capacity_ == 0 ? kInitialOps : capacity_ * 2;
        if (new_capacity > static_cast<std::size_t>(-1) / sizeof(StagedOp)) return false;
        new_ops = static_cast<StagedOp*>(alloc_bytes(new_capacity * sizeof(StagedOp), alignof(StagedOp)));
        if (new_ops == nullptr) return false;
    }
    unsigned char* new_buffer = nullptr;
    std::size_t new_buffer_capacity = buffer_capacity_;
    if (length > buffer_capacity_ - used_) {
        new_buffer_capacity = buffer_capacity_ == 0 ? kInitialBytes : buffer_capacity_;
        while (new_buffer_capacity - used_ < length) {
            if (new_buffer_capacity > static_cast<std::size_t>(-1) / 2) {
                free_bytes(new_ops);
                return false;
            }
            new_buffer_capacity *= 2;
        }
        new_buffer = static_cast<unsigned char*>(alloc_bytes(new_buffer_capacity));
        if (new_buffer == nullptr) {
            free_bytes(new_ops);
            return false;
        }
    }
    // Nothing below can fail.
    if (new_ops != nullptr) {
        if (count_ != 0) std::memcpy(new_ops, ops_, count_ * sizeof(StagedOp));
        free_bytes(ops_);
        ops_ = new_ops;
        capacity_ = new_capacity;
    }
    if (new_buffer != nullptr) {
        if (used_ != 0) std::memcpy(new_buffer, buffer_, used_);
        free_bytes(buffer_);
        buffer_ = new_buffer;
        buffer_capacity_ = new_buffer_capacity;
    }
    if (length != 0) std::memcpy(buffer_ + used_, value, length);
    ops_[count_++] = StagedOp{key, used_, length};
    used_ += length;
    return true;
}

}  // namespace nova
