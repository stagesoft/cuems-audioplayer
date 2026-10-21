// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// See playercore.h. Cell names (E2, E10, ...) are those of plan 869ejxng3 +
// 1amfwm7e6cw rev 3, section 4.3h v2.1, h.3.

#include "playercore.h"
#include <cmath>
#include <cstdio>

using lockloop::Mtc;
using A = AudioFstream;

namespace {
const int64_t INF_SAMPLES = INT64_MAX / 4;
const double FIFO_STEADY = 241.0, FIFO_BAND = 256.0;   // section 4.3a, FIFO health
const int SETTLE_PERIODS = 3;
const double RESYNC_MS = 80.0;                          // D3
const int RESYNC_PERIODS = 3;
}  // namespace

const char* PlayerCore::modeName(A::LoopMode m)
{
    switch (m) {
    case A::LoopMode::PENDING: return "pending";
    case A::LoopMode::ONCE: return "once";
    case A::LoopMode::WRAP: return "wrap";
    case A::LoopMode::OFF: return "off";
    }
    return "?";
}

PlayerCore::PlayerCore(AudioFstream& engine, unsigned outRate, unsigned periodFrames)
    : engine_(engine), outRate_(outRate), period_(periodFrames)
{
    rNom_ = engine_.getSampleRate() > 0 && outRate_ > 0 ? (double)engine_.getSampleRate() / outRate_ : 1.0;
    T_ = outRate_ > 0 ? (double)period_ / outRate_ : 0.0;
    modeWord_.store((uint64_t)A::LoopMode::PENDING);
}

std::string PlayerCore::loopAckLine(const std::string& slug, int passes, int num, int den) const
{
    // the mode and its version, read together (one 64-bit atomic, J11, K12)
    const uint64_t w = modeWord_.load();
    char buf[256];
    snprintf(buf, sizeof buf, "[%s] [OK] LOOP %llu %d %d %d %s\n", slug.c_str(),
             (unsigned long long)(w >> 8), passes, num, den, modeName((A::LoopMode)(w & 0xff)));
    return buf;
}

void PlayerCore::push(LogEvent::Type t, int64_t a, int64_t b, int64_t c, double x, double y, double z, int kind)
{
    LogEvent e;
    e.type = t; e.a = a; e.b = b; e.c = c; e.x = x; e.y = y; e.z = z; e.kind = kind;
    log_.push(e);
}

int64_t PlayerCore::computeTarget(int64_t nowNs) const
{
    return lockloop::targetSamples(live_.mtcNowNs(nowNs), offsetNs_, latencyNs_, engine_.getSampleRate());
}

int64_t PlayerCore::endEff() const
{
    const int passes = engine_.loopPassesValue();
    const int64_t p = engine_.periodForStart();
    if (passes <= 0 || p >= INF_SAMPLES) return INF_SAMPLES;
    return (int64_t)std::max<int64_t>(passes, engine_.passIndex() + 1) * p;
}

// h.3 cells of the OSC events (E2-E5, E25, E26); E20 and E19 are the OSC
// thread's and the main loop's.
void PlayerCore::applyOsc(const OscEvent& e, const A::LiveCtx& ctx)
{
    const A::Ps ps = engine_.ps();
    switch (e.type) {
    case OscEvent::MTCFOLLOW:
        if (e.a != 0) {                                   // E2
            engine_.clearSeekFailed();
            followingMtc_ = true;                         // in every column (T4)
            lastKind_ = A::StartKind::REVEAL;
            if (ps == A::Ps::PASTEND) {
                if (engine_.reenable(ctx.target)) lastKind_ = A::StartKind::REENABLE;
            }
        } else {                                          // E3
            followingMtc_ = false;
            engine_.stopOutput();
        }
        break;
    case OscEvent::OFFSET_F:
    case OscEvent::OFFSET_I: {                            // E4
        offsetNs_ = (e.type == OscEvent::OFFSET_F) ? (int64_t)std::floor(e.f) * 1000000
                                                   : (int64_t)e.a * 1000000;
        engine_.clearSeekFailed();
        lastKind_ = A::StartKind::OFFSET;
        if (!live_.live()) break;                         // HOLD by E7/E8: the condition starts it later
        target_ = computeTarget(lastNowNs_);
        if (ps == A::Ps::PLAY || ps == A::Ps::SEEKING) {
            engine_.offsetChanged(target_);
        } else if (ps == A::Ps::PASTEND) {
            if (engine_.reenable(target_)) lastKind_ = A::StartKind::REENABLE;
        }
        break;
    }
    case OscEvent::LOOP: {                                // E5
        const int passes = e.nargs >= 1 ? e.a : 1;
        const int num = e.nargs >= 3 ? e.b : 0;
        const int den = e.nargs >= 3 ? e.c : 0;
        lastLoopArgs_[0] = passes; lastLoopArgs_[1] = num; lastLoopArgs_[2] = den;
        engine_.setLoop(passes, num, den);
        if (ps == A::Ps::PASTEND && live_.live()) {
            if (engine_.reenable(ctx.target)) lastKind_ = A::StartKind::REENABLE;
        }
        break;
    }
    case OscEvent::PLAY_STOP:                             // E25
        if (!playStopWarned_) { playStopWarned_ = true; push(LogEvent::PLAY_STOP_REFUSED); }
        break;
    case OscEvent::STOPONLOST:                            // E26
        if (!stopOnLostWarned_) { stopOnLostWarned_ = true; push(LogEvent::STOPONLOST_IGNORED, e.a); }
        break;
    }
}

