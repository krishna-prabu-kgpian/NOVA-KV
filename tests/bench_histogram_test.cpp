// bench_histogram_test.cpp -- the latency histogram used by the benchmark
// harness must not distort the numbers it reports.
#include "bench_common.h"

#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <vector>

using bench::LatencyHistogram;

int main() {
    // Bucket mapping: exact below 1024 ns; above, the lower bound never exceeds
    // the value and is within 1/64 of it; indices are monotonic.
    bool exact = true, bounded = true, monotonic = true;
    std::size_t prev = 0;
    for (std::uint64_t v = 0; v < 3000000; v += (v < 5000 ? 1 : 997)) {
        const std::size_t i = LatencyHistogram::index_of(v);
        const std::uint64_t lo = LatencyHistogram::lower_bound_of(i);
        if (v < 1024 && lo != v) exact = false;
        if (lo > v || (v >= 1024 && (v - lo) * 64 > v)) bounded = false;
        if (i < prev) monotonic = false;
        prev = i;
    }
    NOVA_CHECK(exact);
    NOVA_CHECK(bounded);
    NOVA_CHECK(monotonic);
    NOVA_CHECK(LatencyHistogram::index_of(~std::uint64_t{0}) < LatencyHistogram::kBuckets);

    // Percentiles agree with a sorted vector using the same rank rule as the
    // supplied generator: sorted[min(N - 1, floor(N * p))].
    LatencyHistogram h;
    std::vector<std::uint64_t> samples;
    bench::Rng rng(5);
    for (int i = 0; i < 200000; ++i) {
        std::uint64_t v = 20 + rng.next() % 200;             // bulk: 20..219 ns
        if (i % 100 == 0) v = 3000 + rng.next() % 9000;      // 1 %: microseconds
        if (i % 20000 == 0) v = 400000 + rng.next() % 1000;  // rare stalls
        h.record(v);
        samples.push_back(v);
    }
    std::sort(samples.begin(), samples.end());
    NOVA_CHECK(h.total() == samples.size());
    NOVA_CHECK(h.max() == samples.back());  // the maximum is exact, never bucketed
    for (double p : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
        const std::uint64_t want =
            samples[std::min(samples.size() - 1, static_cast<std::size_t>(static_cast<double>(samples.size()) * p))];
        const std::uint64_t got = h.percentile(p);
        NOVA_CHECK(got <= want);
        NOVA_CHECK((want - got) * 64 <= want);
        if (want < 1024) NOVA_CHECK(got == want);
    }
    double sum = 0;
    for (std::uint64_t v : samples) sum += static_cast<double>(v);
    NOVA_CHECK(std::abs(h.mean() - sum / static_cast<double>(samples.size())) < 1e-6);

    // Merging per-thread histograms equals recording everything in one.
    LatencyHistogram a, b, merged;
    for (std::size_t i = 0; i < samples.size(); ++i) (i % 2 == 0 ? a : b).record(samples[i]);
    merged.merge(a);
    merged.merge(b);
    NOVA_CHECK(merged.total() == h.total() && merged.max() == h.max());
    NOVA_CHECK(merged.percentile(0.99) == h.percentile(0.99));
    NOVA_CHECK(merged.percentile(0.5) == h.percentile(0.5));

    NOVA_CHECK(LatencyHistogram().percentile(0.99) == 0);  // empty histogram
    return nova_test::finish("bench_histogram_test");
}
