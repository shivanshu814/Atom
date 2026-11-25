// Atom benchmark: throughput and tail latency of every structure against its mutex baseline,
// swept over producer / consumer (or thread) counts. Writes one CSV row per run; the median
// of each configuration is printed as it goes. scripts/plot.py turns the CSV into charts.
//
//   atom_bench [--quick] [--runs N] [--load F] [--out results.csv] [--only spsc,mpsc,mpmc,stack]
//
// Method (docs/ATOM_DESIGN.md section 9):
//  - Throughput: producers push a fixed number of items flat out; items / elapsed time.
//  - Latency: producers push at a fixed total rate, a fraction (--load, default 0.25) of what
//    the slowest queue in that configuration sustained, so every queue is measured at the
//    same offered load. A run that cannot keep up with that rate is flagged. Each
//    item carries the time it was *scheduled* to be sent, not when it was sent, so a stalled
//    producer cannot hide the delay of the items it should have sent (coordinated omission).
//  - One warm-up run, then N measured runs; the median is reported.
//  - Every queue waits the same way when full or empty: Backoff (spin, then yield) in
//    throughput mode; pure spinning in latency mode. Threads never outnumber the performance
//    cores, and on macOS sched_yield() lowers the caller's priority for a while, which put
//    milliseconds into every queue's tail when an idle consumer kept yielding.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <pthread.h>
#endif

#include "atom/bounded_mpmc_queue.hpp"
#include "atom/config.hpp"
#include "atom/mpsc_queue.hpp"
#include "atom/mutex_queue.hpp"
#include "atom/reclaim/epoch.hpp"
#include "atom/reclaim/hazard_pointer.hpp"
#include "atom/spsc_queue.hpp"
#include "atom/treiber_stack.hpp"
#include "histogram.hpp"

