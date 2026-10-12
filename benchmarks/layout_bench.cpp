// layout_bench.cpp -- storage-layout microbenchmark for the Part A design note.
//
// Three physical layouts of the SAME logical table of Trade records
// {long trade_id; char symbol[8]; double price; long ts} are built from the same
// data and driven with the same pre-generated access sequences:
//
//   NSM  row-major:    one array of 32-byte rows; a row is one contiguous block.
//   DSM  column-major: four parallel arrays, one per field.
//   PAX  hybrid:       4096-byte pages of 128 records; inside a page the fields
//                      are stored column-major in four 1024-byte mini-pages.
//
// Records are addressed by record number in every layout, so the measurement
// isolates the cost of the layout itself; the hash index that would map a key
// to a record number is identical for all three and deliberately excluded.
//
// Access patterns (each is real memory traffic; results are folded into a
// checksum that is compared across layouts and also kept alive against the
// optimizer):
//   row_chain    whole-row point get, each get's position taken from the row
//                just read (dependent accesses: true per-get latency)
//   row_uniform  whole-row point get, independent uniformly random positions
//                (accesses may overlap in the CPU: throughput)
//   row_hot      whole-row point get, 80 % from a hot 1 % of records, 20 % random
//                (the workload generator's read mix)
//   sym_price    two fields (symbol + price) of a random record
//   field_price  one field (price) of a random record
//   scan_price   sequential scan summing one field over the whole table
//
// Output: one CSV row per (layout, pattern, repetition).
#include "bench_common.h"

#include <algorithm>
#include <memory>
#include <numeric>

