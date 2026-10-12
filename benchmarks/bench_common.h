// bench_common.h -- shared pieces of the project's own benchmark harnesses
// (not part of the assignment API, not used by the supplied workload generator).
#pragma once

#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bench {

// Same record as the supplied workload generator's Trade.
struct Trade {
    long trade_id;
    char symbol[8];
    double price;
    long ts;
};
static_assert(sizeof(Trade) == 32, "Trade must match the workload's 32-byte record");

// Keeps a value "used" so the compiler cannot delete the code that produced it.
template <class T>
__attribute__((always_inline)) inline void do_not_optimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// xorshift64*: per-thread generator, no shared state.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL) {
        if (s == 0) s = 1;
    }
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1DULL;
    }
};

// Pins the calling thread to one logical CPU. Returns false if the kernel refused.
inline bool pin_this_thread(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

// Latency histogram with fixed memory (no allocation while measuring).
//   value < 1024 ns : one bucket per nanosecond (exact)
//   value >= 1024 ns: 64 buckets per power of two (bucket width <= 1.6 % of value)
// Every sample is counted; nothing is dropped or clipped. The exact maximum and
// the exact sum are tracked separately.
class LatencyHistogram {
public:
    static constexpr std::size_t kLinear = 1024;
    static constexpr std::size_t kSub = 64;
    static constexpr std::size_t kBuckets = kLinear + (64 - 10) * kSub;

    LatencyHistogram() : counts_(kBuckets, 0) {}

    void record(std::uint64_t ns) {
        ++counts_[index_of(ns)];
        ++total_;
        sum_ += ns;
        if (ns > max_) max_ = ns;
    }

    void merge(const LatencyHistogram& o) {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        if (o.max_ > max_) max_ = o.max_;
    }

    std::uint64_t total() const { return total_; }
    std::uint64_t max() const { return max_; }
    double mean() const { return total_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(total_); }

    // Lower bound of the bucket holding the sample of rank floor(N * p)
    // (0-based, clamped to N - 1): the same rank rule as the supplied workload
    // generator's percentile(), at bucket resolution.
    std::uint64_t percentile(double p) const {
        if (total_ == 0) return 0;
        std::uint64_t rank = static_cast<std::uint64_t>(static_cast<double>(total_) * p);
        if (rank >= total_) rank = total_ - 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen > rank) return lower_bound_of(i);
        }
        return max_;
    }

    // Writes "bucket_lower_ns,count" for every non-empty bucket.
    void dump(std::FILE* f, const char* series) const {
        for (std::size_t i = 0; i < kBuckets; ++i) {
            if (counts_[i] != 0) {
                std::fprintf(f, "%s,%llu,%llu\n", series,
                             static_cast<unsigned long long>(lower_bound_of(i)),
                             static_cast<unsigned long long>(counts_[i]));
            }
        }
    }

    static std::size_t index_of(std::uint64_t ns) {
        if (ns < kLinear) return static_cast<std::size_t>(ns);
        const unsigned msb = 63u - static_cast<unsigned>(__builtin_clzll(ns));  // >= 10
        const std::size_t sub = static_cast<std::size_t>((ns >> (msb - 6)) & (kSub - 1));
        return kLinear + (msb - 10) * kSub + sub;
    }

    static std::uint64_t lower_bound_of(std::size_t index) {
        if (index < kLinear) return index;
        const std::size_t rel = index - kLinear;
        const unsigned msb = 10u + static_cast<unsigned>(rel / kSub);
        const std::uint64_t sub = rel % kSub;
        return (kSub + sub) << (msb - 6);
    }

private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
};

// Minimal "--name value" command-line reader.
class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) args_.emplace_back(argv[i]);
    }
    std::string get(const char* name, const char* fallback) {
        for (std::size_t i = 0; i + 1 < args_.size(); ++i) {
            if (args_[i] == name) {
                used_[i] = used_[i + 1] = true;
                return args_[i + 1];
            }
        }
        return fallback;
    }
    long get_long(const char* name, long fallback) {
        const std::string v = get(name, "");
        return v.empty() ? fallback : std::strtol(v.c_str(), nullptr, 10);
    }
    double get_double(const char* name, double fallback) {
        const std::string v = get(name, "");
        return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
    }
    bool has_flag(const char* name) {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            if (args_[i] == name) {
                used_[i] = true;
                return true;
            }
        }
        return false;
    }
    // Returns the first argument no get()/has_flag() call consumed, or "".
    std::string first_unused() const {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            if (!used_[i]) return args_[i];
        }
        return "";
    }
    void finalize() { used_.resize(args_.size(), false); }

private:
    std::vector<std::string> args_;
    std::vector<bool> used_ = std::vector<bool>(256, false);
};

inline std::vector<int> parse_cpu_list(const std::string& s) {
    std::vector<int> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        if (comma > pos) out.push_back(std::atoi(s.substr(pos, comma - pos).c_str()));
        pos = comma + 1;
    }
    return out;
}

}  // namespace bench