namespace {

using atom::bench::Histogram;

// Latency timestamps are taken twice per item, so the clock read is on the measured path.
// On Apple Silicon steady_clock::now() costs about 16 ns and mach_absolute_time() about
// 6 ns (measured on the M1 Pro); both tick at 24 MHz, so neither resolves below ~41 ns.
std::uint64_t now_ns() {
#ifdef __APPLE__
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

// macOS has no thread pinning. The highest QoS class at least asks the scheduler for the
// performance cores.
void prefer_performance_cores() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

struct BenchItem {
    std::uint64_t seq = 0;
    std::uint64_t ts  = 0;  // scheduled send time (latency mode)
};

// Lines every thread up, then releases them together.
class StartGate {
public:
    void arrive_and_wait() {
        ready_.fetch_add(1, std::memory_order_release);
        while (!go_.load(std::memory_order_acquire)) atom::cpu_relax();
    }
    void open_when(unsigned n) {
        while (ready_.load(std::memory_order_acquire) < n) std::this_thread::yield();
        go_.store(true, std::memory_order_release);
    }

private:
    std::atomic<unsigned> ready_{0};
    std::atomic<bool>     go_{false};
};

template <class Q>
bool pop_counting(Q& q, BenchItem& out, std::uint64_t& retries) {
    if constexpr (requires { q.try_pop_ex(out); }) {
        const atom::PopResult r = q.try_pop_ex(out);
        if (r == atom::PopResult::Retry) ++retries;
        return r == atom::PopResult::Item;
    } else {
        return q.try_pop(out);
    }
}

// Written by every consumer, so it gets its own cache line, away from everything the
// producers read in their loop.
struct alignas(atom::kCacheLine) Counter {
    std::atomic<std::uint64_t> value{0};
};

struct RunResult {
    double        ops_per_sec = 0;
    bool          saturated   = false;  // latency mode: fell behind the offered rate
    std::uint64_t items       = 0;
    std::uint64_t retries     = 0;
    Histogram     hist;
};

// One run: P producers, C consumers, `total` items. rate == 0 is throughput mode; otherwise
// producers send at `rate` items/sec in total and consumers record latency.
template <class Q>
RunResult run_queue(const std::function<std::unique_ptr<Q>()>& make, unsigned P, unsigned C,
                    std::uint64_t total, double rate) {
    std::unique_ptr<Q>  q            = make();
    const std::uint64_t per_producer = total / P;
    total                            = per_producer * P;

    StartGate                  gate;
    std::atomic<std::uint64_t> start_ns{0};
    Counter                    consumed;
    std::vector<Histogram>     hists(C);
    std::vector<std::uint64_t> retries(C, 0), end_ns(C, 0);
    std::vector<std::thread>   threads;
    const double               interval = rate > 0 ? 1e9 / rate : 0;

    for (unsigned p = 0; p < P; ++p) {
        threads.emplace_back([&, p] {
            prefer_performance_cores();
            // Locals, so the loop reads nothing that lives on the main thread's stack.
            Q&                  queue = *q;
            const std::uint64_t n     = per_producer;
            const double        step  = interval;
            const bool          timed = rate > 0;
            atom::Backoff       backoff;
            gate.arrive_and_wait();
            const std::uint64_t t0 = start_ns.load(std::memory_order_acquire);
            for (std::uint64_t k = 0; k < n; ++k) {
                BenchItem it{k, 0};
                if (timed) {
                    const auto due = t0 + static_cast<std::uint64_t>(static_cast<double>(k * P + p) * step);
                    while (now_ns() < due) atom::cpu_relax();
                    it.ts = due;
                }
                while (!queue.try_push(it)) {
                    if (timed) atom::cpu_relax();
                    else backoff.pause();
                }
                backoff.reset();
            }
        });
    }
    for (unsigned c = 0; c < C; ++c) {
        threads.emplace_back([&, c] {
            prefer_performance_cores();
            Q&                  queue = *q;
            const std::uint64_t all   = total;
            const bool          timed = rate > 0;
            atom::Backoff       backoff;
            Histogram           hist;  // on this thread's stack, merged after the run
            std::uint64_t       local = 0, tries = 0;
            BenchItem           it;
            gate.arrive_and_wait();
            for (;;) {
                if (pop_counting(queue, it, tries)) {
                    if (timed) hist.record(now_ns() - it.ts);
                    // Batched so consumers do not fight over the counter on every item.
                    if (++local == 256) {
                        consumed.value.fetch_add(local, std::memory_order_relaxed);
                        local = 0;
                    }
                    backoff.reset();
                } else {
                    if (local != 0) {
                        consumed.value.fetch_add(local, std::memory_order_relaxed);
                        local = 0;
                    }
                    if (consumed.value.load(std::memory_order_relaxed) >= all) break;
                    if (timed) atom::cpu_relax();
                    else backoff.pause();
                }
            }
            end_ns[c]  = now_ns();
            retries[c] = tries;
            hists[c]   = hist;
        });
    }

    // In latency mode give the threads a moment to reach their spin loops before the first
    // scheduled send, so the first items do not start out late.
    start_ns.store(now_ns() + (rate > 0 ? 2'000'000 : 0), std::memory_order_release);
    gate.open_when(P + C);
    for (auto& t : threads) t.join();

    RunResult r;
    const std::uint64_t end = *std::max_element(end_ns.begin(), end_ns.end());
    r.items                 = total;
    r.ops_per_sec           = static_cast<double>(total) * 1e9 / static_cast<double>(end - start_ns.load());
    r.saturated             = rate > 0 && r.ops_per_sec < 0.95 * rate;
    for (unsigned c = 0; c < C; ++c) {
        r.hist.merge(hists[c]);
        r.retries += retries[c];
    }
    return r;
}

// A std::mutex around a vector: the baseline for the lock-free stacks.
class MutexStack {
public:
    void push(std::uint64_t v) {
        std::lock_guard lock(m_);
        items_.push_back(v);
    }
    std::optional<std::uint64_t> pop() {
        std::lock_guard lock(m_);
        if (items_.empty()) return std::nullopt;
        const std::uint64_t v = items_.back();
        items_.pop_back();
        return v;
    }

private:
    std::mutex                 m_;
    std::vector<std::uint64_t> items_;
};

// T threads, each doing push then pop `ops` times (50/50). Returns operations per second.
template <class S>
double run_stack(unsigned T, std::uint64_t ops) {
    S                        stack;
    StartGate                gate;
    std::vector<std::uint64_t> end_ns(T, 0);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < T; ++t) {
        threads.emplace_back([&, t] {
            prefer_performance_cores();
            gate.arrive_and_wait();
            for (std::uint64_t i = 0; i < ops; ++i) {
                stack.push(i);
                (void)stack.pop();
            }
            end_ns[t] = now_ns();
        });
    }
    const std::uint64_t start = now_ns();
    gate.open_when(T);
    for (auto& th : threads) th.join();
    const std::uint64_t end = *std::max_element(end_ns.begin(), end_ns.end());
    return static_cast<double>(2 * T * ops) * 1e9 / static_cast<double>(end - start);
}

// ---------------------------------------------------------------------------------------

struct Options {
    unsigned    runs  = 5;
    double      load  = 0.25;  // latency-mode rate, as a fraction of the slowest throughput
    bool        quick = false;
    std::string out   = "results.csv";
    std::string only;
    bool        enabled(const char* name) const { return only.empty() || only.find(name) != std::string::npos; }
    std::uint64_t scale(std::uint64_t n) const { return quick ? n / 10 : n; }
};

class Csv {
public:
    explicit Csv(const std::string& path) : f_(std::fopen(path.c_str(), "w")) {
        if (f_ == nullptr) {
            std::perror(path.c_str());
            std::exit(1);
        }
        std::fprintf(f_, "experiment,queue,mode,producers,consumers,capacity,run,items,ops_per_sec,"
                         "p50_ns,p99_ns,p999_ns,max_ns,target_rate,pop_retries,saturated\n");
    }
    ~Csv() { std::fclose(f_); }
    Csv(const Csv&)            = delete;
    Csv& operator=(const Csv&) = delete;

