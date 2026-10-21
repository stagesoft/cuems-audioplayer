// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// The lock simulation of plan 869ejxng3 + 1amfwm7e6cw rev 3, section 4.3g
// (the gate for the loop constants, the MTC state machine and the position
// bookkeeping): an MTC master that sends exactly what libmtcmaster sends
// (8 quarter frames carrying the time vector of the group start, rate bits
// 0x20 = 25 fps, a full-frame SysEx every 2 s), a delivery model (latency,
// arrival jitter, losses, duplication, freezes, jumps, or a recorded node01
// arrival trace), the REAL mtcreceiver decoder (its MTCRECV_TESTING entry
// points), the trusted-tick seqlock, PlayerCore and the real AudioFstream
// feed engine on a synthetic looping file, with the audio clock offset from
// MTC by `audioPpm`. Simulated time only; the worker runs synchronously.
#ifndef MTC_SIM_H
#define MTC_SIM_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <zlib.h>
#include "audiofstream.h"
#include "mtcreceiver.h"
#include "playercore.h"
#include "qfseqlock.h"
#include "feed_test_util.h"

namespace mtcsim {

using Msg = std::vector<unsigned char>;
using A = AudioFstream;

constexpr int64_t MS = 1000000;
constexpr int64_t QF_NS = 10 * MS;     // 25 fps: a quarter frame every 10 ms

// MTC time vector of libmtcmaster (fillMtcTimeVector): frames, s, min, h at 25 fps
inline void timeVector(int64_t mtcMs, int v[4])
{
    const int64_t frames = mtcMs / 40;
    v[0] = (int)(frames % 25);
    v[1] = (int)((frames / 25) % 60);
    v[2] = (int)((frames / 1500) % 60);
    v[3] = (int)((frames / 90000) % 24);
}
// quarter frame `type` (0-7) of a group whose time is `groupMs`
inline Msg quarterFrame(int type, int64_t groupMs)
{
    int v[4];
    timeVector(groupMs, v);
    unsigned char c;
    if ((type & 1) == 0) c = (unsigned char)((type << 4) | (v[type / 2] & 0x0f));
    else if (type < 7) c = (unsigned char)((type << 4) | ((v[type / 2] & 0xf0) >> 4));
    else c = (unsigned char)(0x70 | (((0x20 | v[3]) & 0xf0) >> 4));
    return {0xF1, c};
}
inline Msg fullFrame(int64_t mtcMs)
{
    int v[4];
    timeVector(mtcMs, v);
    return {0xF0, 0x7F, 0x7F, 0x01, 0x01, (unsigned char)(0x20 | (v[3] & 0x1f)), (unsigned char)v[2],
            (unsigned char)v[1], (unsigned char)v[0], 0xF7};
}

struct Delivery {
    int64_t arrivalNs;
    Msg msg;
    int64_t truthMs;    // the master's MTC time when the message was sent
};

// What happens to the MTC stream (all times in simulated ns from the start)
struct Scenario {
    double audioPpm = 0.0;             // the audio clock against MTC
    double jitterSigmaMs = 0.0;        // half-normal arrival delay
    bool gaussianJitter = false;       // normal (sigma, centred +3 ms) instead of half-normal
    double uniformJitterMs = 0.0;      // uniform +-x ms (the plan's "+-3 ms arrival jitter")
    double hiccupPerS = 0.0;           // extra 25-45 ms delays
    double lossProb = 0.0;             // random single-message loss
    int64_t lossAtNs = -1;             // a burst of `lossCount` consecutive messages lost from here
    int lossCount = 0;                 // (a network gap of 2.03 s = 203 quarter frames)
    int64_t stopAtNs = -1;             // the master stops for good (a STOP)
    int64_t jumpAtNs = -1;             // the master's time jumps by jumpMs at a group boundary
    int64_t jumpMs = 0;
    bool jumpAsFullFrame = false;      // the jump announced by a full frame (a locate) right away
    int64_t doubleFromNs = -1, doubleToNs = -1;   // every message delivered twice (a doubled subscription)
    int64_t fullFrameEveryNs = 2000 * MS;
    std::string traceCsvGz;            // replay node01's recorded arrivals instead of the model
    uint32_t seed = 1;
};

// Builds the delivery list for `durationNs`.
inline std::vector<Delivery> buildStream(const Scenario& sc, int64_t durationNs, int64_t mtcStartMs)
{
    std::vector<Delivery> out;
    std::mt19937 rng(sc.seed);
    std::normal_distribution<double> n01(0.0, 1.0);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    if (!sc.traceCsvGz.empty()) {
        // node01's raw arrivals: exactly the bytes and the times it received
        gzFile gz = gzopen(sc.traceCsvGz.c_str(), "rb");
        char line[256];
        int64_t t0 = -1;
        int64_t qfIndex = 0;
        while (gz && gzgets(gz, line, sizeof line)) {
            if (strncmp(line, "raw,", 4) != 0) continue;
            long long ns; int st, d1;
            if (sscanf(line + 4, "%lld,%d,%d", &ns, &st, &d1) != 3) continue;
            if (t0 < 0) t0 = ns;
            const int64_t a = ns - t0 + 5 * MS;
            if (a > durationNs) break;
            if (st == 0xF1) {
                out.push_back({a, {0xF1, (unsigned char)d1}, mtcStartMs + qfIndex * 10});
                qfIndex++;
            }
        }
        if (gz) gzclose(gz);
        return out;
    }
    int64_t mtcBaseMs = mtcStartMs;     // master time at emission index 0
    int64_t lastArrival = 0;
    int64_t sinceFull = 0;
    int64_t burstLeft = 0;
    bool burstStarted = false;
    bool jumped = false;
    int64_t extraMs = 0;                // accumulated jumps
    for (int64_t n = 0;; n++) {
        const int64_t emit = 5 * MS + n * QF_NS;
        if (emit > durationNs) break;
        if (sc.stopAtNs >= 0 && emit >= sc.stopAtNs) break;
        const int type = (int)(n % 8);
        // the master's MTC time at this emission (rate 1, plus jumps)
        bool locateNow = false;
        if (!jumped && sc.jumpAtNs >= 0 && emit >= sc.jumpAtNs && type == 0) {
            jumped = true;
            extraMs += sc.jumpMs;
            locateNow = sc.jumpAsFullFrame;
        }
        const int64_t truth = mtcBaseMs + (emit - 5 * MS) / MS + extraMs;
        if (locateNow) {   // a locate: the full frame first, then the stream from there
            out.push_back({std::max(lastArrival + 1, emit), fullFrame(truth), truth});
            lastArrival = out.back().arrivalNs;
        }
        const int64_t groupMs = truth - type * 10;   // the group's time: its QF 0
        Msg m = quarterFrame(type, groupMs);
        // what MTC can express: the group's whole frame (libmtcmaster floors the
        // time vector to frames) plus this quarter frame's position
        const int64_t framedTruth = (groupMs / 40) * 40 + type * 10;
        // delivery
        double delay = 1.0;
        if (sc.jitterSigmaMs > 0) {
            delay += sc.gaussianJitter ? sc.jitterSigmaMs * n01(rng) + 3.0 : std::fabs(sc.jitterSigmaMs * n01(rng));
        }
        if (sc.uniformJitterMs > 0) delay += sc.uniformJitterMs * (1.0 + (2.0 * u01(rng) - 1.0));
        if (sc.hiccupPerS > 0 && u01(rng) < sc.hiccupPerS * 0.010) delay += 25.0 + 20.0 * u01(rng);
        int64_t a = emit + (int64_t)(delay * MS);
        a = std::max(a, lastArrival + 1);            // rtpmidi keeps the order
        bool lost = sc.lossProb > 0 && u01(rng) < sc.lossProb;
        if (sc.lossAtNs >= 0 && !burstStarted && emit >= sc.lossAtNs) {
            burstStarted = true;
            burstLeft = sc.lossCount;
        }
        if (burstLeft > 0) { lost = true; burstLeft--; }
        if (!lost) {
            out.push_back({a, m, framedTruth});
            lastArrival = a;
            if (sc.doubleFromNs >= 0 && emit >= sc.doubleFromNs && emit < sc.doubleToNs) {
                out.push_back({a + 100000, m, framedTruth});   // the doubled subscription: every message twice
                lastArrival = a + 100000;
            }
        }
        // the full frame every 2 s, after a group (libmtcmaster's resync), and the locate
        if (type == 7) {
            sinceFull += 8 * QF_NS;
            if (sc.fullFrameEveryNs > 0 && sinceFull >= sc.fullFrameEveryNs && !lost && burstLeft == 0) {
                sinceFull = 0;
                out.push_back({lastArrival + 200000, fullFrame(truth + 10), truth + 10});
                lastArrival += 200000;
            }
        }
    }
    return out;
}

struct Stats {
    long periods = 0, playing = 0, silentWhilePlaying = 0;
    long starts = 0, resyncs = 0, invalidErrors = 0, lostEvents = 0;
    long clampHits = 0, slewHits = 0;           // in the steady window
    double maxAbsEfSteady = 0, uMin = 1e9, uMax = -1e9, maxAbsETrueSteady = 0;
    double maxTargetDevMs = 0;                  // in a watched window
    double suspendedFraction = 0;
    long wrongTrustedHeads = 0;
    std::vector<int64_t> startTimesNs;          // when each start was logged
    std::vector<int64_t> lostTimesNs, returnTimesNs, invalidTimesNs;
    int64_t firstSilentNs = -1;
};

// The simulation rig
class Sim {
public:
    unsigned outRate = 48000, N = 512;
    Scenario sc;
    std::string wavPath;
    std::unique_ptr<A> file;
    std::unique_ptr<PlayerCore> core;
    std::unique_ptr<MtcReceiver> rx;
    TickSeqlock seq;
    static int64_t s_nowNs;
    Stats st;
    int64_t cueStartMtcMs = 0;     // the offset: the cue starts at this MTC time
    int64_t cueStartAfterGoMs = -1; // >= 0: the cue starts this long after the decoded head at GO (traces)
    int64_t excludeWrongFromNs = -1, excludeWrongToNs = -1;   // a jump's transition (one group)
    int64_t latencyFrames = 1536;
    double headOffsetMs = 0;       // measured decoded-head vs truth offset (c0)

