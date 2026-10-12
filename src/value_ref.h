// value_ref.h -- location of one value's bytes in the chunked arena.
#pragma once

#include <cstdint>

namespace nova {

// The hash index stores this instead of the value itself: key indexing and
// value storage are separate structures. A zero-length value is
// {kNoChunk, 0, 0} and owns no arena bytes.
struct ValueRef {
    std::uint32_t chunk_id;
    std::uint32_t offset;
    std::uint32_t length;
};

}  // namespace nova
