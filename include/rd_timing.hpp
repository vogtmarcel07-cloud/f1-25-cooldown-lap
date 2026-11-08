#pragma once
// Time spent in a module's own hooks (the launcher's performance report does not count addon wraps): every hook that
// runs per frame or per wheel adds its time here, and once a minute the module logs ms per second and calls per
// second of each, e.g. "[rd-perf] wt: weather update 0.42 ms/s (60/s), surface query 1.10 ms/s (5200/s)".
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <swofty/mixin.hpp>

namespace rdtime {

struct Acc {
    const char *name;
    std::atomic<uint64_t> ticks{0}, calls{0};
    Acc *next = nullptr;
    explicit Acc(const char *n);
};

struct Registry {
    Acc *head = nullptr;
    std::atomic<uint64_t> last_report{0};
    const char *module = "rd";
};

inline Registry &registry()
{
    static Registry r;
    return r;
}

inline Acc::Acc(const char *n) : name(n)
{
    next = registry().head;
    registry().head = this;
}

inline double freq()
{
    static double f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return double(q.QuadPart);
    }();
    return f;
}

inline void maybe_report()
{
    Registry &r = registry();
    uint64_t now = GetTickCount64(), last = r.last_report.load(std::memory_order_relaxed);
    if (last == 0) {
        r.last_report.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 60000 || !r.last_report.compare_exchange_strong(last, now))
        return;
    double secs = double(now - last) / 1000.0;
    char buf[900];
    int len = 0;
    for (Acc *a = r.head; a && len < int(sizeof buf) - 80; a = a->next) {
        uint64_t t = a->ticks.exchange(0), c = a->calls.exchange(0);
        len += std::snprintf(buf + len, sizeof buf - len, "%s%s %.2f ms/s (%.0f/s)", len ? ", " : "", a->name,
                             double(t) / freq() * 1000.0 / secs, double(c) / secs);
    }
    swofty::log("[rd-perf] %s: %s", r.module, buf);
}

struct Scope {
    Acc &acc;
    LARGE_INTEGER t0;
    explicit Scope(Acc &a) : acc(a) { QueryPerformanceCounter(&t0); }
    ~Scope()
    {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        acc.ticks.fetch_add(uint64_t(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
        acc.calls.fetch_add(1, std::memory_order_relaxed);
        maybe_report();
    }
};

}

#define RD_TIMED_CAT2(a, b) a##b
#define RD_TIMED_CAT(a, b) RD_TIMED_CAT2(a, b)
// Times the rest of the enclosing block under `label`.
#define RD_TIMED(label)                                                                                               \
    static ::rdtime::Acc RD_TIMED_CAT(s_rd_acc_, __LINE__){label};                                                    \
    ::rdtime::Scope RD_TIMED_CAT(s_rd_scope_, __LINE__){RD_TIMED_CAT(s_rd_acc_, __LINE__)}
