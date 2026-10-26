/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * Day alignment of engine /offset values (24h MTC wrap).
 *
 * The engine sends /offset = -start in ITS 24h-extended MTC domain; this
 * process's mtcHead is extended by the wraps THIS process has seen. A player
 * spawned after the engine passed 24h (lleida-diposit, 2026-10-24) resolved the
 * start a day off and held silence with no log. Each /offset is now converted
 * into this process's day with MtcReceiver::toLocalDay() before use.
 *
 * Pure helpers, testable without JACK (test/test_dayalign.cpp). Plan:
 * cuems-RELATIONS Plans/2026-10-25-mtc-day-align-24h-wrap.md, step 2.
 */

#ifndef DAYALIGN_H
#define DAYALIGN_H

#include <atomic>
#include <string>
#include "mtcreceiver.h"

namespace DayAlign {

constexpr long long HOUR_MS = 3600000LL;

// One /offset as received on the OSC thread.
struct EngineOffset {
    long long rawMs = 0;        // floor()ed OSC float, engine domain (= -start)
    long long legacyBytes = 0;  // today's byte value, from the float expression
    long long latencyMs = 0;    // output latency the legacy value was built with
};

// Build it exactly as the player always did: (offsetOSC + latency) * ams is
// evaluated in float and truncated on the store, so k = 0 stays byte-identical.
inline EngineOffset fromOsc(float offsetOSC, long int latencyMs, unsigned int audioMillisecondSize) {
    EngineOffset o;
    o.rawMs = static_cast<long long>(offsetOSC);
    o.legacyBytes = static_cast<long int>((offsetOSC + latencyMs) * audioMillisecondSize);
    o.latencyMs = latencyMs;
    return o;
}

// The engine time the offset refers to (the cue's start).
inline long long engineStartMs(const EngineOffset& o) { return -o.rawMs; }

// headOffset in bytes for day shift k: localStart = start - k*DAY, i.e.
// offset + k*DAY. k = 0 returns the legacy bytes untouched; k != 0 is exact
// int64 (the float expression is what the museum runs aligned today).
inline long long localOffsetBytes(const EngineOffset& o, long k, unsigned int audioMillisecondSize) {
    if (k == 0) return o.legacyBytes;
    return (o.rawMs + static_cast<long long>(k) * MtcReceiver::DAY_MS + o.latencyMs)
           * static_cast<long long>(audioMillisecondSize);
}

// A resolved position more than an hour before the media start or past its end
// means a wrong day (or an engine time far from now): worth one WARNING.
inline bool implausibleSeek(long long seekBytes, unsigned long long fileBytes, unsigned int audioMillisecondSize) {
    const long long hourBytes = HOUR_MS * static_cast<long long>(audioMillisecondSize);
    return seekBytes < -hourBytes || seekBytes > static_cast<long long>(fileBytes) + hourBytes;
}

// Phase 1 (engine sends its extended domain): a fresh process lagging the
// engine by whole days is expected (k >= 1, INFO); k < 0 has no explanation.
inline bool realignIsWarning(long k) { return k < 0; }

inline std::string realignLine(long k, long long engineMs, long long headMs, unsigned long conversions) {
    return "MTC day realign k=" + std::to_string(k) + " (engine " + std::to_string(engineMs)
           + " ms, local head " + std::to_string(headMs) + " ms, phase=ext); "
           + std::to_string(conversions) + " conversions";
}

// Single-writer seqlock carrying the latest /offset from the OSC thread to the
// audio callback. Lock-free on both sides; the reader never blocks and simply
// tries again on its next callback when it races a write. Latest value wins.
class OffsetMailbox {
public:
    // OSC thread only.
    void publish(const EngineOffset& o) {
        const unsigned long s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);           // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        rawMs_.store(o.rawMs, std::memory_order_relaxed);
        legacyBytes_.store(o.legacyBytes, std::memory_order_relaxed);
        latencyMs_.store(o.latencyMs, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);           // even: stable
    }

    // A complete value newer than `appliedSeq`? Its sequence goes to `seq`.
    bool readNew(unsigned long appliedSeq, EngineOffset& o, unsigned long& seq) const {
        const unsigned long s1 = seq_.load(std::memory_order_acquire);
        if ((s1 & 1UL) || s1 == appliedSeq) return false;
        EngineOffset tmp;
        tmp.rawMs = rawMs_.load(std::memory_order_relaxed);
        tmp.legacyBytes = legacyBytes_.load(std::memory_order_relaxed);
        tmp.latencyMs = latencyMs_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;   // torn
        o = tmp;
        seq = s1;
        return true;
    }

private:
    std::atomic<unsigned long> seq_{0};     // 0 = nothing published yet
    std::atomic<long long> rawMs_{0};
    std::atomic<long long> legacyBytes_{0};
    std::atomic<long long> latencyMs_{0};
};

} // namespace DayAlign

#endif // DAYALIGN_H