    // A 20 s mono float WAV (a 997 Hz sine), looping forever.
    bool open(int passes = -1)
    {
        wavPath = feedtest::tmpPath("mtcsim");
        std::vector<float> x(20 * outRate);
        for (size_t i = 0; i < x.size(); i++) x[i] = 0.25f * (float)std::sin(2 * M_PI * 997.0 * i / outRate);
        feedtest::writeWavF32(wavPath, outRate, 1, x);
        file.reset(new A());
        file->open(wavPath, std::ios::binary | std::ios::in);
        if (!file->good() || !file->prepareFeed(outRate, N, false)) return false;
        core.reset(new PlayerCore(*file, outRate, N));
        core->setLatencyNs(latencyFrames * 1000000000LL / outRate);
        core->setEndWaitMs(__LONG_MAX__);
        if (passes != 1) file->setLoop(passes, 0, 0);   // /loop [1] = the default (mode stays pending)
        MtcReceiver::resetStaticStateForTesting();
        try {
            rx.reset(new MtcReceiver(MtcReceiver::SkipPortOpenTag{}));
        } catch (...) {
            return false;   // no ALSA sequencer access (the RtMidi backend)
        }
        rx->resetDecoderStateForTesting();
        MtcReceiver::setTickCallback([this](long head, bool complete) { seq.onTick(head, complete, s_nowNs); });
        return true;
    }
    ~Sim()
    {
        MtcReceiver::setTickCallback(nullptr);
        MtcReceiver::resetStaticStateForTesting();
        if (!wavPath.empty()) unlink(wavPath.c_str());
    }
    void latch(OscEvent::Type t, int32_t a = 0, double f = 0, int32_t b = 0, int32_t c = 0, int nargs = 1)
    {
        OscEvent e;
        e.type = t; e.a = a; e.b = b; e.c = c; e.f = f; e.nargs = nargs;
        core->latch(e);
    }

