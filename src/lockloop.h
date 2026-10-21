// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Pure helpers of plan 869ejxng3 + 1amfwm7e6cw rev 3 (section 4.3a and
// 4.3d): the target arithmetic in ns, MTC liveness (the mtc variable of
// section 4.3h, h.1) and the filtered PI lock loop on the resampler's ratio.
// No I/O, no allocation, no clocks of their own: everything is fed in, so the
// simulation drives them with simulated time.
#ifndef LOCKLOOP_H
#define LOCKLOOP_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "qfseqlock.h"

namespace lockloop {

// target = round((mtc_now_ns + offset_ns + latency_ns) x rate / 1e9), 64-bit
// integer arithmetic in ns, x first then / with rounding (D6: nothing is
// truncated per ms). Input samples, absolute; may be negative.
inline int64_t targetSamples(int64_t mtcNowNs, int64_t offsetNs, int64_t latencyNs, unsigned rate)
{
    const __int128 x = (__int128)(mtcNowNs + offsetNs + latencyNs) * (__int128)rate;
    const __int128 g = 1000000000;
    return (int64_t)(x >= 0 ? (x + g / 2) / g : -((-x + g / 2) / g));
}

enum class Mtc { LIVE, SUSPENDED, LOST, INVALID };

inline const char* mtcName(Mtc m)
{
    switch (m) {
    case Mtc::LIVE: return "live";
    case Mtc::SUSPENDED: return "suspended";
    case Mtc::LOST: return "lost";
    case Mtc::INVALID: return "invalid";
    }
    return "?";
}

// MTC liveness (section 4.3a; h.1's mtc, E6-E9). Updated once per callback:
//  - LIVE: the last trusted (anchored) tick is <= 20 ms old;
//  - SUSPENDED: the head moves, the last trusted tick is older (the target is
//    extrapolated from it, the loop holds, no seek decision);
//  - LOST: the head (the atomic, advanced by every quarter frame, valid or
//    not) has not moved for 150 ms;
//  - INVALID: 800 ms without an anchored tick, counted from the later of the
//    last anchored tick and the moment the head resumed moving after a
//    freeze (or the player's start), while the head keeps moving (D11).
// LOST / INVALID -> LIVE on any anchored tick while the head moves (level-
// triggered, N1). The player starts in LOST (J16).
class MtcLiveness {
public:
    static constexpr int64_t LIVE_AGE_NS = 20000000;     // 20 ms
    static constexpr int64_t FROZEN_NS = 150000000;      // 150 ms
    static constexpr int64_t INVALID_NS = 800000000;     // 800 ms

    struct Edges {
        bool lost = false;      // E7 (entered LOST)
        bool invalid = false;   // E8 (entered INVALID: ERROR once)
        bool returned = false;  // E6 (LOST/INVALID -> LIVE)
        bool aged = false;      // E9 (LIVE <-> SUSPENDED)
    };

    void start(int64_t nowNs, long head)
    {
        state_ = Mtc::LOST;
        lastHead_ = head;
        lastMotionNs_ = nowNs - FROZEN_NS;   // not moving until it moves
        resumedNs_ = nowNs;
        lastAnchoredNs_ = INT64_MIN / 2;
        enteredNs_ = nowNs;
        haveTrusted_ = false;
        seenSeq_ = 0;
        started_ = true;
    }

    Edges update(int64_t nowNs, long head, const TickSample& t)
    {
        if (!started_) start(nowNs, head);
        Edges e;
        const bool frozenBefore = (nowNs - lastMotionNs_) >= FROZEN_NS;
        if (head != lastHead_) {
            if (frozenBefore) resumedNs_ = nowNs;   // the head resumed after a freeze
            lastMotionNs_ = nowNs;
            lastHead_ = head;
        }
        const bool frozen = (nowNs - lastMotionNs_) >= FROZEN_NS;
        bool freshAnchor = false;
        if (t.seq != seenSeq_) {
            seenSeq_ = t.seq;
            if (t.anchored) {
                trustedMonoNs_ = t.monoNs;
                trustedHeadMs_ = t.headMs;
                haveTrusted_ = true;
                lastAnchoredNs_ = t.monoNs;
                freshAnchor = true;
            }
        }
        if (frozen) {
            if (state_ != Mtc::LOST) {           // E7
                state_ = Mtc::LOST;
                enteredNs_ = nowNs;
                e.lost = true;
            }
            return e;
        }
        const int64_t since = std::max(lastAnchoredNs_, resumedNs_);
        if ((state_ == Mtc::LOST || state_ == Mtc::INVALID) && freshAnchor && t.monoNs >= enteredNs_ - LIVE_AGE_NS) {
            state_ = Mtc::LIVE;                  // E6
            e.returned = true;
        } else if (state_ != Mtc::INVALID && nowNs - since >= INVALID_NS) {
            state_ = Mtc::INVALID;               // E8
            enteredNs_ = nowNs;
            e.invalid = true;
            return e;
        }
        if (state_ == Mtc::LIVE || state_ == Mtc::SUSPENDED) {   // E9
            const Mtc s = (haveTrusted_ && nowNs - trustedMonoNs_ <= LIVE_AGE_NS) ? Mtc::LIVE : Mtc::SUSPENDED;
            if (s != state_) e.aged = true;
            state_ = s;
        }
        return e;
    }

