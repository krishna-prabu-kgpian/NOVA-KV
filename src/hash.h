// hash.h -- the one key hash used by writers, readers and rebuilds.
#pragma once

#include "config.h"

#include <cstdint>

namespace nova {

// MurmurHash3's 64-bit finalizer: a fixed bijective avalanche mixer over the
// key's bits. Being a bijection, two distinct keys never share a full hash;
// they can still share a partition and a start slot, which linear probing with
// full-key comparison resolves.
inline std::uint64_t hash_key(long key) noexcept {
    std::uint64_t x = static_cast<std::uint64_t>(key);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Low bits select the partition; the bits above them select the start slot
// inside the partition's segment (HashSegment shifts by kPartitionBits).
inline std::uint32_t partition_of(std::uint64_t h) noexcept {
    return static_cast<std::uint32_t>(h) & (kPartitionCount - 1);
}

}  // namespace nova
