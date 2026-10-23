// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Feed-engine tests of plan 869ejxng3 + 1amfwm7e6cw rev 3, step 1 (section
// 4.3g, the AudioFstream part of section 4.3h v2.1): the head cache, the
// wrap below the resampler, the start rule, the worker jobs, the pad, the
// failure paths and the T1/T6/T7/T8 cases, plus a randomized driver over the
// (event x state) pairs that live in AudioFstream, seeded from the step-0
// model (cuems-RELATIONS baselines/1amfwm7e6cw-varispeed-probe/step0/
// h3_model_v2.py). The worker runs synchronously when the rig says so (the
// fake worker clock). Fixtures: STEP0_GATE_DIR (step0/gate/fixtures).

#include <gtest/gtest.h>
#include <algorithm>
#include <random>
#include "feed_test_util.h"

using namespace feedtest;

namespace {

class FeedEngine : public ::testing::Test {
protected:
    void SetUp() override {
        if (gateDir().empty()) GTEST_SKIP() << "STEP0_GATE_DIR not set";
        fixtures = manifest();
        ASSERT_FALSE(fixtures.empty());
    }
    const Fixture* find(const std::string& n) {
        for (const auto& f : fixtures) if (f.name == n) return &f;
        return nullptr;
    }
    std::vector<Fixture> fixtures;
};

const int64_t M_MS = 250;

}  // namespace

// K1/K5: the cache filled at prepareFeed() equals the linear decode, and is
// at least 1 s (or the whole file).
TEST_F(FeedEngine, HeadCacheEqualsLinearDecode) {
    for (const auto& f : fixtures) {
        SCOPED_TRACE(f.name);
        unsigned ch = 0;
        const auto ref = linearDecode(fixturePath(f.name), &ch);
        Rig g;
        ASSERT_TRUE(g.open(fixturePath(f.name)));
        const auto& c = g.s.headCache();
        const int64_t H = g.s.cacheLength();
        EXPECT_GE(H, std::min<int64_t>(f.rate, f.ffprobeSum));
        ASSERT_LE((size_t)H * ch, ref.size());
        double maxd = 0;
        for (size_t i = 0; i < c.size(); i++) maxd = std::max(maxd, (double)std::fabs(c[i] - ref[i]));
        EXPECT_EQ(maxd, 0.0);
        EXPECT_TRUE(g.s.decAtH());
        EXPECT_TRUE(g.s.soxrPristine());
        EXPECT_EQ(g.s.ps(), A::Ps::HOLD);
        EXPECT_TRUE(g.s.demuxDiscardsNonAudio());
    }
}

// The wrap below the resampler (section 4.3c, E12/E14, WRAP job): 3 passes,
// the input fed to soxr == the linear decode three times, no sample lost or
// repeated; every period exactly N frames; FIFO at 241; resampled too.
TEST_F(FeedEngine, GaplessWrapBelowTheResampler) {
    for (const auto& f : fixtures) {
        for (unsigned outRate : {48000u, 44100u}) {
            if (outRate == 44100u && f.name != "a.wav" && f.name != "a.m4a") continue;
            SCOPED_TRACE(f.name + " -> " + std::to_string(outRate));
            unsigned ch = 0;
            const auto ref = linearDecode(fixturePath(f.name), &ch);
            const long L = (long)(ref.size() / ch);
            Rig g;
            ASSERT_TRUE(g.open(fixturePath(f.name), false, outRate));
            g.s.setLoop(3, 0, 0);
            EXPECT_EQ(g.s.loopMode(), A::LoopMode::WRAP);
            g.s.start(0, A::StartKind::REVEAL);
            ASSERT_EQ(g.s.ps(), A::Ps::PLAY);
            double fmin = 1e9, fmax = -1e9;
            for (long i = 0; i < 4000000 && !(g.s.ps() == A::Ps::PASTEND && g.s.feed() == A::Feed::ENDED); i++) {
                g.period();
                if (g.periods > 100 && g.s.ps() == A::Ps::PLAY) {
                    fmin = std::min(fmin, g.s.fifo());
                    fmax = std::max(fmax, g.s.fifo());
                }
            }
            g.runToEnd(10);
            ASSERT_EQ((long)(g.tap.v.size() / ch), 3 * L);
            long fd = -1;
            const double maxd = maxDiffPeriodic(g.tap.v, ref, ch, 0, L, &fd);
            EXPECT_LE(maxd, tolFor(f)) << "first differing frame " << fd;
            EXPECT_EQ(g.shortPeriods, 0);
            EXPECT_GE(fmin, 241.0 - 4.0);
            EXPECT_LE(fmax, 241.0 + 4.0);
            EXPECT_EQ(g.s.passIndex(), 2);
            EXPECT_EQ(g.count(A::EvType::PASS_CROSSED), 2);
            EXPECT_EQ(g.count(A::EvType::END), 1);
            EXPECT_EQ(g.count(A::EvType::DRAINED), 1);
            EXPECT_EQ(g.s.invariantViolations(), 0);
            EXPECT_EQ(g.s.soxrClearsOutsideWorker(), 0);
            EXPECT_TRUE(g.s.soxrPristine());   // E21's RESET ran
            EXPECT_TRUE(g.s.decAtH());
        }
    }
}

