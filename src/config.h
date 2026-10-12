// config.h -- storage-engine parameters (blueprint section 0 defaults; see
// docs/decisions.md D-018). These are initial engineering defaults, not
// measured optima.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nova {

// ---- Hash index ---------------------------------------------------------------
constexpr unsigned kPartitionBits = 8;
constexpr std::uint32_t kPartitionCount = 1u << kPartitionBits;
constexpr std::uint32_t kInitialSegmentCapacity = 1024;  // slots, power of two
constexpr std::uint32_t kMaxSegmentCapacity = 1u << 30;
// Part B (copy-on-write) segments start small: every write copies the whole
// segment it touches, so a segment should be no larger than its contents need.
constexpr std::uint32_t kCowInitialSegmentCapacity = 16;

// Occupied slots (live + tombstones) may fill at most 7/10 of a segment.
constexpr std::uint64_t kMaxLoadNum = 7;
constexpr std::uint64_t kMaxLoadDen = 10;
// A segment is rebuilt once tombstones reach 1/5 of its capacity.
constexpr std::uint64_t kTombstoneRebuildDen = 5;

// ---- Value arena --------------------------------------------------------------
constexpr std::uint32_t kChunkBytes = 64 * 1024;
constexpr std::uint32_t kMaxValueBytes = 1u << 30;  // longer values are rejected

// Global chunk directory: fixed two-level table, blocks installed on demand.
constexpr unsigned kChunkDirBlockBits = 10;
constexpr std::uint32_t kChunkDirBlockSlots = 1u << kChunkDirBlockBits;
constexpr std::uint32_t kChunkDirMaxBlocks = 1024;

// chunk_id of the ValueRef that represents a zero-length value. It is never
// dereferenced and never a real chunk ID (IDs stay below 2^20).
constexpr std::uint32_t kNoChunk = 0xFFFFFFFFu;

constexpr std::size_t kCacheLine = 64;

}  // namespace nova
