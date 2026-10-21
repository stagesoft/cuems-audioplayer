// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Test-build knobs of plan 869ejxng3 + 1amfwm7e6cw rev 3 (section 4.3e,
// I16c), compiled in only when CUEMS_TEST_KNOBS is defined (cmake
// -DCUEMS_TEST_BUILD=ON); the release build has none of this.
//   HF_LOG=<path>       per period (mono_ns, mtc_now_ns, age, state, pos, e_f,
//                       u, fifo, callback us) and per tick (mono_ns, head_ms,
//                       complete, run, anchored), streamed to <path> every
//                       second by the main loop (the engine stops players with
//                       /quit + SIGKILL, so a dump at exit is never written, H13)
//   HF_DROP_LOOP=<n>    drop the first n /loop datagrams (the lost-/loop row)
//   HF_FAIL_WRAP_SEEK=1 every WRAP job fails (the off-ack row)
// Each knob is announced with a WARNING at start.
#ifndef TESTKNOBS_H
#define TESTKNOBS_H

#ifdef CUEMS_TEST_KNOBS

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "rtlog.h"

namespace testknobs {

struct PeriodRec {
    int64_t monoNs, mtcNowNs, ageNs;
    int state;
    double pos, ef, u, fifo;
    int64_t callbackUs;
};
struct TickRec {
    int64_t monoNs;
    long headMs;
    int complete, run, anchored;
};

struct Knobs {
    std::string logPath;
    int dropLoop = 0;
    bool failWrap = false;
    FILE* f = nullptr;
    SpscRing<PeriodRec, 8192> periods;   // audio thread -> main loop
    SpscRing<TickRec, 8192> ticks;       // MIDI thread -> main loop
    std::atomic<int> loopsDropped{0};

    void readEnv()
    {
        if (const char* p = std::getenv("HF_LOG")) logPath = p;
        if (const char* p = std::getenv("HF_DROP_LOOP")) dropLoop = std::atoi(p);
        if (const char* p = std::getenv("HF_FAIL_WRAP_SEEK")) failWrap = std::atoi(p) != 0;
        if (!logPath.empty()) f = std::fopen(logPath.c_str(), "w");
    }
    // main loop, every second
    void flush()
    {
        if (!f) return;
        PeriodRec p;
        while (periods.pop(p))
            std::fprintf(f, "period,%lld,%lld,%lld,%d,%.3f,%.4f,%.3f,%.2f,%lld\n", (long long)p.monoNs,
                         (long long)p.mtcNowNs, (long long)p.ageNs, p.state, p.pos, p.ef, p.u, p.fifo,
                         (long long)p.callbackUs);
        TickRec t;
        while (ticks.pop(t))
            std::fprintf(f, "tick,%lld,%ld,%d,%d,%d\n", (long long)t.monoNs, t.headMs, t.complete, t.run, t.anchored);
        std::fflush(f);
    }
};

inline Knobs& knobs()
{
    static Knobs k;
    return k;
}

}  // namespace testknobs

#endif  // CUEMS_TEST_KNOBS
#endif  // TESTKNOBS_H