    // Runs `durationNs` of simulated time. The GO: at `goAtNs` the engine's
    // run + reveal: /offset = -cueStart and /mtcfollow 1.
    // steadyFromNs: the window for the steady-state gates.
    template <typename Watch>
    void run(int64_t durationNs, int64_t goAtNs, int64_t mtcStartMs, int64_t steadyFromNs, Watch watch)
    {
        auto stream = buildStream(sc, durationNs, mtcStartMs);
        size_t si = 0;
        const double periodNs = 1e9 * N / (outRate * (1.0 + sc.audioPpm * 1e-6));
        TickSample ts;
        std::vector<float> out(N);
        bool gone = false;
        long trustedChecked = 0;
        uint64_t seenSeq = 0;
        double sumOff = 0;
        long nOff = 0;
        for (long k = 0;; k++) {
            const int64_t now = (int64_t)(k * periodNs);
            if (now > durationNs) break;
            // deliver everything that has arrived
            while (si < stream.size() && stream[si].arrivalNs <= now) {
                s_nowNs = stream[si].arrivalNs;
                Msg m = stream[si].msg;
                if (m[0] == 0xF1) rx->decodeQuarterFrameForTesting(m);
                else rx->decodeFullFrameForTesting(m);
                // a trusted head against the truth of its message
                TickSample t;
                if (seq.read(t) && t.seq != seenSeq) {
                    seenSeq = t.seq;
                    if (t.anchored) {
                        const double off = (double)t.headMs - (double)stream[si].truthMs;
                        if (nOff < 200) { sumOff += off; nOff++; headOffsetMs = sumOff / nOff; }
                        else if (std::fabs(off - headOffsetMs) > 20.0 &&
                                 !(stream[si].arrivalNs >= excludeWrongFromNs && stream[si].arrivalNs < excludeWrongToNs))
                            st.wrongTrustedHeads++;
                        trustedChecked++;
                    }
                }
                si++;
            }
            if (!gone && goAtNs >= 0 && now >= goAtNs) {
                gone = true;
                if (cueStartAfterGoMs >= 0) cueStartMtcMs = MtcReceiver::mtcHead.load() + cueStartAfterGoMs;
                latch(OscEvent::OFFSET_F, 0, -(double)cueStartMtcMs);
                latch(OscEvent::MTCFOLLOW, 1);
            }
            seq.read(ts);
            const long startsBefore = core->starts();
            core->callback(out.data(), N, now, MtcReceiver::mtcHead.load(), ts);
            file->workerRunPending();
            st.periods++;
            if (core->starts() != startsBefore) st.startTimesNs.push_back(now);
            const bool playing = file->ps() == A::Ps::PLAY && file->outPlaying();
            if (playing) st.playing++;
            LogEvent le;
            while (core->popLog(le)) {
                if (le.type == LogEvent::MTC_LOST) st.lostTimesNs.push_back(now);
                if (le.type == LogEvent::MTC_RETURN) st.returnTimesNs.push_back(now);
                if (le.type == LogEvent::MTC_INVALID) st.invalidTimesNs.push_back(now);
            }
            if (now >= steadyFromNs && playing && core->mtc() == lockloop::Mtc::LIVE) {
                st.maxAbsEfSteady = std::max(st.maxAbsEfSteady, std::fabs(core->loop().ef()));
                st.uMin = std::min(st.uMin, core->loop().u());
                st.uMax = std::max(st.uMax, core->loop().u());
            }
            watch(*this, now, playing);
        }
        st.starts = core->starts();
        st.resyncs = core->resyncs();
        st.invalidErrors = core->invalidErrors();
        st.lostEvents = core->lostCount();
        st.suspendedFraction = core->suspendedFraction();
        (void)trustedChecked;
    }
    // the target the player should have with the true MTC (and the measured head offset)
    int64_t truthTarget(int64_t nowNs, int64_t mtcStartMs, int64_t extraMs = 0) const
    {
        const int64_t mtcNs = (mtcStartMs + extraMs) * MS + (nowNs - 5 * MS) + (int64_t)(headOffsetMs * MS);
        return lockloop::targetSamples(mtcNs, -cueStartMtcMs * MS, latencyFrames * 1000000000LL / outRate, outRate);
    }
};

inline int64_t Sim::s_nowNs = 0;

}  // namespace mtcsim

#endif  // MTC_SIM_H