// The same with the real worker thread (paced like a JACK period).
TEST_F(FeedEngine, GaplessWrapWithTheWorkerThread) {
    for (const char* n : {"a.wav", "av.mkv", "a.weba", "a.m4a"}) {
        const Fixture* f = find(n);
        if (!f) continue;
        SCOPED_TRACE(n);
        unsigned ch = 0;
        const auto ref = linearDecode(fixturePath(n), &ch);
        const long L = (long)(ref.size() / ch);
        Rig g;
        ASSERT_TRUE(g.open(fixturePath(n), true));
        g.runWorker = false;
        g.s.setLoop(3, 0, 0);
        g.s.start(0, A::StartKind::REVEAL);
        for (long i = 0; i < 100000 && !(g.s.ps() == A::Ps::PASTEND && g.s.feed() == A::Feed::ENDED); i++) {
            g.period();
            usleep(200);
        }
        usleep(20000);
        g.s.serviceWorker(g.ctx);
        ASSERT_EQ((long)(g.tap.v.size() / ch), 3 * L);
        EXPECT_LE(maxDiffPeriodic(g.tap.v, ref, ch, 0, L), tolFor(*f));
        EXPECT_EQ(g.count(A::EvType::HANDOVER_LATE), 0);
        EXPECT_EQ(g.s.invariantViolations(), 0);
        EXPECT_EQ(g.s.soxrClearsOutsideWorker(), 0);
    }
}

// I16f / M1: the reveal from a pristine HOLD is a cache start with no job
// and no fade; the first follow plays from the cache.
TEST_F(FeedEngine, RevealIsACleanCacheStartWithNoJob) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(1, 0, 0);
    g.ctx.target = 1536;   // 32 ms in: the output latency
    g.s.start(g.ctx.target, A::StartKind::REVEAL);
    EXPECT_EQ(g.s.ps(), A::Ps::PLAY);
    EXPECT_EQ(g.s.jobsOutstanding(), 0);
    EXPECT_EQ(g.s.origin(), 1536);
    g.period();
    const A::FeedEvent* e = g.last(A::EvType::CACHE_START);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->kind, (int)A::StartKind::REVEAL);
    // the same start as an /offset is faded over 2 ms: less energy in the first 96 frames
    Rig h;
    ASSERT_TRUE(h.open(fixturePath("a.wav")));
    h.s.setLoop(1, 0, 0);
    h.ctx.target = 1536;
    h.s.start(h.ctx.target, A::StartKind::OFFSET);
    h.period();
    double clean = 0, faded = 0;
    for (size_t i = 0; i < 96 * g.ch; i++) { clean += g.out[i] * g.out[i]; faded += h.out[i] * h.out[i]; }
    EXPECT_GT(clean, 0.0);
    EXPECT_LT(faded, clean);
    for (size_t i = 96 * g.ch; i < g.out.size(); i++) EXPECT_EQ(g.out[i], h.out[i]);
}

// M3: a start at H - M - 1 ms is a cache start, one at H - M + 1 ms a worker
// seek; neither ends in a late handover.
TEST_F(FeedEngine, StartRuleMarginIs250ms) {
    for (int d : {-1, +1}) {
        SCOPED_TRACE(d);
        Rig g;
        ASSERT_TRUE(g.open(fixturePath("a.wav")));
        g.s.setLoop(1, 0, 0);
        const int64_t t = g.s.cacheLength() - M_MS * 48 + d * 48;
        g.ctx.target = t;
        g.s.start(t, A::StartKind::OFFSET);
        if (d < 0) {
            EXPECT_EQ(g.s.ps(), A::Ps::PLAY);
            EXPECT_EQ(g.s.jobsOutstanding(), 0);
        } else {
            EXPECT_EQ(g.s.ps(), A::Ps::SEEKING);
            EXPECT_EQ(g.s.runningOrPendingJob(), A::JobKind::SEEK);
        }
        for (int i = 0; i < 300; i++) g.period();
        EXPECT_EQ(g.count(A::EvType::HANDOVER_LATE), 0);
        EXPECT_EQ(g.s.ps(), A::Ps::PLAY);
        EXPECT_EQ(g.s.invariantViolations(), 0);
    }
}