namespace {

using bench::Trade;

constexpr std::size_t kPageRecords = 128;  // 128 * 32 bytes = one 4096-byte page

struct FreeDeleter {
    void operator()(void* p) const { std::free(p); }
};
template <class T>
using Buffer = std::unique_ptr<T[], FreeDeleter>;

template <class T>
Buffer<T> allocate(std::size_t count, std::size_t align) {
    const std::size_t bytes = (count * sizeof(T) + align - 1) / align * align;
    void* p = std::aligned_alloc(align, bytes);
    if (p == nullptr) {
        std::fprintf(stderr, "allocation of %zu bytes failed\n", bytes);
        std::exit(1);
    }
    std::memset(p, 0, bytes);  // touch every page before any measurement
    return Buffer<T>(static_cast<T*>(p));
}

// ---- NSM ------------------------------------------------------------------------
struct Nsm {
    static constexpr const char* kName = "nsm";
    Buffer<Trade> rows;
    explicit Nsm(std::size_t n) : rows(allocate<Trade>(n, 64)) {}
    void set(std::size_t i, const Trade& t) { rows[i] = t; }
    void get_row(std::size_t i, Trade& out) const { out = rows[i]; }
    double price(std::size_t i) const { return rows[i].price; }
    std::uint64_t symbol(std::size_t i) const {
        std::uint64_t s;
        std::memcpy(&s, rows[i].symbol, 8);
        return s;
    }
    static std::size_t bytes(std::size_t n) { return n * sizeof(Trade); }
};

// ---- DSM ------------------------------------------------------------------------
struct Dsm {
    static constexpr const char* kName = "dsm";
    Buffer<long> ids;
    Buffer<std::uint64_t> symbols;
    Buffer<double> prices;
    Buffer<long> tss;
    explicit Dsm(std::size_t n)
        : ids(allocate<long>(n, 64)),
          symbols(allocate<std::uint64_t>(n, 64)),
          prices(allocate<double>(n, 64)),
          tss(allocate<long>(n, 64)) {}
    void set(std::size_t i, const Trade& t) {
        ids[i] = t.trade_id;
        std::memcpy(&symbols[i], t.symbol, 8);
        prices[i] = t.price;
        tss[i] = t.ts;
    }
    void get_row(std::size_t i, Trade& out) const {  // reconstruct the row from 4 arrays
        out.trade_id = ids[i];
        std::memcpy(out.symbol, &symbols[i], 8);
        out.price = prices[i];
        out.ts = tss[i];
    }
    double price(std::size_t i) const { return prices[i]; }
    std::uint64_t symbol(std::size_t i) const { return symbols[i]; }
    static std::size_t bytes(std::size_t n) { return n * 32; }
};

// ---- PAX ------------------------------------------------------------------------
struct PaxPage {
    long ids[kPageRecords];
    std::uint64_t symbols[kPageRecords];
    double prices[kPageRecords];
    long tss[kPageRecords];
};
static_assert(sizeof(PaxPage) == 4096, "a PAX page must be exactly 4096 bytes");

struct Pax {
    static constexpr const char* kName = "pax";
    Buffer<PaxPage> pages;
    explicit Pax(std::size_t n) : pages(allocate<PaxPage>((n + kPageRecords - 1) / kPageRecords, 4096)) {}
    void set(std::size_t i, const Trade& t) {
        PaxPage& p = pages[i / kPageRecords];
        const std::size_t s = i % kPageRecords;
        p.ids[s] = t.trade_id;
        std::memcpy(&p.symbols[s], t.symbol, 8);
        p.prices[s] = t.price;
        p.tss[s] = t.ts;
    }
    void get_row(std::size_t i, Trade& out) const {  // reconstruct from 4 mini-pages
        const PaxPage& p = pages[i / kPageRecords];
        const std::size_t s = i % kPageRecords;
        out.trade_id = p.ids[s];
        std::memcpy(out.symbol, &p.symbols[s], 8);
        out.price = p.prices[s];
        out.ts = p.tss[s];
    }
    double price(std::size_t i) const { return pages[i / kPageRecords].prices[i % kPageRecords]; }
    std::uint64_t symbol(std::size_t i) const {
        return pages[i / kPageRecords].symbols[i % kPageRecords];
    }
    static std::size_t bytes(std::size_t n) {
        return (n + kPageRecords - 1) / kPageRecords * sizeof(PaxPage);
    }
};

// ---- Data -----------------------------------------------------------------------
// `next` is a single random cycle over all records (Sattolo's algorithm); it is
// stored in each record's ts field and drives the row_chain pattern.
template <class L>
void fill(L& layout, std::size_t n, const std::vector<std::uint32_t>& next) {
    for (std::size_t i = 0; i < n; ++i) {
        Trade t;
        t.trade_id = static_cast<long>(i) * 1000 + 7;
        std::snprintf(t.symbol, sizeof(t.symbol), "SYM%03d", static_cast<int>(i % 50));
        t.price = 100.0 + static_cast<double>(i % 97) * 0.37;
        t.ts = static_cast<long>(next[i]);
        layout.set(i, t);
    }
}

std::uint64_t fold_row(const Trade& t) {
    std::uint64_t sym, price;
    std::memcpy(&sym, t.symbol, 8);
    std::memcpy(&price, &t.price, 8);
    return static_cast<std::uint64_t>(t.trade_id) + sym + price + static_cast<std::uint64_t>(t.ts);
}

// ---- Patterns: each returns a checksum of everything it read ------------------
template <class L>
__attribute__((noinline)) std::uint64_t run_row_chain(const L& layout, std::size_t ops) {
    std::uint64_t sum = 0;
    std::size_t pos = 0;
    Trade t;
    for (std::size_t k = 0; k < ops; ++k) {
        layout.get_row(pos, t);
        // Force the row to exist in the caller's buffer in every layout, as a
        // get() into a caller-supplied buffer does; the next position is then
        // read back from that buffer.
        bench::do_not_optimize(t);
        sum += fold_row(t);
        pos = static_cast<std::size_t>(t.ts);  // next position comes from this row
    }
    return sum;
}

template <class L>
__attribute__((noinline)) std::uint64_t run_rows(const L& layout, const std::uint32_t* idx,
                                                 std::size_t ops) {
    std::uint64_t sum = 0;
    Trade t;
    for (std::size_t k = 0; k < ops; ++k) {
        layout.get_row(idx[k], t);
        bench::do_not_optimize(t);
        sum += fold_row(t);
    }
    return sum;
}

template <class L>
__attribute__((noinline)) std::uint64_t run_sym_price(const L& layout, const std::uint32_t* idx,
                                                      std::size_t ops) {
    std::uint64_t sum = 0;
    for (std::size_t k = 0; k < ops; ++k) {
        const double p = layout.price(idx[k]);
        std::uint64_t bits;
        std::memcpy(&bits, &p, 8);
        sum += bits + layout.symbol(idx[k]);
    }
    return sum;
}

template <class L>
__attribute__((noinline)) std::uint64_t run_field_price(const L& layout, const std::uint32_t* idx,
                                                        std::size_t ops) {
    std::uint64_t sum = 0;
    for (std::size_t k = 0; k < ops; ++k) {
        const double p = layout.price(idx[k]);
        std::uint64_t bits;
        std::memcpy(&bits, &p, 8);
        sum += bits;
    }
    return sum;
}

template <class L>
__attribute__((noinline)) std::uint64_t run_scan_price(const L& layout, std::size_t n) {
    // Integer sum of the raw field bits: unlike a floating-point sum it has no
    // multi-cycle loop-carried dependency, so the loop is limited by how fast
    // the field can be fetched from memory, which is what is being compared.
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double p = layout.price(i);
        std::uint64_t bits;
        std::memcpy(&bits, &p, 8);
        sum += bits;
    }
    return sum;
}

