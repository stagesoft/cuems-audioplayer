// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// The audio callback's logic of plan 869ejxng3 + 1amfwm7e6cw rev 3, step 2
// (section 4.3a, 4.3d, 4.3e and the callback / OSC side of the normative
// 4.3h v2.1), without JACK: AudioPlayer::audioCallback calls callback() with
// the monotonic time, the atomic MTC head and the seqlock's tick sample; the
// simulation calls it with simulated ones. Owns MTC liveness, followingMtc,
// the offset, the lock loop, SETTLE, E10 / E11 / E22, the start condition,
// the latched OSC events (the latching rule of h.1), the loop mode and its
// version (the LOOP ack lines of section 4.2), the journal ring (4.3e) and
// the -w exit timer (E21). The feed itself is AudioFstream's (step 1).
// The cue start (/offset or /start_tod) is converted into this process's
// MTC day at first use (day alignment, cuems-RELATIONS
// Plans/2026-10-25-mtc-day-align-24h-wrap.md §3.3 row 2 and §7; dayalign.h).
#ifndef PLAYERCORE_H
#define PLAYERCORE_H

#include <atomic>
#include <cstdint>
#include <string>
#include "audiofstream.h"
#include "dayalign.h"
#include "lockloop.h"
#include "qfseqlock.h"
#include "rtlog.h"

// An OSC event latched by the OSC thread, applied at the top of the next
// callback in arrival order, never coalesced (h.1, N13).
struct OscEvent {
    // START_TOD: a = the start as MTC position modulo 24 h, already normalised
    // into [0, DAY) by the OSC thread (day alignment §7.2)
    enum Type : uint8_t { MTCFOLLOW, OFFSET_F, START_TOD, LOOP, PLAY_STOP, STOPONLOST } type = MTCFOLLOW;
    int32_t a = 0, b = 0, c = 0;
    int nargs = 0;
    double f = 0.0;
};

// A journal event pushed by the audio thread, written by the main loop.
struct LogEvent {
    enum Type : uint8_t {
        CACHE_START, SEEK_PICKUP, RESYNC, HANDOVER_LATE, LOCKED, UNLOCKED, MTC_LOST, MTC_RETURN,
        MTC_INVALID, FIFO_DRIFT, LP_MISMATCH, MODE, SEEK_FAILED, WRAP_FAILED, RESET_FAILED,
        TINY_LOOP, HEADER_SHORT, END, END_WAIT, SAFETY_END, STATS, PLAY_STOP_REFUSED,
        STOPONLOST_IGNORED, INVARIANT, PAST_END_START,
        DAY_REALIGN, DAY_UNRESOLVED, DAY_IMPLAUSIBLE   // day alignment, logging 1-3
    } type = STATS;
    int64_t a = 0, b = 0, c = 0;
    double x = 0, y = 0, z = 0;
    int kind = 0;
};

// Converts an engine time (ms) into this process's MTC day: false while the
// head is not authoritative. Lock-free. Production: MtcReceiver::toLocalDay;
// the tests drive it from their fake MTC.
struct DayResolver {
    bool (*fn)(void* ctx, long engineMs, long& localMs, long& k) = nullptr;
    void* ctx = nullptr;
};

class PlayerCore {
public:
    static const char* modeName(AudioFstream::LoopMode m);

    PlayerCore(AudioFstream& engine, unsigned outRate, unsigned periodFrames);

    // Configuration, before the stream starts
    void setOffsetMs(double ms) { offsetNs_ = (int64_t)std::floor(ms) * 1000000; }
    void setLatencyNs(int64_t ns) { latencyNs_ = ns; }
    void setFollowing(bool f) { followingMtc_ = f; }
    void setEndWaitMs(long ms) { endWaitMs_.store(ms); }
    void setDayResolver(DayResolver r) { day_ = r; }

    // OSC thread: latch an event (false if the latch is full)
    bool latch(const OscEvent& e) { return osc_.push(e); }
    // OSC thread: the receipt ack of a /loop (one write(2) by the caller)
    std::string loopAckLine(const std::string& slug, int passes, int num, int den) const;