// M1: a start inside the head while soxr is fed goes through a RESET, then
// starts at the live target (faded), content and pos exact.
TEST_F(FeedEngine, CacheStartOnAFedSoxrGoesThroughAReset) {
    unsigned ch = 0;
    const auto ref = linearDecode(fixturePath("a.wav"), &ch);
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    for (int i = 0; i < 30; i++) g.period();
    const int64_t t = 4800;
    g.ctx.target = t;
    g.s.offsetChanged(t);
    EXPECT_EQ(g.s.ps(), A::Ps::SEEKING);
    EXPECT_EQ(g.s.runningOrPendingJob(), A::JobKind::RESET);
    g.tap.v.clear();
    int64_t liveAtStart = -1;
    for (int i = 0; i < 10 && g.s.ps() != A::Ps::PLAY; i++) {
        liveAtStart = g.ctx.target;
        g.period();
    }
    ASSERT_EQ(g.s.ps(), A::Ps::PLAY);
    EXPECT_EQ(g.s.origin(), liveAtStart);
    EXPECT_EQ(maxDiffPeriodic(g.tap.v, ref, ch, liveAtStart, (int64_t)(ref.size() / ch)), 0.0);
    const A::FeedEvent* e = g.last(A::EvType::CACHE_START);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->a, liveAtStart);
    EXPECT_EQ(g.s.soxrClearsOutsideWorker(), 0);
}

// The worker seek (h.2 SEEK, section 4.3a): random seeks land on the live
// target; the content at origin is the linear decode at that index, exact on
// sample-timebase containers, within +-1 ms on mkv/webm (K6).
TEST_F(FeedEngine, WorkerSeekLandsOnTheLiveTarget) {
    std::mt19937 rng(7);
    for (const auto& f : fixtures) {
        SCOPED_TRACE(f.name);
        unsigned ch = 0;
        const auto ref = linearDecode(fixturePath(f.name), &ch);
        const long L = (long)(ref.size() / ch);
        for (int k = 0; k < 4; k++) {
            Rig g;
            ASSERT_TRUE(g.open(fixturePath(f.name)));
            g.s.setLoop(1, 0, 0);
            const int64_t H = g.s.cacheLength();
            std::uniform_int_distribution<long> d(H, L - 2 * f.rate);
            const int64_t t = d(rng);
            g.ctx.target = t;
            g.s.start(t, A::StartKind::OFFSET);
            ASSERT_EQ(g.s.runningOrPendingJob(), A::JobKind::SEEK);
            int64_t liveAtPickup = -1;
            for (int i = 0; i < 20 && g.s.ps() != A::Ps::PLAY; i++) {
                liveAtPickup = g.ctx.target;
                g.tap.v.clear();
                g.period();
            }
            ASSERT_EQ(g.s.ps(), A::Ps::PLAY);
            EXPECT_EQ(g.s.origin(), liveAtPickup);
            const long off = contentOffset(g.tap.v, ref, ch, g.s.origin());
            const long tol = (f.cls == "ms" || f.name.find(".mkv") != std::string::npos ||
                              f.name.find(".weba") != std::string::npos) ? f.rate / 1000 : 0;
            EXPECT_LE(std::labs(off), tol) << "seek to " << t;
            EXPECT_EQ(g.s.invariantViolations(), 0);
        }
    }
}

// Seek acceptance (section 4.3a, the coordinator's decisions on step 1's
// item 7): a landing is accepted iff it leaves the codec twice its declared
// pre-roll, landing <= target - 2 x seek_preroll (7680 samples = 160 ms for
// Opus at 48 kHz). The Opus and webm-Opus decode in the first 100 ms after
// the pickup matches a linear decode of the same region within 0.01. Opus
// does not converge bit-exactly: on this chirp the worst error was 0.0057 over
// 120 engine seeks at 2x pre-roll (casas probe8), and plain libav gives
// 0.0025 with 160-240 ms of lead, 2.8e-4 with 240-480 ms, 6e-8 beyond (probe
// opusconv2), so 1e-4 is not reachable at this bound; 0.01 = 1.75x the worst.
TEST_F(FeedEngine, OpusSeekGetsItsPrerollAndConverges) {
    std::mt19937 rng(3);
    for (const char* n : {"a.opus", "a.weba"}) {
        if (!find(n)) continue;
        SCOPED_TRACE(n);
        unsigned ch = 0;
        const auto ref = linearDecode(fixturePath(n), &ch);
        const long L = (long)(ref.size() / ch);
        for (int k = 0; k < 6; k++) {
            Rig g;
            ASSERT_TRUE(g.open(fixturePath(n)));
            g.s.setLoop(1, 0, 0);
            std::uniform_int_distribution<long> d(g.s.cacheLength(), L - 2 * 48000);
            const int64_t t = d(rng);
            g.ctx.target = t;
            g.s.start(t, A::StartKind::OFFSET);
            for (int i = 0; i < 20 && g.s.ps() != A::Ps::PLAY; i++) {
                g.tap.v.clear();
                g.period();
            }
            ASSERT_EQ(g.s.ps(), A::Ps::PLAY);
            for (int i = 0; i < 12; i++) g.period();                 // >= 100 ms of input below soxr
            ASSERT_GE((long)(g.tap.v.size() / ch), 4800L);
            const int64_t tol = std::string(n) == "a.weba" ? 48 : 0;   // webm's 1 ms index
            EXPECT_GE(g.s.lastSeekLeadSamples(), 7680 - tol) << "seek to " << t;
            const long off = contentOffset(g.tap.v, ref, ch, g.s.origin(), 2048, 60);
            EXPECT_LE(std::labs(off), tol);
            double maxd = 0;
            for (long i = 0; i < 4800; i++)
                for (unsigned c = 0; c < ch; c++)
                    maxd = std::max(maxd, (double)std::fabs(g.tap.v[(size_t)i * ch + c] -
                                                             ref[(size_t)(g.s.origin() + off + i) * ch + c]));
            EXPECT_LE(maxd, 0.01) << "seek to " << t;
            EXPECT_EQ(g.s.seekRetryCount(), 0);
        }
    }
}