    Mtc state() const { return state_; }
    bool live() const { return state_ == Mtc::LIVE || state_ == Mtc::SUSPENDED; }
    // MTC now (ns), extrapolated from the last trusted tick at rate 1
    int64_t mtcNowNs(int64_t nowNs) const
    {
        return (int64_t)trustedHeadMs_ * 1000000 + (nowNs - trustedMonoNs_);
    }
    int64_t trustedAgeNs(int64_t nowNs) const { return haveTrusted_ ? nowNs - trustedMonoNs_ : INT64_MAX; }
    int64_t invalidForNs(int64_t nowNs) const { return nowNs - std::max(lastAnchoredNs_, resumedNs_); }

private:
    Mtc state_ = Mtc::LOST;
    bool started_ = false;
    long lastHead_ = 0;
    int64_t lastMotionNs_ = 0;
    int64_t resumedNs_ = 0;
    int64_t lastAnchoredNs_ = 0;
    int64_t enteredNs_ = 0;
    int64_t trustedMonoNs_ = 0;
    long trustedHeadMs_ = 0;
    bool haveTrusted_ = false;
    uint64_t seenSeq_ = 0;
};

// The filtered PI lock loop (section 4.3d):
//   e_f  += (e_ms - e_f) x T / tau_f          first-order low-pass, tau_f = 1 s
//   I    += e_f x T                           not while u is clamped or slew-limited
//   u_raw = -(Kp e_f + Ki I)                  ppm
//   u     = clamp(u_raw, +-U_MAX), slew-limited to |du| <= dU per period
//   r     = r_nom (1 + u 1e-6)
// Lock state with hysteresis: locked when |e_f| < 5 ms for 2 s, unlocked when
// |e_f| > 10 ms.
struct LoopParams {
    double kp = 100.0;       // ppm / ms
    double ki = 2.5;         // ppm / (ms s)
    double tauF = 1.0;       // s
    double uMax = 3000.0;    // ppm (G9)
    double dU = 20.0;        // ppm per period
    double lockIn = 5.0, lockOut = 10.0, lockHold = 2.0;
};

class LockLoop {
public:
    explicit LockLoop(LoopParams p = LoopParams()) : p_(p) {}

    // One period with the loop running; returns true when the lock state changed.
    bool step(double eMs, double T)
    {
        ef_ += (eMs - ef_) * (T / p_.tauF);
        const double iTry = i_ + ef_ * T;
        const double uRaw = -(p_.kp * ef_ + p_.ki * iTry);
        double uC = std::max(-p_.uMax, std::min(p_.uMax, uRaw));
        const bool clamped = (uC != uRaw);
        bool slewed = false;
        if (std::fabs(uC - u_) > p_.dU) {
            uC = u_ + (uC > u_ ? p_.dU : -p_.dU);
            slewed = true;
        }
        if (!clamped && !slewed) i_ = iTry;
        if (clamped) clampHits_++;
        if (slewed) slewHits_++;
        u_ = uC;
        return lockStep(T);
    }
    // SUSPENDED: u frozen, I kept, no integration
    void hold() {}
    // a start (h.4 step 5): e_f <- 0, I kept
    void resetFilter() { ef_ = 0.0; belowFor_ = 0.0; }
    double ratio(double rNom) const { return rNom * (1.0 + u_ * 1e-6); }
    double u() const { return u_; }
    double ef() const { return ef_; }
    double integral() const { return i_; }
    bool locked() const { return locked_; }
    long clampHits() const { return clampHits_; }
    long slewHits() const { return slewHits_; }
    void resetCounters() { clampHits_ = slewHits_ = 0; }

private:
    bool lockStep(double T)
    {
        const bool before = locked_;
        if (std::fabs(ef_) < p_.lockIn) belowFor_ += T; else belowFor_ = 0.0;
        if (!locked_ && belowFor_ >= p_.lockHold) locked_ = true;
        if (locked_ && std::fabs(ef_) > p_.lockOut) locked_ = false;
        return locked_ != before;
    }
    LoopParams p_;
    double ef_ = 0.0, i_ = 0.0, u_ = 0.0, belowFor_ = 0.0;
    bool locked_ = false;
    long clampHits_ = 0, slewHits_ = 0;
};

}  // namespace lockloop

#endif  // LOCKLOOP_H