    // The audio callback: exactly `frames` interleaved frames into `out`.
    void callback(float* out, unsigned frames, int64_t nowNs, long headMs, const TickSample& tick);

    // Main loop: drain the journal
    bool popLog(LogEvent& e) { return log_.pop(e); }
    size_t logDropped() const { return log_.dropped(); }
    static std::string describe(const LogEvent& e, const std::string& slug, bool* stdoutLine, int* level);

    // State
    lockloop::Mtc mtc() const { return live_.state(); }
    bool following() const { return followingMtc_; }
    int64_t target() const { return target_; }
    double errorMs() const { return lastEMs_; }
    const lockloop::LockLoop& loop() const { return loop_; }
    uint64_t modeWord() const { return modeWord_.load(); }   // (ver << 8) | mode
    bool endOfPlay() const { return endOfPlay_.load(); }
    long starts() const { return starts_; }
    long resyncs() const { return resyncs_; }
    long invalidErrors() const { return invalidErrors_; }
    long lostCount() const { return lostCount_; }
    double suspendedFraction() const { return periods_ ? (double)suspendedPeriods_ / periods_ : 0.0; }
    long periods() const { return periods_; }
    bool offsetPending() const { return pendWire_ != dayalign::Wire::NONE; }
    int64_t offsetNs() const { return offsetNs_; }
    long dayConversions() const { return dayConversions_; }
    AudioFstream& engine() { return engine_; }

private:
    void applyOsc(const OscEvent& e, const AudioFstream::LiveCtx& ctx);
    void push(LogEvent::Type t, int64_t a = 0, int64_t b = 0, int64_t c = 0, double x = 0, double y = 0,
              double z = 0, int kind = 0);
    void drainEngineEvents();
    int64_t computeTarget(int64_t nowNs) const;
    int64_t endEff() const;
    bool resolvePendingOffset();
    void setKind(AudioFstream::StartKind k) { lastKind_ = k; kindSeq_++; }

    AudioFstream& engine_;
    unsigned outRate_, period_;
    double rNom_;
    double T_;
    lockloop::MtcLiveness live_;
    lockloop::LockLoop loop_;
    SpscRing<OscEvent, 256> osc_;
    SpscRing<LogEvent, 1024> log_;
    bool followingMtc_ = false;
    int64_t offsetNs_ = 0, latencyNs_ = 0;
    int64_t target_ = 0;
    int64_t lastNowNs_ = 0;
    double lastEMs_ = 0;
    AudioFstream::StartKind lastKind_ = AudioFstream::StartKind::REVEAL;
    uint32_t kindSeq_ = 0;                              // bumped by every setKind()
    int settle_ = 0;
    int e10_ = 0;
    bool fifoErrorOnce_ = false, safetyOnce_ = false, playStopWarned_ = false, stopOnLostWarned_ = false;
    bool invalidLogged_ = false;
    AudioFstream::LoopMode lastMode_ = AudioFstream::LoopMode::PENDING;
    std::atomic<uint64_t> modeWord_{0};
    int lastLoopArgs_[3] = {1, 0, 0};
    std::atomic<long> endWaitMs_{0};
    std::atomic<bool> endOfPlay_{false};
    int64_t endSinceNs_ = 0;
    int64_t lastStatsNs_ = 0;
    long starts_ = 0, resyncs_ = 0, invalidErrors_ = 0, lostCount_ = 0;
    long periods_ = 0, suspendedPeriods_ = 0;
    // day alignment: the latest raw start not yet converted (latest wins)
    DayResolver day_;
    dayalign::Wire pendWire_ = dayalign::Wire::NONE;
    int64_t pendRaw_ = 0;
    int64_t pendSinceNs_ = 0;
    uint32_t pendKindSeq_ = 0;                          // kindSeq_ when the pending value was latched
    long lastHeadMs_ = 0;
    bool unresolvedWarned_ = false, implausibleWarned_ = false;
    int64_t seenK_[8] = {0};
    int nSeenK_ = 0;
    long dayConversions_ = 0;
};

#endif  // PLAYERCORE_H