// PCM (seek_preroll 0) is accepted at any landing <= target: never a retry.
TEST_F(FeedEngine, PcmSeekNeverRetries) {
    std::mt19937 rng(5);
    for (const char* n : {"a.wav", "a_pcm.avi", "a_pcm.mov", "a_pcm.mkv", "a_44k.wav"}) {
        const Fixture* f = find(n);
        if (!f) continue;
        SCOPED_TRACE(n);
        for (int k = 0; k < 8; k++) {
            Rig g;
            ASSERT_TRUE(g.open(fixturePath(n)));
            g.s.setLoop(1, 0, 0);
            std::uniform_int_distribution<long> d(g.s.cacheLength(), f->ffprobeSum - 2 * f->rate);
            const int64_t t = d(rng);
            g.ctx.target = t;
            g.s.start(t, A::StartKind::OFFSET);
            ASSERT_TRUE(g.untilPlay());
            EXPECT_EQ(g.s.seekRetryCount(), 0) << "seek to " << t;
            EXPECT_GE(g.s.lastSeekLeadSamples(), std::string(n) == "a_pcm.mkv" ? -48 : 0);
        }
    }
}

// M4: a cue-less A/V matroska seeks on the audio index; an A/V mkv with
// cues on its dense video index.
TEST_F(FeedEngine, MatroskaSeekIndexByDensity) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("av.mkv")));
    EXPECT_TRUE(g.s.seekStreamIsVideo());
    Rig h;
    ASSERT_TRUE(h.open(fixturePath("a_pcm.mkv")));
    EXPECT_FALSE(h.s.seekStreamIsVideo());   // no video stream at all
}

