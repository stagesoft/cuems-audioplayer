// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// The always-on VR soxr and its demand feeding (plan 869ejxng3 +
// 1amfwm7e6cw rev 3, section 4.3b, the feeding bullets of 4.3g): every
// period returns exactly N frames, the FIFO health metric sits at 241, pos
// = origin + sum(r N) + c matches the real output (impulse trains, +-2000
// ppm wobble), pos is exact after a seek and after a RESET, the sums
// restart at every clear (I7), soxr_clear never runs outside the worker
// (L10), and the end drain delivers the tail.

#include <gtest/gtest.h>
#include <algorithm>
#include "feed_test_util.h"

using namespace feedtest;

namespace {

// An impulse every `spacing` samples from `first`, mono float WAV.
std::string impulseWav(unsigned rate, long seconds, long first, long spacing)
{
    std::vector<float> x((size_t)rate * seconds, 0.0f);
    for (size_t p = first; p < x.size(); p += spacing) x[p] = 1.0f;
    const std::string path = tmpPath("imp" + std::to_string(rate));
    writeWavF32(path, rate, 1, x);
    return path;
}

struct PosCheck { int peaks = 0; double maxRes = 0; double meanRes = 0; };

// Plays the rig for `periods`, with the ratio wobbling by `ppm`; returns the
// residual of every output impulse against pos's prediction.
PosCheck playAndCheckImpulses(Rig& g, int periods, double ppm, long first, long spacing)
{
    std::vector<float> all;
    std::vector<double> posAt, rAt;
    for (int k = 0; k < periods && g.s.ps() == A::Ps::PLAY; k++) {
        const double r = g.r * (1.0 + ppm * 1e-6 * std::sin(2.0 * M_PI * k / 400.0));
        posAt.push_back(g.s.pos());
        rAt.push_back(r);
        g.period(r);
        all.insert(all.end(), g.out.begin(), g.out.end());
    }
    PosCheck c;
    double sum = 0;
    for (size_t q = 1; q + 1 < all.size(); q++) {
        if (all[q] > 0.5f && all[q] >= all[q - 1] && all[q] > all[q + 1]) {
            const double y0 = all[q - 1], y1 = all[q], y2 = all[q + 1];
            const double qq = q + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
            const size_t k = q / g.N;
            const double pred = posAt[k] + (qq - (double)k * g.N) * rAt[k];
            const double nearest = first + std::round((pred - first) / spacing) * spacing;
            const double res = pred - nearest;
            c.maxRes = std::max(c.maxRes, std::fabs(res));
            sum += res;
            c.peaks++;
        }
    }
    c.meanRes = c.peaks ? sum / c.peaks : 0;
    return c;
}

bool needGate() { return !gateDir().empty(); }

}  // namespace

// Demand feeding: every period exactly N frames over 10 min of periods with
// the ratio wobbling +-2000 ppm, at 48 -> 48 and 44.1 -> 48; FIFO 241 +- 4.
TEST(SoxrFeeding, EveryPeriodIsExactlyNFramesForTenMinutes) {
    if (!needGate()) GTEST_SKIP() << "STEP0_GATE_DIR not set";
    for (const char* n : {"a.wav", "a_44k.wav"}) {
        SCOPED_TRACE(n);
        Rig g;
        ASSERT_TRUE(g.open(fixturePath(n)));
        g.s.setLoop(-1, 0, 0);
        g.s.start(0, A::StartKind::REVEAL);
        double fmin = 1e9, fmax = -1e9;
        const long periods = 10L * 60 * 48000 / 512;
        for (long k = 0; k < periods; k++) {
            g.period(g.r * (1.0 + 2000e-6 * std::sin(2.0 * M_PI * k / 3000.0)));
            if (k > 100) { fmin = std::min(fmin, g.s.fifo()); fmax = std::max(fmax, g.s.fifo()); }
        }
        EXPECT_EQ(g.shortPeriods, 0);
        EXPECT_GE(fmin, 241.0 - 4.0);
        EXPECT_LE(fmax, 241.0 + 4.0);
        EXPECT_EQ(g.s.invariantViolations(), 0);
        EXPECT_EQ(g.count(A::EvType::HANDOVER_LATE), 0);
    }
}

// pos against the real output: impulse trains at 48 and 44.1 kHz, the ratio
// steady and wobbling +-2000 ppm; residual <= 4 input frames (c calibrated
// at prepareFeed: measured -2.0 at 48 -> 48 and +0.2 at 44.1 -> 48).
TEST(SoxrFeeding, PosMatchesTheOutputUnderWobble) {
    for (unsigned rate : {48000u, 44100u}) {
        for (double ppm : {0.0, 2000.0}) {
            SCOPED_TRACE(std::to_string(rate) + " Hz, " + std::to_string((int)ppm) + " ppm");
            const std::string path = impulseWav(rate, 20, 2000, 3001);
            Rig g;
            ASSERT_TRUE(g.open(path));
            g.s.setLoop(1, 0, 0);
            g.s.start(0, A::StartKind::REVEAL);
            const PosCheck c = playAndCheckImpulses(g, 1500, ppm, 2000, 3001);
            unlink(path.c_str());
            EXPECT_GT(c.peaks, 200);
            EXPECT_LE(c.maxRes, 4.0);
            if (rate == 48000) EXPECT_NEAR(g.s.posConstant(), -2.0, 1.0);
            else EXPECT_NEAR(g.s.posConstant(), 0.2, 1.0);
        }
    }
}