void PlayerCore::drainEngineEvents()
{
    A::FeedEvent ev[A::MAX_EVENTS];
    int n;
    while ((n = engine_.takeEvents(ev, A::MAX_EVENTS)) > 0) {
        for (int i = 0; i < n; i++) {
            const A::FeedEvent& f = ev[i];
            switch (f.type) {
            case A::EvType::CACHE_START:
            case A::EvType::SEEK_PICKUP:
                // every start: SETTLE, the E10 counter reset, e_f <- 0 (h.4 step 5)
                settle_ = SETTLE_PERIODS;
                e10_ = 0;
                loop_.resetFilter();
                starts_++;
                push(f.type == A::EvType::CACHE_START ? LogEvent::CACHE_START : LogEvent::SEEK_PICKUP,
                     f.a, f.b, 0, 0, 0, 0, f.kind);
                break;
            case A::EvType::HANDOVER_LATE: push(LogEvent::HANDOVER_LATE, f.a, f.b); break;
            case A::EvType::END: push(LogEvent::END, f.a); break;
            case A::EvType::SEEK_FAILED: push(LogEvent::SEEK_FAILED, f.a); break;
            case A::EvType::WRAP_FAILED: push(LogEvent::WRAP_FAILED); break;
            case A::EvType::RESET_FAILED: push(LogEvent::RESET_FAILED); break;
            case A::EvType::LP_MISMATCH: push(LogEvent::LP_MISMATCH, f.a, f.b); break;
            case A::EvType::TINY_LOOP: push(LogEvent::TINY_LOOP, f.a, f.b); break;
            case A::EvType::HEADER_SHORT: push(LogEvent::HEADER_SHORT, f.a, f.b); break;
            case A::EvType::INVARIANT: push(LogEvent::INVARIANT, f.a); break;
            case A::EvType::PAST_END_START: push(LogEvent::PAST_END_START, f.a); break;
            default: break;
            }
        }
    }
}