struct Sequences {
    std::vector<std::uint32_t> uniform, hot;
};

struct Measurement {
    double ns_per_op;
    std::uint64_t checksum;
    std::size_t ops;
};

template <class L>
Measurement measure(const L& layout, const std::string& pattern, std::size_t n, std::size_t ops,
                    const Sequences& seq) {
    std::uint64_t sum = 0;
    std::size_t done = ops;
    const std::uint64_t t0 = bench::now_ns();
    if (pattern == "row_chain") sum = run_row_chain(layout, ops);
    else if (pattern == "row_uniform") sum = run_rows(layout, seq.uniform.data(), ops);
    else if (pattern == "row_hot") sum = run_rows(layout, seq.hot.data(), ops);
    else if (pattern == "sym_price") sum = run_sym_price(layout, seq.uniform.data(), ops);
    else if (pattern == "field_price") sum = run_field_price(layout, seq.uniform.data(), ops);
    else if (pattern == "scan_price") {
        // Scan the table as many times as needed to cover about `ops` reads.
        const std::size_t passes = std::max<std::size_t>(1, ops / n);
        for (std::size_t p = 0; p < passes; ++p) sum += run_scan_price(layout, n);
        done = passes * n;
    } else {
        std::fprintf(stderr, "unknown pattern %s\n", pattern.c_str());
        std::exit(2);
    }
    const std::uint64_t t1 = bench::now_ns();
    bench::do_not_optimize(sum);
    return Measurement{static_cast<double>(t1 - t0) / static_cast<double>(done), sum, done};
}

}  // namespace