// pos exact (+-1 frame) after a worker seek (the clear) and after a cache
// start that went through a RESET; the FIFO metric restarts at each (I7).
TEST(SoxrFeeding, PosIsExactAfterASeekAndAfterAReset) {
    for (unsigned rate : {48000u, 44100u}) {
        for (int which : {0, 1}) {
            SCOPED_TRACE(std::to_string(rate) + (which ? " reset" : " seek"));
            const std::string path = impulseWav(rate, 20, 2000, 3001);
            Rig g;
            ASSERT_TRUE(g.open(path));
            g.s.setLoop(1, 0, 0);
            g.s.start(0, A::StartKind::REVEAL);
            for (int k = 0; k < 60; k++) g.period();
            const int64_t t = which ? (int64_t)(0.31 * rate) : (int64_t)(5.3 * rate);
            g.ctx.target = t;
            g.s.offsetChanged(t);
            ASSERT_TRUE(g.untilPlay());
            const PosCheck c = playAndCheckImpulses(g, 400, 500.0, 2000, 3001);
            unlink(path.c_str());
            EXPECT_GT(c.peaks, 50);
            EXPECT_LE(c.maxRes, 1.0);
            EXPECT_NEAR(g.s.fifo(), 241.0, 4.0);
        }
    }
}

// After two hard seeks the FIFO metric still reads 241 (the sums restart at
// the clear; without the restart it falls by 241 per seek, I7).
TEST(SoxrFeeding, FifoAfterTwoSeeksStillReads241) {
    if (!needGate()) GTEST_SKIP() << "STEP0_GATE_DIR not set";
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav")));
    g.s.setLoop(-1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    for (int k = 0; k < 100; k++) g.period();
    for (int64_t t : {(int64_t)(4 * 48000), (int64_t)(8 * 48000)}) {
        g.ctx.target = t;
        g.s.offsetChanged(t);
        ASSERT_TRUE(g.untilPlay());
        for (int k = 0; k < 200; k++) g.period();
        EXPECT_NEAR(g.s.fifo(), 241.0, 4.0);
    }
    EXPECT_EQ(g.count(A::EvType::SEEK_PICKUP), 2);
}

// L10: soxr_clear (13-43 allocations) never runs outside the worker, also
// with the real worker thread through seeks, resets and wraps.
TEST(SoxrFeeding, SoxrClearRunsOnlyOnTheWorker) {
    if (!needGate()) GTEST_SKIP() << "STEP0_GATE_DIR not set";
    Rig g;
    ASSERT_TRUE(g.open(fixturePath("a.wav"), true));
    g.runWorker = false;
    g.s.setLoop(-1, 0, 0);
    g.s.start(0, A::StartKind::REVEAL);
    for (int k = 0; k < 3000; k++) {
        if (k % 500 == 250) { g.ctx.target = (k % 1000 == 250) ? 9600 : 5 * 48000; g.s.offsetChanged(g.ctx.target); }
        g.period();
        usleep(100);
    }
    usleep(20000);
    g.s.serviceWorker(g.ctx);
    EXPECT_EQ(g.s.soxrClearsOutsideWorker(), 0);
    EXPECT_EQ(g.s.invariantViolations(), 0);
    EXPECT_GE(g.count(A::EvType::SEEK_PICKUP) + g.count(A::EvType::CACHE_START), 6);
}

// The end drain delivers the tail: a play-once cue resampled 48 -> 44.1 kHz
// produces L x 44.1 / 48 frames (+-2), then zero-fill.
TEST(SoxrFeeding, TheEndDrainDeliversTheTail) {
    if (!needGate()) GTEST_SKIP() << "STEP0_GATE_DIR not set";
    for (unsigned outRate : {44100u, 48000u}) {
        SCOPED_TRACE(outRate);
        Rig g;
        ASSERT_TRUE(g.open(fixturePath("a.wav"), false, outRate));
        g.s.setLoop(1, 0, 0);
        g.s.start(0, A::StartKind::REVEAL);
        long produced = 0;
        for (long k = 0; k < 4000 && !(g.s.ps() == A::Ps::PASTEND && g.s.feed() == A::Feed::ENDED); k++) {
            g.period();
            produced += (long)g.s.lastPeriodProduced();
        }
        const double expected = 576000.0 * outRate / 48000.0;
        EXPECT_LE(std::fabs(produced - expected), 2.0) << "produced " << produced;
    }
}
