#!/usr/bin/env bash
# The assignment forbids pulling in an existing concurrent hash-map library and
# requires our own hash table. Fails if store code (include/, src/, drivers/,
# benchmarks/) includes a third-party container library or a standard hash
# container. std::unordered_map is allowed in tests/ only, as an oracle.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

FORBIDDEN='#[[:space:]]*include[[:space:]]*[<"](tbb/|oneapi/|folly/|boost/|absl/|libcuckoo/|cuckoohash|junction/|parallel_hashmap/|phmap|robin_hood|tsl/|ankerl/|unordered_map|unordered_set|map>|set>)'
status=0

hits="$(grep -rnE "$FORBIDDEN" include src drivers benchmarks --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.cc' 2>/dev/null)"
if [ -n "$hits" ]; then
    echo "forbidden container/library include in store code:"; echo "$hits"; status=1
fi

# Third-party libraries may only enter through the build system.
build_hits="$(grep -nE 'find_package|FetchContent|ExternalProject|add_subdirectory|pkg_check_modules' CMakeLists.txt | grep -v 'find_package(Threads REQUIRED)')"
if [ -n "$build_hits" ]; then
    echo "unexpected external dependency in CMakeLists.txt:"; echo "$build_hits"; status=1
fi

[ $status -eq 0 ] && echo "no external container or concurrent hash-map dependency found"
exit $status
