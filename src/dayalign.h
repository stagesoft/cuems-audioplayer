// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Day alignment of engine start times (cuems-RELATIONS
// Plans/2026-10-25-mtc-day-align-24h-wrap.md, §3.1, §3.3 row 2 and §7.2).
//
// The engine's cue start reaches the player as either
//   - /offset <float ms> = -start, in the engine's own 24 h-extended MTC domain
//     (a released engine, or a player without the `dayalign` CAPS token), or
//   - /start_tod <int32 ms> = the start as MTC position modulo 24 h (phase 2).
// Neither is comparable with this process's mtcHead, which is extended by the
// wraps THIS process has seen. PlayerCore keeps the raw value and converts it
// into this process's day with MtcReceiver::toLocalDay() at first use, in the
// audio thread, once the head is authoritative. Pure helpers, tested in
// test/test_dayalign.cpp.

#ifndef DAYALIGN_H
#define DAYALIGN_H

#include <cmath>
#include <cstdint>

namespace dayalign {

constexpr int64_t DAY_MS = 86400000LL;
constexpr int64_t HOUR_MS = 3600000LL;

// Which message carried the start
enum class Wire : uint8_t { NONE, OFFSET_F, START_TOD };

// /start_tod: the value modulo DAY, in [0, DAY). A value out of range is
// normalised, not rejected (a rejected value is a dead cue); `changed` says
// whether a WARNING is due.
inline int64_t normalizeTod(int64_t v, bool* changed)
{
    int64_t r = v % DAY_MS;
    if (r < 0) r += DAY_MS;
    if (changed) *changed = (r != v);
    return r;
}

// /start_tod sent as a float (a lenient sender): floored first, then normalised.
inline int64_t todFromFloat(double f, bool* changed)
{
    return normalizeTod((int64_t)std::floor(f), changed);
}

// The engine time the raw value refers to (the cue's start), in the sender's
// domain: -offset for /offset, the time of day itself for /start_tod.
inline int64_t engineStartMs(Wire w, int64_t raw) { return w == Wire::OFFSET_F ? -raw : raw; }

// The player's offset in ns for a start resolved to `localStartMs` in this
// process's day: offset = -start. For /offset with k = 0 this is exactly
// floor(offset) * 1e6, the value the player always used.
inline int64_t offsetNsFor(int64_t localStartMs) { return -localStartMs * 1000000; }

// Is a k line a WARNING? Phase ext (/offset): a fresh process lagging the
// engine by whole days is expected (k >= 1); k < 0 has no explanation. Phase
// tod (/start_tod): any k is explained (k = minus this process's day count, or
// +1 at the wrap edge), so INFO.
inline bool realignIsWarning(Wire w, int64_t k) { return w == Wire::OFFSET_F && k < 0; }

}  // namespace dayalign

#endif  // DAYALIGN_H