    void row(const char* exp, const std::string& queue, const char* mode, unsigned P, unsigned C,
             std::size_t cap, unsigned run, const RunResult& r, double rate) {
        const bool lat = rate > 0;
        std::fprintf(f_, "%s,%s,%s,%u,%u,%zu,%u,%llu,%.0f,%llu,%llu,%llu,%llu,%.0f,%llu,%d\n", exp,
                     queue.c_str(), mode, P, C, cap, run, static_cast<unsigned long long>(r.items),
                     r.ops_per_sec,
                     static_cast<unsigned long long>(lat ? r.hist.percentile(0.50) : 0),
                     static_cast<unsigned long long>(lat ? r.hist.percentile(0.99) : 0),
                     static_cast<unsigned long long>(lat ? r.hist.percentile(0.999) : 0),
                     static_cast<unsigned long long>(lat ? r.hist.max() : 0), rate,
                     static_cast<unsigned long long>(r.retries), r.saturated ? 1 : 0);
        std::fflush(f_);
    }

private:
    std::FILE* f_;
};

template <class T>
T median(std::vector<T> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// A queue under test: a name and a type-erased "run once" function.
struct Contender {
    std::string                                                              name;
    std::function<RunResult(unsigned, unsigned, std::uint64_t, double)> run;
};

template <class Q>
Contender contender(std::string name, std::size_t capacity) {
    std::function<std::unique_ptr<Q>()> make;
    if constexpr (std::is_constructible_v<Q, std::size_t>)
        make = [capacity] { return std::make_unique<Q>(capacity); };
    else
        make = [] { return std::make_unique<Q>(); };
    return {std::move(name), [make](unsigned P, unsigned C, std::uint64_t n, double rate) {
                return run_queue<Q>(make, P, C, n, rate);
            }};
}

// Throughput for every contender, then latency for every contender at half the slowest
// one's median throughput.
void sweep_config(Csv& csv, const Options& opt, const char* exp, std::vector<Contender>& cs,
                  unsigned P, unsigned C, std::size_t cap, std::uint64_t items) {
    double slowest = 1e18;
    for (Contender& c : cs) {
        (void)c.run(P, C, items / 4, 0);  // warm-up
        std::vector<double> tp;
        for (unsigned run = 0; run < opt.runs; ++run) {
            const RunResult r = c.run(P, C, items, 0);
            csv.row(exp, c.name, "throughput", P, C, cap, run, r, 0);
            tp.push_back(r.ops_per_sec);
        }
        const double med = median(tp);
        slowest          = std::min(slowest, med);
        std::printf("%-6s %-16s P=%u C=%u cap=%-6zu throughput %8.2f M/s  (min %.2f, max %.2f)\n", exp,
                    c.name.c_str(), P, C, cap, med / 1e6, *std::min_element(tp.begin(), tp.end()) / 1e6,
                    *std::max_element(tp.begin(), tp.end()) / 1e6);
    }

    const double        rate      = slowest * opt.load;
    const std::uint64_t lat_items = std::clamp<std::uint64_t>(static_cast<std::uint64_t>(rate * 0.25),
                                                              opt.scale(200'000), 2'000'000);
    for (Contender& c : cs) {
        std::vector<std::uint64_t> p50, p99, p999;
        std::uint64_t              retries = 0;
        unsigned                   saturated = 0;
        for (unsigned run = 0; run < opt.runs; ++run) {
            const RunResult r = c.run(P, C, lat_items, rate);
            csv.row(exp, c.name, "latency", P, C, cap, run, r, rate);
            p50.push_back(r.hist.percentile(0.50));
            p99.push_back(r.hist.percentile(0.99));
            p999.push_back(r.hist.percentile(0.999));
            retries += r.retries;
            saturated += r.saturated ? 1 : 0;
        }
        std::printf("%-6s %-16s P=%u C=%u cap=%-6zu latency @ %.2f M/s  p50 %6llu  p99 %7llu  p99.9 %8llu ns",
                    exp, c.name.c_str(), P, C, cap, rate / 1e6,
                    static_cast<unsigned long long>(median(p50)), static_cast<unsigned long long>(median(p99)),
                    static_cast<unsigned long long>(median(p999)));
        if (retries != 0) std::printf("  (Retry polls: %llu)", static_cast<unsigned long long>(retries));
        if (saturated != 0) std::printf("  SATURATED in %u/%u runs", saturated, opt.runs);
        std::printf("\n");
    }
}

void bench_spsc(Csv& csv, const Options& opt) {
    for (std::size_t cap : {std::size_t{1024}, std::size_t{65536}}) {
        std::vector<Contender> cs{
            contender<atom::MutexQueue<BenchItem>>("MutexQueue", cap),
            contender<atom::NaiveSpscQueue<BenchItem>>("NaiveSpscQueue", cap),
            contender<atom::SpscQueue<BenchItem>>("SpscQueue", cap),
        };
        sweep_config(csv, opt, "spsc", cs, 1, 1, cap, opt.scale(20'000'000));
    }
}

void bench_mpsc(Csv& csv, const Options& opt) {
    constexpr std::size_t cap = 65536;
    for (unsigned P : {1u, 2u, 4u, 6u, 7u}) {
        std::vector<Contender> cs{
            contender<atom::MutexQueue<BenchItem>>("MutexQueue", cap),
            contender<atom::MpscQueue<BenchItem>>("MpscQueue", cap),
            contender<atom::BoundedMpmcQueue<BenchItem>>("BoundedMpmcQueue", cap),
        };
        sweep_config(csv, opt, "mpsc", cs, P, 1, cap, opt.scale(4'000'000));
    }
}

void bench_mpmc(Csv& csv, const Options& opt) {
    constexpr std::size_t cap = 65536;
    for (unsigned P : {1u, 2u, 4u}) {
        for (unsigned C : {1u, 2u, 4u}) {
            std::vector<Contender> cs{
                contender<atom::MutexQueue<BenchItem>>("MutexQueue", cap),
                contender<atom::BoundedMpmcQueue<BenchItem>>("BoundedMpmcQueue", cap),
            };
            sweep_config(csv, opt, "mpmc", cs, P, C, cap, opt.scale(4'000'000));
        }
    }
}

void bench_stack(Csv& csv, const Options& opt) {
    using Hp  = atom::TreiberStack<std::uint64_t, atom::HazardPointers>;
    using Ebr = atom::TreiberStack<std::uint64_t, atom::EpochReclaim>;
    struct S {
        const char* name;
        double (*run)(unsigned, std::uint64_t);
    };
    const S stacks[] = {{"MutexStack", &run_stack<MutexStack>},
                        {"Treiber+HP", &run_stack<Hp>},
                        {"Treiber+EBR", &run_stack<Ebr>}};
    for (unsigned T : {1u, 2u, 4u, 8u}) {
        const std::uint64_t ops = opt.scale(1'000'000) / T;
        for (const S& s : stacks) {
            (void)s.run(T, ops / 4);  // warm-up
            std::vector<double> tp;
            for (unsigned run = 0; run < opt.runs; ++run) {
                RunResult r;
                r.ops_per_sec = s.run(T, ops);
                r.items       = 2 * T * ops;
                csv.row("stack", s.name, "throughput", T, T, 0, run, r, 0);
                tp.push_back(r.ops_per_sec);
            }
            std::printf("stack  %-16s T=%u  throughput %8.2f M ops/s\n", s.name, T, median(tp) / 1e6);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") {
            opt.quick = true;
            opt.runs  = 3;
        } else if (a == "--runs" && i + 1 < argc) {
            opt.runs = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (a == "--load" && i + 1 < argc) {
            opt.load = std::stod(argv[++i]);
        } else if (a == "--out" && i + 1 < argc) {
            opt.out = argv[++i];
        } else if (a == "--only" && i + 1 < argc) {
            opt.only = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--quick] [--runs N] [--load F] [--out file.csv] [--only spsc,mpsc,mpmc,stack]\n", argv[0]);
            return 2;
        }
    }
    prefer_performance_cores();
    std::printf("threads available: %u, runs per point: %u%s\n", std::thread::hardware_concurrency(),
                opt.runs, opt.quick ? " (quick)" : "");

    Csv csv(opt.out);
    if (opt.enabled("spsc")) bench_spsc(csv, opt);
    if (opt.enabled("mpsc")) bench_mpsc(csv, opt);
    if (opt.enabled("mpmc")) bench_mpmc(csv, opt);
    if (opt.enabled("stack")) bench_stack(csv, opt);
    std::printf("wrote %s\n", opt.out.c_str());
}