void PlayerCore::callback(float* out, unsigned frames, int64_t nowNs, long headMs, const TickSample& tick)
{
    lastNowNs_ = nowNs;
    periods_++;
    // 1. MTC events (E6-E9)
    const lockloop::MtcLiveness::Edges ed = live_.update(nowNs, headMs, tick);
    if (ed.lost || ed.invalid) {
        engine_.stopOutput();                             // E7 / E8: silence, u frozen, I kept
        if (ed.lost) { lostCount_++; push(LogEvent::MTC_LOST, (int64_t)(target_)); invalidLogged_ = false; }
        if (ed.invalid && !invalidLogged_) {
            invalidLogged_ = true;
            invalidErrors_++;
            push(LogEvent::MTC_INVALID, live_.invalidForNs(nowNs) / 1000000);
        }
    }
    if (ed.returned) {
        lastKind_ = A::StartKind::RETURN;
        invalidLogged_ = false;
        push(LogEvent::MTC_RETURN);
    }
    if (live_.state() == Mtc::SUSPENDED) suspendedPeriods_++;
    if (live_.live()) target_ = computeTarget(nowNs);
    A::LiveCtx ctx;
    ctx.target = target_;
    ctx.mtcLive = live_.live();

    // 2. the worker's hand-back (E15-E17)
    engine_.serviceWorker(ctx);

    // 3. the latched OSC events, in arrival order
    OscEvent oe;
    while (osc_.pop(oe)) {
        applyOsc(oe, ctx);
        ctx.target = target_;
    }

    // 4. the start condition (h.3)
    if (engine_.ps() == A::Ps::HOLD && followingMtc_ && live_.live() && target_ >= 0 &&
        !engine_.seekFailedLatched()) {
        engine_.start(target_, lastKind_);
    }

    // 5. the lock loop and the feed / output events (E10, E11, E22)
    if (engine_.ps() == A::Ps::PLAY && engine_.outPlaying()) {
        const double eMs = (engine_.pos() - (double)target_) * 1000.0 / engine_.getSampleRate();
        lastEMs_ = eMs;
        if (settle_ > 0) {
            settle_--;                                    // SETTLE: the loop off
        } else if (live_.state() == Mtc::LIVE) {
            if (std::fabs(eMs) > RESYNC_MS) {
                if (++e10_ >= RESYNC_PERIODS) {           // E10
                    e10_ = 0;
                    resyncs_++;
                    push(LogEvent::RESYNC, (int64_t)std::llround(eMs), target_ * 1000 / engine_.getSampleRate());
                    engine_.start(target_, A::StartKind::CORRECTION);
                }
            } else {
                e10_ = 0;
                if (loop_.step(eMs, T_)) push(loop_.locked() ? LogEvent::LOCKED : LogEvent::UNLOCKED, 0, 0, 0, loop_.ef());
            }
        } else {
            loop_.hold();                                 // SUSPENDED: u frozen, I kept
        }
        if (engine_.ps() == A::Ps::PLAY && live_.live() &&
            std::fabs(engine_.fifo() - FIFO_STEADY) > FIFO_BAND) {   // E11
            if (!fifoErrorOnce_) { fifoErrorOnce_ = true; push(LogEvent::FIFO_DRIFT, (int64_t)engine_.fifo()); }
            engine_.start(target_, A::StartKind::FIFO);
        }
        const int64_t ee = endEff();
        if (engine_.ps() == A::Ps::PLAY && live_.state() == Mtc::LIVE && ee < INF_SAMPLES &&
            target_ > ee + (int64_t)engine_.getSampleRate() * 80 / 1000) {   // E22
            if (!safetyOnce_) { safetyOnce_ = true; push(LogEvent::SAFETY_END, target_, ee); }
            engine_.endNow();
        }
    }
    engine_.process(out, frames, loop_.ratio(rNom_), ctx);
    drainEngineEvents();

    // the loop mode and its version: a change is a MODE event for the main loop's ack line
    const A::LoopMode m = engine_.loopMode();
    if (m != lastMode_) {
        lastMode_ = m;
        const uint64_t ver = (modeWord_.load() >> 8) + 1;
        modeWord_.store((ver << 8) | (uint64_t)m);
        push(LogEvent::MODE, (int64_t)ver, lastLoopArgs_[0], lastLoopArgs_[1], lastLoopArgs_[2], 0, 0, (int)m);
    }

    // E21's -w timer: from the end of the drain, as today
    if (engine_.ps() == A::Ps::PASTEND && engine_.feed() == A::Feed::ENDED) {
        const long w = endWaitMs_.load();
        if (endSinceNs_ == 0) {
            endSinceNs_ = nowNs;
            push(LogEvent::END_WAIT, w);
        }
        if (w != __LONG_MAX__ && (nowNs - endSinceNs_) / 1000000 >= w) endOfPlay_.store(true);
    } else {
        endSinceNs_ = 0;
    }

    // a DEBUG line every 10 s (the drift row)
    if (nowNs - lastStatsNs_ >= 10000000000LL) {
        lastStatsNs_ = nowNs;
        push(LogEvent::STATS, engine_.passIndex(), loop_.slewHits(), loop_.clampHits(), loop_.ef(), loop_.u(),
             engine_.fifo(), (int)live_.state());
    }
}