// E18 / L5: a WRAP still running when the cursor reaches H becomes a SEEK of
// the live target; origin is set by the pickup.
TEST_F(FeedEngine, LateHandoverBecomesASeek) {
    unsigned ch = 0;
    const auto ref = linearDecode(fixturePath("a.wav"), &ch);
    const long L = (long)(ref.size() / ch);
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(-1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    g.blockWrap = true;
    for (int i = 0; i < 3000 && g.s.ps() != A::Ps::SEEKING; i++) g.period();
    ASSERT_EQ(g.s.ps(), A::Ps::SEEKING);
    EXPECT_EQ(g.count(A::EvType::HANDOVER_LATE), 1);
    EXPECT_EQ(g.s.runningOrPendingJob(), A::JobKind::SEEK);
    g.blockWrap = false;
    g.tap.v.clear();
    ASSERT_TRUE(g.untilPlay());
    EXPECT_EQ(g.s.passIndex(), 1);
    const int64_t o = g.s.origin();
    EXPECT_GE(o, L);
    EXPECT_EQ(maxDiffPeriodic(g.tap.v, ref, ch, o % L, L), 0.0);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// A hard seek posted during a WRAP supersedes it (generation only).
TEST_F(FeedEngine, SeekSupersedesAWrap) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(-1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    g.blockWrap = true;
    for (int i = 0; i < 3000 && g.s.runningOrPendingJob() != A::JobKind::WRAP; i++) g.period();
    ASSERT_EQ(g.s.runningOrPendingJob(), A::JobKind::WRAP);
    const uint64_t g0 = g.s.generation();
    const int64_t t = g.ctx.target + 3 * 48000;
    g.ctx.target = t;
    g.s.offsetChanged(t);
    EXPECT_GT(g.s.generation(), g0);
    g.blockWrap = false;
    ASSERT_TRUE(g.untilPlay());
    EXPECT_EQ(g.count(A::EvType::HANDOVER_LATE), 0);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// PAD (E12 after PAD) and the |L - P_s| check: P_s = L + 200 ms, 3 passes.
TEST_F(FeedEngine, PadBetweenEndOfFileAndThePeriod) {
    unsigned ch = 0;
    const auto ref = linearDecode(fixturePath("a.wav"), &ch);
    const long L = (long)(ref.size() / ch);
    const long P = L + 9600;
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(3, P, 48000);
    g.s.start(0, A::StartKind::REVEAL);
    g.runToEnd();
    ASSERT_EQ((long)(g.tap.v.size() / ch), 3 * P);
    EXPECT_EQ(maxDiffPeriodic(g.tap.v, ref, ch, 0, P), 0.0);
    EXPECT_EQ(g.count(A::EvType::LP_MISMATCH), 1);   // ERROR once per file
    EXPECT_EQ(g.count(A::EvType::FIRST_EOF), 1);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// T8: a start in the pad region feeds PAD, no seek past EOF.
TEST_F(FeedEngine, PadStartNeverSeeksPastTheEnd) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    const long L = 576000;
    g.s.setLoop(-1, L + 9600, 48000);
    g.s.start(0, A::StartKind::REVEAL);
    for (int i = 0; i < 1200; i++) g.period();          // past the first EOF: L_dec known
    ASSERT_EQ(g.s.decodedLength(), L);
    g.s.stopOutput();
    g.s.start(-1, A::StartKind::OFFSET);                // step 0: RESET so the next start is pristine
    for (int i = 0; i < 4; i++) g.period();
    const int64_t t = 2 * (L + 9600) + L + 4800;        // pass 2, inside the pad
    g.ctx.target = t;
    g.s.start(t, A::StartKind::OFFSET);
    EXPECT_EQ(g.s.ps(), A::Ps::PLAY);
    EXPECT_EQ(g.s.feed(), A::Feed::PAD);
    EXPECT_EQ(g.s.runningOrPendingJob(), A::JobKind::NONE);
    EXPECT_EQ(g.s.passIndex(), 2);
}

// I14: P_s < L cuts every pass at P_s from pass 1.
TEST_F(FeedEngine, CutAtThePeriodFromPassOne) {
    unsigned ch = 0;
    const auto ref = linearDecode(fixturePath("a.flac"), &ch);
    const long P = 3 * 48000 + 77;
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.flac")));
    g.s.setLoop(3, P, 48000);
    g.s.start(0, A::StartKind::REVEAL);
    g.runToEnd();
    ASSERT_EQ((long)(g.tap.v.size() / ch), 3 * P);
    EXPECT_EQ(maxDiffPeriodic(g.tap.v, ref, ch, 0, P), 0.0);
    EXPECT_EQ(g.count(A::EvType::LP_MISMATCH), 0);   // never reaches EOF
}

// K5 / S8: a loop that fits the cache plays from it alone (no job), 100
// wraps; an /offset into it is a cache start (N7: no margin there).
TEST_F(FeedEngine, ShortLoopPlaysFromTheCacheAlone) {
    unsigned ch = 0;
    const auto ref = linearDecode(fixturePath("a.wav"), &ch);
    const long P = 24000;   // 0.5 s
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(-1, P, 48000);
    g.s.start(0, A::StartKind::REVEAL);
    int posted = 0;
    for (int i = 0; i < 100 * 47 + 10; i++) {
        g.period();
        posted += g.s.jobsOutstanding() > 0;
    }
    EXPECT_EQ(posted, 0);
    EXPECT_GE(g.s.passIndex(), 100);
    EXPECT_EQ(maxDiffPeriodic(g.tap.v, ref, ch, 0, P), 0.0);
    const int64_t t = (g.s.passIndex() + 1) * P + 14400;   // 300 ms into a pass
    g.ctx.target = t;
    g.s.offsetChanged(t);
    for (int i = 0; i < 5; i++) g.period();
    EXPECT_EQ(g.s.ps(), A::Ps::PLAY);
    EXPECT_EQ(g.count(A::EvType::SEEK_PICKUP), 0);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// The tiny-loop guard: off, latched per file; a re-send does not re-arm it.
TEST_F(FeedEngine, TinyLoopIsOffAndLatched) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(-1, 2400, 48000);   // 50 ms
    EXPECT_EQ(g.s.loopMode(), A::LoopMode::OFF);
    g.drain();
    EXPECT_EQ(g.count(A::EvType::TINY_LOOP), 1);
    g.s.setLoop(-1, 0, 0);
    EXPECT_EQ(g.s.loopMode(), A::LoopMode::OFF);
    EXPECT_EQ(g.s.wrapsLeft(), 0);
    g.s.setLoop(1, 0, 0);
    EXPECT_EQ(g.s.loopMode(), A::LoopMode::OFF);
}

// T5 / section 4.3c step 4: a failed WRAP latches off and, at H, takes the
// E13 path: drain, PASTEND, E21 reached.
TEST_F(FeedEngine, FailedWrapTakesTheEndPath) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(-1, 0, 0);
    g.s.failNextJob(A::JobKind::WRAP);
    g.s.start(0, A::StartKind::REVEAL);
    g.runToEnd(4000);
    EXPECT_EQ(g.s.ps(), A::Ps::PASTEND);
    EXPECT_EQ(g.s.feed(), A::Feed::ENDED);
    EXPECT_EQ(g.s.loopMode(), A::LoopMode::OFF);
    EXPECT_EQ(g.count(A::EvType::WRAP_FAILED), 1);
    EXPECT_EQ(g.count(A::EvType::DRAINED), 1);
    g.s.setLoop(-1, 0, 0);
    EXPECT_EQ(g.s.loopMode(), A::LoopMode::OFF);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// T1: posting a SEEK marks soxr FED and dec MID at once, so a start while the
// (abandoned) job still runs is never a cache start.
TEST_F(FeedEngine, PostingASeekMarksTheStateDirtyAtOnce) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(1, 0, 0);
    g.runWorker = false;
    const int64_t t = 5 * 48000;
    g.ctx.target = t;
    g.s.start(t, A::StartKind::REVEAL);
    ASSERT_EQ(g.s.runningOrPendingJob(), A::JobKind::SEEK);
    EXPECT_FALSE(g.s.soxrPristine());
    EXPECT_FALSE(g.s.decAtH());
    g.s.stopOutput();                    // E3: abandoned
    g.ctx.target = 9600;
    g.s.start(9600, A::StartKind::REVEAL);
    EXPECT_EQ(g.s.ps(), A::Ps::SEEKING);
    EXPECT_EQ(g.s.invariantViolations(), 0);
    g.runWorker = true;
    ASSERT_TRUE(g.untilPlay());
    EXPECT_EQ(g.s.feed(), A::Feed::CACHE);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// T6: a live target that went negative during the SEEK (an MTC jump back):
// the pickup runs h.4 step 0, never a negative origin or pass index.
TEST_F(FeedEngine, NegativeTargetAtThePickupHolds) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(1, 0, 0);
    g.ctx.target = 5 * 48000;
    g.s.start(g.ctx.target, A::StartKind::OFFSET);
    g.s.workerRunPending();
    g.ctx.target = -3 * 48000;
    g.s.serviceWorker(g.ctx);
    EXPECT_EQ(g.s.ps(), A::Ps::HOLD);
    EXPECT_GE(g.s.origin(), 0);
    EXPECT_GE(g.s.passIndex(), 0);
    EXPECT_EQ(g.s.runningOrPendingJob(), A::JobKind::RESET);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// T7: the live target crossed P_s during the SEEK: HOLD and h.4 re-runs with
// the live target (here a cache start in the next pass).
TEST_F(FeedEngine, ChaseAcrossThePassBoundaryReRunsTheStart) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    const long L = 576000;
    g.s.setLoop(-1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    for (int i = 0; i < 1200; i++) g.period();   // L_dec known
    const int64_t t = L - 2400;                  // 50 ms before the end of pass 0
    g.ctx.target = t;
    g.s.offsetChanged(t);
    ASSERT_EQ(g.s.runningOrPendingJob(), A::JobKind::SEEK);
    g.s.workerRunPending();
    g.ctx.target = L + 4800;                     // the target is now 100 ms into pass 1
    g.s.serviceWorker(g.ctx);
    for (int i = 0; i < 6 && g.s.ps() != A::Ps::PLAY; i++) g.period();
    ASSERT_EQ(g.s.ps(), A::Ps::PLAY);
    EXPECT_EQ(g.s.passIndex(), 1);
    EXPECT_EQ(g.s.feed(), A::Feed::CACHE);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// T3 / N12: a SEEK that fails, then fails its retry 1 s later, latches
// seekFailed (HOLD); exactly two attempts; clearSeekFailed + start retries.
TEST_F(FeedEngine, SeekFailureLatchesAfterTheRetry) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(1, 0, 0);
    g.ctx.target = 5 * 48000;
    g.s.failNextJob(A::JobKind::SEEK);
    g.s.start(g.ctx.target, A::StartKind::OFFSET);
    for (int i = 0; i < 5; i++) g.period();
    EXPECT_EQ(g.count(A::EvType::SEEK_FAILED), 1);
    EXPECT_EQ(g.s.ps(), A::Ps::SEEKING);
    g.s.failNextJob(A::JobKind::SEEK);
    for (int i = 0; i < 200; i++) g.period();
    EXPECT_EQ(g.count(A::EvType::SEEK_FAILED), 2);
    EXPECT_EQ(g.s.ps(), A::Ps::HOLD);
    EXPECT_TRUE(g.s.seekFailedLatched());
    g.s.clearSeekFailed();
    g.s.start(g.ctx.target, A::StartKind::OFFSET);
    ASSERT_TRUE(g.untilPlay());
}

// T2 / N8 / S6 / S17: lowering passes below the current pass finishes that
// pass, then E13 at its boundary (E22's decision is step 2's).
TEST_F(FeedEngine, LoweringPassesFinishesTheCurrentPass) {
    for (int scenario : {6, 17}) {
        SCOPED_TRACE(scenario);
        Rig g;
        ASSERT_TRUE(g.open(fixturePath("a.wav")));
        const long L = 576000;
        g.s.setLoop(scenario == 6 ? -1 : 3, 0, 0);
        g.s.start(0, A::StartKind::REVEAL);
        const int64_t into = scenario == 6 ? L + L / 2 : 2 * L + L / 2;   // mid pass 2 / mid pass 3
        while (g.ctx.target < into) g.period();
        g.s.setLoop(scenario == 6 ? 1 : 2, 0, 0);
        g.runToEnd();
        const A::FeedEvent* e = g.last(A::EvType::END);
        ASSERT_NE(e, nullptr);
        const long passes = scenario == 6 ? 2 : 3;
        EXPECT_EQ((long)(g.tap.v.size() / g.ch), passes * L);
        EXPECT_EQ(g.s.invariantViolations(), 0);
    }
}

// E21 / M1: after a drain the RESET leaves soxr pristine; a re-enable and a
// new start begin without the full-scale transient.
TEST_F(FeedEngine, DrainThenResetThenACleanRestart) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    g.runToEnd();
    ASSERT_EQ(g.s.feed(), A::Feed::ENDED);
    EXPECT_TRUE(g.s.soxrPristine());
    g.s.setLoop(2, 0, 0);
    ASSERT_TRUE(g.s.reenable(g.ctx.target));      // passes increased since the end
    EXPECT_EQ(g.s.ps(), A::Ps::HOLD);
    g.ctx.target = 576000 + 1000;
    g.s.start(g.ctx.target, A::StartKind::REENABLE);
    EXPECT_EQ(g.s.ps(), A::Ps::PLAY);
    g.period();
    float peak = 0;
    for (float v : g.out) peak = std::max(peak, std::fabs(v));
    EXPECT_LE(peak, 1.0f);
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// T7-12: a file shorter than the re-enable margin (341 + 80 ms) in once mode,
// re-enabled at every pass by the engine's legacy per-pass /offset_i (the
// tiny-loop guard turned wrapping off). The fixed margin made e - margin
// negative, so every re-enable after the first was refused and the cue went
// silent. The margin no longer reaches into the file's first 341 + 80 ms, so
// any target inside a tiny file re-enables it (the rig's targets carry ~32 ms
// of latency: 30-45 ms into a 50 ms file); a target at the end still does not,
// and a long file keeps the full margin.
TEST_F(FeedEngine, TinyFileIsReenabledByEveryPerPassOffset) {
    const std::string p = tmpPath("tiny50ms");
    const long L = 2400;                           // 50 ms at 48 kHz
    std::vector<float> x(L);
    for (long i = 0; i < L; i++) x[i] = 0.5f * (float)std::sin(2.0 * M_PI * 1000.0 * i / 48000.0);
    writeWavF32(p, 48000, 1, x);
    Rig g;
    ASSERT_TRUE(g.open(p));
    g.s.setLoop(1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    g.runToEnd();
    ASSERT_EQ(g.s.ps(), A::Ps::PASTEND);
    EXPECT_FALSE(g.s.reenable(L));                 // at the end: stays ended
    const int passes = 6;
    const int64_t targets[] = {0, 0, 1700, 2000, L - 10};   // 0, 0, 35, 42 ms, 10 samples before the end
    for (int pass = 2; pass <= passes; pass++) {
        SCOPED_TRACE(pass);
        const int64_t t = targets[pass - 2];
        g.ctx.target = t;
        ASSERT_TRUE(g.s.reenable(t));
        g.s.start(t, A::StartKind::REENABLE);
        ASSERT_TRUE(g.untilPlay());
        g.runToEnd();
        ASSERT_EQ(g.s.ps(), A::Ps::PASTEND);
    }
    EXPECT_EQ((long)(g.tap.v.size() / g.ch), 2 * L + (L - 1700) + (L - 2000) + 10);
    EXPECT_EQ(g.s.invariantViolations(), 0);
    std::remove(p.c_str());
    // a long file keeps the full margin: a target 50 ms before its end stays ended
    Rig h;
    ASSERT_TRUE(h.open(fixturePath("a.wav")));
    h.s.setLoop(1, 0, 0);
    h.s.start(0, A::StartKind::REVEAL);
    h.runToEnd();
    ASSERT_EQ(h.s.ps(), A::Ps::PASTEND);
    EXPECT_FALSE(h.s.reenable(576000 - 2400));
    EXPECT_TRUE(h.s.reenable(576000 - 4181 - 1));
}

// h.2: a job superseded between its steps hands back with soxr FED and dec
// MID (T1, N13); the next start goes through a RESET again.
TEST_F(FeedEngine, AnAbandonedJobLeavesTheStateDirty) {
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    for (int i = 0; i < 20; i++) g.period();
    g.ctx.target = 9600;
    g.s.offsetChanged(9600);                       // fed soxr: RESET
    ASSERT_EQ(g.s.runningOrPendingJob(), A::JobKind::RESET);
    bool fired = false;
    g.s.setWorkerStepHook([&](A::JobKind k, int step) {
        if (k == A::JobKind::RESET && step == 1 && !fired) { fired = true; g.s.stopOutput(); }
    });
    g.s.workerRunPending();
    g.s.serviceWorker(g.ctx);
    g.s.setWorkerStepHook(nullptr);
    EXPECT_TRUE(fired);
    EXPECT_EQ(g.s.ps(), A::Ps::HOLD);
    EXPECT_FALSE(g.s.soxrPristine());
    EXPECT_FALSE(g.s.decAtH());
    g.s.start(9600, A::StartKind::REVEAL);
    EXPECT_EQ(g.s.runningOrPendingJob(), A::JobKind::RESET);
    ASSERT_TRUE(g.untilPlay());
    EXPECT_EQ(g.s.invariantViolations(), 0);
}

// The randomized driver over the (event x state) pairs that live in
// AudioFstream (seeded from the step-0 model's random driver): random
// /offset, /mtcfollow 0/1, MTC loss/return, /loop re-sends, job failures,
// slow workers and MTC jumps. The test plays the step-2 start condition
// (h.3). Invariants, the token, soxr_clear's thread and liveness are checked.
TEST_F(FeedEngine, RandomizedEventsKeepTheInvariants) {
    struct Case { const char* file; int passes; int64_t num; };
    const Case cases[] = {{"a.wav", -1, 0}, {"a.wav", 3, 0}, {"av.mkv", -1, 0}, {"a.wav", -1, 24000},
                          {"a.mp3", -1, 0}, {"a.weba", 2, 0}, {"a_44k.mp3", -1, 0}};
    std::mt19937 rng(10);
    for (const auto& c : cases) {
        for (int trial = 0; trial < 6; trial++) {
            SCOPED_TRACE(std::string(c.file) + " trial " + std::to_string(trial));
            Rig g;
            ASSERT_TRUE(g.open(fixturePath(c.file)));
            g.s.setLoop(c.passes, c.num, c.num ? 48000 : 0);
            bool following = true, live = true;
            A::StartKind kind = A::StartKind::REVEAL;
            long silentDue = 0, maxSilentDue = 0;
            std::uniform_real_distribution<double> u(0, 1);
            for (int step = 0; step < 4000; step++) {
                const double x = u(rng);
                if (x < 0.004) {                                   // E4 (re-GO, legacy per-pass offset)
                    g.ctx.target += (long)(u(rng) * 20 - 3) * 48000;
                    g.s.clearSeekFailed();
                    if (g.s.ps() == A::Ps::PASTEND) {
                        g.s.reenable(g.ctx.target);
                    } else {
                        g.s.offsetChanged(g.ctx.target);
                    }
                    kind = A::StartKind::OFFSET;
                } else if (x < 0.006) {                            // E3
                    following = false;
                    g.s.stopOutput();
                } else if (x < 0.008) {                            // E2
                    following = true;
                    g.s.clearSeekFailed();
                    if (g.s.ps() == A::Ps::PASTEND) g.s.reenable(g.ctx.target);
                    kind = A::StartKind::REVEAL;
                } else if (x < 0.010) {                            // E7
                    live = false;
                    g.s.stopOutput();
                } else if (x < 0.012) {                            // E6
                    live = true;
                    kind = A::StartKind::RETURN;
                } else if (x < 0.013) {                            // E5
                    const int p[] = {-1, 1, 2, 3};
                    g.s.setLoop(p[rng() % 4], c.num, c.num ? 48000 : 0);
                    if (g.s.ps() == A::Ps::PASTEND) g.s.reenable(g.ctx.target);
                } else if (x < 0.014) {                            // E16 (injected)
                    const A::JobKind k[] = {A::JobKind::SEEK, A::JobKind::WRAP, A::JobKind::RESET};
                    g.s.failNextJob(k[rng() % 3]);
                } else if (x < 0.015) {                            // an MTC jump
                    g.ctx.target += (long)(u(rng) * 8 - 4) * 48000;
                }
                g.runWorker = u(rng) < 0.7;                                          // a slow worker
                g.ctx.mtcLive = live;
                // h.3's start condition (step 2's)
                if (g.s.ps() == A::Ps::HOLD && following && live && g.ctx.target >= 0 && !g.s.seekFailedLatched())
                    g.s.start(g.ctx.target, kind);
                g.period();
                const bool due = following && live && g.ctx.target >= 0 && g.ctx.target < g.s.endSamples() &&
                                 g.s.ps() != A::Ps::PASTEND && !g.s.seekFailedLatched() &&
                                 g.s.loopMode() != A::LoopMode::OFF;
                if (due && !g.s.outPlaying() && g.s.jobsOutstanding() == 0) silentDue++; else silentDue = 0;
                maxSilentDue = std::max(maxSilentDue, silentDue);
                ASSERT_LE(g.s.jobsOutstanding(), 2);
                ASSERT_GE(g.s.origin(), 0);
                ASSERT_GE(g.s.passIndex(), 0);
            }
            EXPECT_EQ(g.s.invariantViolations(), 0);
            EXPECT_EQ(g.s.soxrClearsOutsideWorker(), 0);
            EXPECT_LE(maxSilentDue, 2 * 48000 / 512);   // liveness: the budget + 1 s
        }
    }
}