int main(int argc, char** argv) {
    bench::Args args(argc, argv);
    const std::size_t n = static_cast<std::size_t>(args.get_long("--records", 100000));
    const std::size_t ops = static_cast<std::size_t>(args.get_long("--ops", 4000000));
    const int reps = static_cast<int>(args.get_long("--reps", 7));
    const std::string only_layout = args.get("--layout", "all");
    const std::string only_pattern = args.get("--pattern", "all");
    const int cpu = static_cast<int>(args.get_long("--cpu", -1));
    const std::string csv_path = args.get("--csv", "");
    const std::string run_id = args.get("--run-id", "adhoc");
    const std::uint64_t seed = static_cast<std::uint64_t>(args.get_long("--seed", 1));
    if (!args.first_unused().empty()) {
        std::fprintf(stderr, "unknown argument: %s\n", args.first_unused().c_str());
        return 2;
    }
    if (n < 2 || n > 0xFFFFFFFFu || ops < 1 || reps < 1) {
        std::fprintf(stderr, "invalid --records/--ops/--reps\n");
        return 2;
    }
    if (cpu >= 0 && !bench::pin_this_thread(cpu)) {
        std::fprintf(stderr, "cannot pin to cpu %d\n", cpu);
        return 1;
    }

    // ---- Access sequences, generated once and shared by all layouts.
    bench::Rng rng(seed);
    std::vector<std::uint32_t> next(n);
    std::iota(next.begin(), next.end(), 0u);
    for (std::size_t i = n - 1; i > 0; --i) {  // Sattolo: one cycle through all records
        const std::size_t j = static_cast<std::size_t>(rng.next() % i);
        std::swap(next[i], next[j]);
    }
    Sequences seq;
    seq.uniform.resize(ops);
    seq.hot.resize(ops);
    const std::size_t hot_count = std::max<std::size_t>(1, n / 100);
    for (std::size_t k = 0; k < ops; ++k) {
        seq.uniform[k] = static_cast<std::uint32_t>(rng.next() % n);
        const std::uint64_t r = rng.next();
        seq.hot[k] = (r % 10) < 8
                         ? static_cast<std::uint32_t>(((r >> 8) % hot_count) * 100 % n)  // every 100th record
                         : static_cast<std::uint32_t>((r >> 8) % n);
    }

    Nsm nsm(n);
    Dsm dsm(n);
    Pax pax(n);
    fill(nsm, n, next);
    fill(dsm, n, next);
    fill(pax, n, next);

    // --pattern none builds everything and measures nothing: the setup-only
    // baseline for hardware-counter runs.
    const char* patterns[] = {"row_chain", "row_uniform", "row_hot",
                              "sym_price", "field_price", "scan_price"};
    const char* layouts[] = {"nsm", "dsm", "pax"};

    std::FILE* csv = nullptr;
    if (!csv_path.empty()) {
        std::FILE* probe = std::fopen(csv_path.c_str(), "r");
        const bool exists = probe != nullptr;
        if (probe != nullptr) std::fclose(probe);
        csv = std::fopen(csv_path.c_str(), "a");
        if (csv == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", csv_path.c_str());
            return 1;
        }
        if (!exists) {
            std::fputs("run_id,layout,pattern,records,table_bytes,ops,rep,ns_per_op,mops_per_s,"
                       "checksum,cpu\n",
                       csv);
        }
    }

    int rc = 0;
    for (const char* pattern : patterns) {
        if (only_pattern != "all" && only_pattern != pattern) continue;
        std::uint64_t reference_checksum = 0;
        bool have_reference = false;
        // Repetitions are interleaved across layouts (rep 0 of each layout, then
        // rep 1, ...) so slow drift of the machine affects all layouts alike.
        for (int rep = -1; rep < reps; ++rep) {  // rep -1 is an unrecorded warm-up
            for (const char* layout : layouts) {
                if (only_layout != "all" && only_layout != layout) continue;
                Measurement m{};
                std::size_t bytes = 0;
                if (std::strcmp(layout, "nsm") == 0) {
                    m = measure(nsm, pattern, n, ops, seq);
                    bytes = Nsm::bytes(n);
                } else if (std::strcmp(layout, "dsm") == 0) {
                    m = measure(dsm, pattern, n, ops, seq);
                    bytes = Dsm::bytes(n);
                } else {
                    m = measure(pax, pattern, n, ops, seq);
                    bytes = Pax::bytes(n);
                }
                // All layouts hold the same logical data and are driven by the
                // same sequence, so they must read exactly the same values.
                if (!have_reference) {
                    reference_checksum = m.checksum;
                    have_reference = true;
                } else if (m.checksum != reference_checksum) {
                    std::fprintf(stderr, "CHECKSUM MISMATCH: %s %s\n", layout, pattern);
                    rc = 1;
                }
                if (rep < 0) continue;
                std::printf("%-4s %-12s records=%zu rep=%d ns/op=%.3f\n", layout, pattern, n, rep,
                            m.ns_per_op);
                if (csv != nullptr) {
                    std::fprintf(csv, "%s,%s,%s,%zu,%zu,%zu,%d,%.4f,%.3f,%llu,%d\n",
                                 run_id.c_str(), layout, pattern, n, bytes, m.ops, rep,
                                 m.ns_per_op, 1000.0 / m.ns_per_op,
                                 static_cast<unsigned long long>(m.checksum), cpu);
                }
            }
        }
    }
    if (csv != nullptr) std::fclose(csv);
    return rc;
}