std::string PlayerCore::describe(const LogEvent& e, const std::string& slug, bool* stdoutLine, int* level)
{
    // level: 3 error, 4 warning, 6 info, 7 debug
    char b[320];
    *stdoutLine = false;
    *level = 6;
    static const char* kinds[] = {"reveal", "offset", "return", "correction", "fifo", "re-enable"};
    const char* kind = (e.kind >= 0 && e.kind < 6) ? kinds[e.kind] : "?";
    switch (e.type) {
    case LogEvent::CACHE_START:
        snprintf(b, sizeof b, "cache start at %lld ms (%s) origin=%lld", (long long)(e.a / 48), kind, (long long)e.b);
        break;
    case LogEvent::SEEK_PICKUP:
        snprintf(b, sizeof b, "seek to %lld ms (%s) done, origin=%lld", (long long)(e.a / 48), kind, (long long)e.b);
        break;
    case LogEvent::RESYNC:
        snprintf(b, sizeof b, "resync \xce\x94=%lld ms at %lld ms", (long long)e.a, (long long)e.b);
        break;
    case LogEvent::HANDOVER_LATE:
        *level = 3;
        if (e.b) snprintf(b, sizeof b, "handover late by %lld ms", (long long)e.a);
        else snprintf(b, sizeof b, "handover late at cursor %lld", (long long)e.a);
        break;
    case LogEvent::LOCKED: snprintf(b, sizeof b, "lock: locked (e_f %.2f ms)", e.x); break;
    case LogEvent::UNLOCKED: snprintf(b, sizeof b, "lock: unlocked (e_f %.2f ms)", e.x); break;
    case LogEvent::MTC_LOST: snprintf(b, sizeof b, "MTC lost (head frozen 150 ms)"); break;
    case LogEvent::MTC_RETURN: snprintf(b, sizeof b, "MTC returned"); break;
    case LogEvent::MTC_INVALID:
        *level = 3;
        snprintf(b, sizeof b, "MTC quarter frames invalid for %lld ms (doubled rtpmidid subscription?)", (long long)e.a);
        break;
    case LogEvent::FIFO_DRIFT: *level = 3; snprintf(b, sizeof b, "soxr input fifo drifting: %lld frames", (long long)e.a); break;
    case LogEvent::LP_MISMATCH:
        *level = 3;
        snprintf(b, sizeof b, "loop period differs from decoded length: P=%lld L=%lld", (long long)e.a, (long long)e.b);
        break;
    case LogEvent::MODE:
        *stdoutLine = true;
        snprintf(b, sizeof b, "[%s] [OK] LOOP %lld %lld %lld %lld %s\n", slug.c_str(), (long long)e.a,
                 (long long)e.b, (long long)e.c, (long long)e.x, modeName((A::LoopMode)e.kind));
        break;
    case LogEvent::SEEK_FAILED: *level = 3; snprintf(b, sizeof b, "seek failed (attempt %lld)", (long long)e.a); break;
    case LogEvent::WRAP_FAILED: *level = 3; snprintf(b, sizeof b, "wrap seek failed: looping off for this file"); break;
    case LogEvent::RESET_FAILED: *level = 3; snprintf(b, sizeof b, "reset failed"); break;
    case LogEvent::TINY_LOOP:
        *level = 3;
        snprintf(b, sizeof b, "loop period %lld samples below the minimum %lld: looping off", (long long)e.a, (long long)e.b);
        break;
    case LogEvent::HEADER_SHORT:
        *level = 4;
        snprintf(b, sizeof b, "loop period %lld more than 1 s short of the header %lld", (long long)e.a, (long long)e.b);
        break;
    case LogEvent::END: snprintf(b, sizeof b, "end of the cue: draining"); break;
    case LogEvent::END_WAIT: snprintf(b, sizeof b, "Out of file boundaries, waiting %lld ms", (long long)e.a); break;
    case LogEvent::SAFETY_END:
        snprintf(b, sizeof b, "safety-net end: target %lld past the end %lld + 80 ms", (long long)e.a, (long long)e.b);
        break;
    case LogEvent::STATS:
        *level = 7;
        snprintf(b, sizeof b, "e_f %.2f ms u %.1f ppm fifo %.1f passIndex %lld slew-limit hits %lld clamp hits %lld mtc %s",
                 e.x, e.y, e.z, (long long)e.a, (long long)e.b, (long long)e.c, lockloop::mtcName((Mtc)e.kind));
        break;
    case LogEvent::PLAY_STOP_REFUSED: *level = 4; snprintf(b, sizeof b, "/play and /stop are refused (MTC drives the player)"); break;
    case LogEvent::STOPONLOST_IGNORED:
        *level = 4;
        snprintf(b, sizeof b, "/stoponlost %lld ignored in this release (silence on MTC loss)", (long long)e.a);
        break;
    case LogEvent::INVARIANT: *level = 3; snprintf(b, sizeof b, "feed invariant violated (code %lld)", (long long)e.a); break;
    case LogEvent::PAST_END_START: snprintf(b, sizeof b, "start past the end (target %lld)", (long long)e.a); break;
    }
    return b;
}
