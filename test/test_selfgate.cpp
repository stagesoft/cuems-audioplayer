// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// CAPS `selfgate` (cuems-RELATIONS Plans/2026-11-03-audio-first-pass-head-869feynrj.md,
// section 4.1 b-g, the player rows of section 7.1): the padded sub-period start,
// the equal-start no-op, /cancelstart and its [OK] CANCEL ack, the [OK] START line,
// the CAPS and LATENCY lines. Driven through PlayerCore and the real AudioFstream
// feed engine with an exact fake clock: the MTC ticks lie on the 10 ms quarter-frame
// grid and carry the exact time, so the player's target is a known linear function
// of the period index and a start can be placed at any sample of a period.

#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "dayalign.h"
#include "feed_test_util.h"
#include "playercore.h"

using A = AudioFstream;

namespace {

const unsigned OUT = 48000, N = 512;

// Day alignment against the fake head: always authoritative, no day shift
bool sameDay(void*, long engineMs, long& localMs, long& k)
{
    localMs = engineMs;
    k = 0;
    return true;
}

enum class Content { RAMP, SINE };

struct SgRig {
    unsigned fileRate;
    std::string path;
    std::vector<float> content;
    std::unique_ptr<A> f;
    std::unique_ptr<PlayerCore> c;
    int64_t k = 0;                       // index of the next period
    int64_t headBaseMs = 3600000;        // the MTC time at now = 0
    bool mtcRunning = true;
    TickSample tick;
    long head = 0;
    std::vector<float> out = std::vector<float>(N);
    std::vector<float> rec;              // every output frame (mono)
    std::vector<size_t> periodAt;        // rec index of each period's first frame
    std::vector<LogEvent> log;
    bool runWorker = true, blockWrap = false;
    int64_t S = 0;                       // the cue start sent (/start_tod, ms)

    SgRig(unsigned rate = 48000, double seconds = 3.0, Content kind = Content::RAMP, int passes = 1)
        : fileRate(rate)
    {
        path = feedtest::tmpPath("selfgate");
        content.resize((size_t)(seconds * rate));
        for (size_t i = 0; i < content.size(); i++) {
            content[i] = kind == Content::RAMP
                ? 0.25f + 0.5f * (float)(i % rate) / (float)rate               // non-zero from sample 0
                : 0.5f * (float)std::sin(2 * M_PI * 100.0 * (double)i / rate);  // smooth: a cut is a click
        }
        feedtest::writeWavF32(path, rate, 1, content);
        f.reset(new A());
        f->open(path, std::ios::binary | std::ios::in);
        f->prepareFeed(OUT, N, false);
        c.reset(new PlayerCore(*f, OUT, N));
        c->setEndWaitMs(__LONG_MAX__);
        DayResolver dr;
        dr.fn = &sameDay;
        c->setDayResolver(dr);
        loop(passes);
        period(3);                       // anchored ticks: LIVE
    }
    ~SgRig() { unlink(path.c_str()); }

    int64_t nowAt(int64_t kk) const { return (int64_t)std::llround((double)kk * N * 1e9 / OUT); }
    double ratio() const { return (double)fileRate / OUT; }
    void latch(OscEvent::Type t, int32_t a = 0, double fv = 0, int32_t b = 0, int32_t cc = 0, int nargs = 1)
    {
        OscEvent e;
        e.type = t; e.a = a; e.f = fv; e.b = b; e.c = cc; e.nargs = nargs;
        ASSERT_TRUE(c->latch(e));
    }
    void loop(int passes) { latch(OscEvent::LOOP, passes); }
    void follow(bool on = true) { latch(OscEvent::MTCFOLLOW, on ? 1 : 0); }
    void cancel(int seq, int fadeMs = -1)
    {
        if (fadeMs < 0) latch(OscEvent::CANCELSTART, seq, 0, 0, 0, 1);
        else latch(OscEvent::CANCELSTART, seq, 0, fadeMs, 0, 2);
    }
    void period(int n = 1)
    {
        for (int i = 0; i < n; i++) {
            const int64_t now = nowAt(k);
            if (mtcRunning) {
                // the latest quarter frame at or before now, carrying the exact time
                const int64_t grid = (now / 10000000) * 10000000;
                const long h = (long)(headBaseMs + grid / 1000000);
                if (tick.seq == 0 || h != tick.headMs) {
                    tick.monoNs = grid;
                    tick.headMs = h;
                    tick.anchored = true;
                    tick.complete = true;
                    tick.seq++;
                }
                head = h;
            }
            c->callback(out.data(), N, now, head, tick);
            if (runWorker && !(blockWrap && f->runningOrPendingJob() == A::JobKind::WRAP)) f->workerRunPending();
            periodAt.push_back(rec.size());
            rec.insert(rec.end(), out.begin(), out.end());
            LogEvent e;
            while (c->popLog(e)) log.push_back(e);
            k++;
        }
    }
    void ms(double msec) { period((int)std::ceil(msec * OUT / 1000.0 / N)); }
    void runTo(int64_t kk) { if (kk > k) period((int)(kk - k)); }

    // The cue start (/start_tod) and the latency such that the target of period
    // kk is exactly t file samples: sample 0 then falls at output frame
    // round(-t / r) of that period.
    void planTarget(int64_t kk, int64_t t)
    {
        const long double xStar = (long double)t * 1e9L / fileRate;
        const long double base = (long double)headBaseMs * 1e6L + (long double)nowAt(kk) - xStar;
        S = (int64_t)std::ceil((double)((base + 30e6L) / 1e6L));
        c->setLatencyNs((int64_t)std::llround((double)((long double)S * 1e6L - base)));
        latch(OscEvent::START_TOD, (int32_t)(S % dayalign::DAY_MS));
    }

    const LogEvent* first(LogEvent::Type t) const
    {
        for (auto& e : log) if (e.type == t) return &e;
        return nullptr;
    }
    int count(LogEvent::Type t) const { int n = 0; for (auto& e : log) n += e.type == t; return n; }
    std::vector<const LogEvent*> all(LogEvent::Type t) const
    {
        std::vector<const LogEvent*> v;
        for (auto& e : log) if (e.type == t) v.push_back(&e);
        return v;
    }
    // first frame at or after `from` whose magnitude exceeds `thr`; -1 if none
    long firstNonZero(size_t from = 0, double thr = 1e-4) const
    {
        for (size_t i = from; i < rec.size(); i++) if (std::fabs(rec[i]) > thr) return (long)i;
        return -1;
    }
    long lastNonZero(double thr = 1e-6) const
    {
        for (size_t i = rec.size(); i-- > 0;) if (std::fabs(rec[i]) > thr) return (long)i;
        return -1;
    }
    double maxStep(size_t from = 1) const
    {
        double m = 0;
        for (size_t i = std::max<size_t>(from, 1); i < rec.size(); i++) m = std::max(m, (double)std::fabs(rec[i] - rec[i - 1]));
        return m;
    }
};

// What a planned crossing produced
struct Crossing {
    long firstNz = -1;          // first non-zero frame, relative to the crossing period
    int64_t K = 0;              // the expected padding, output frames
    int64_t d = 0;              // file samples before sample 0 at the crossing period
    int starts = 0;             // START lines
    int64_t startTarget = 0;    // the START line's target
    int64_t startRate = 0;
    int64_t startTod = 0;
    int follows = 0;            // FOLLOW lines
    int64_t followTarget = 0;   // the target at the follow
    int64_t S = 0;
    int64_t origin = -1;
    double eAfter = 0;          // the lock error 6 periods after the crossing
};

// kOff: the output frame of the crossing period where sample 0 belongs
Crossing runCrossing(unsigned rate, int64_t kOff, bool lossDuringWait = false)
{
    SgRig r(rate);
    Crossing x;
    x.d = std::llround((double)kOff * r.ratio());
    x.K = std::llround((double)x.d / r.ratio());
    const int64_t K0 = r.k + 60;
    r.planTarget(K0, -x.d);
    r.period(1);
    r.follow();
    r.period(1);
    if (lossDuringWait) {
        // 200 ms of MTC loss while the player waits (LOST, then RETURN relabels the start's kind)
        r.mtcRunning = false;
        r.ms(200);
        r.mtcRunning = true;
    }
    r.runTo(K0);
    r.period(6);
    x.eAfter = r.c->errorMs();
    const long nz = r.firstNonZero();
    x.firstNz = nz < 0 ? -1 : nz - (long)r.periodAt[(size_t)K0];
    x.starts = r.count(LogEvent::START);
    if (const LogEvent* s = r.first(LogEvent::START)) {
        x.startTod = s->a; x.startTarget = s->b; x.startRate = s->c;
    }
    x.S = r.S;
    x.origin = r.f->origin();
    x.follows = r.count(LogEvent::FOLLOW);
    if (const LogEvent* fl = r.first(LogEvent::FOLLOW)) x.followTarget = fl->a;
    if (lossDuringWait) {
        EXPECT_GE(r.count(LogEvent::MTC_LOST), 1);
        EXPECT_GE(r.count(LogEvent::MTC_RETURN), 2);   // the rig's own start, then the return
    }
    return x;
}

}  // namespace

////////////////////////////////////////////////////////////
// b. the padded sub-period start
////////////////////////////////////////////////////////////
class PaddedStart : public ::testing::TestWithParam<std::tuple<unsigned, int>> {};

TEST_P(PaddedStart, SampleZeroAtItsFrame)
{
    const unsigned rate = std::get<0>(GetParam());
    const int kOff = std::get<1>(GetParam());
    const Crossing ref = runCrossing(rate, 0);     // today's start at an exact period boundary
    const Crossing x = runCrossing(rate, kOff);
    ASSERT_GE(ref.firstNz, 0);
    EXPECT_LE(std::labs(ref.firstNz), 3) << "the boundary start itself";
    ASSERT_GE(x.firstNz, 0) << "nothing sounded";
    EXPECT_NEAR((double)x.firstNz, (double)(x.K + ref.firstNz), 3.0) << "k=" << kOff << " K=" << x.K;
    EXPECT_EQ(x.origin, 0) << "file position 0";
    EXPECT_EQ(x.starts, 1);
    EXPECT_EQ(x.startTarget, -x.d) << "the START line's target: the period's target, signed";
    EXPECT_EQ(x.startRate, (int64_t)rate);
    EXPECT_EQ(x.startTod, x.S % dayalign::DAY_MS);
    EXPECT_EQ(x.follows, 1) << "one FOLLOW line, at the follow";
    EXPECT_LT(x.followTarget, -x.d) << "the follow landed before the crossing period";
    // the lock loop sees today's constant (|d - r K| <= r / 2 samples)
    EXPECT_NEAR(x.eAfter, ref.eAfter, 0.03) << "e after the start";
}

INSTANTIATE_TEST_SUITE_P(Offsets, PaddedStart,
                         ::testing::Combine(::testing::Values(48000u, 44100u), ::testing::Values(0, 1, 255, 511)));

TEST(Selfgate, PaddedAfterMtcLossDuringTheWait)
{
    const Crossing ref = runCrossing(48000, 0);
    const Crossing x = runCrossing(48000, 255, true);
    ASSERT_GE(x.firstNz, 0);
    EXPECT_NEAR((double)x.firstNz, (double)(x.K + ref.firstNz), 3.0) << "kind RETURN keeps the padded start";
    EXPECT_EQ(x.origin, 0);
    EXPECT_EQ(x.startTarget, -x.d);
}

TEST(Selfgate, TargetAlreadyPastZeroAtTheFollowIsTodaysStart)
{
    SgRig r(48000);
    const int64_t K0 = r.k + 40;
    r.planTarget(K0, 1000);
    r.runTo(K0);
    r.follow();                      // applied in period K0: the target is 1000 already
    r.period(4);
    EXPECT_EQ(r.f->origin(), 1000) << "today's start: from the target";
    ASSERT_EQ(r.count(LogEvent::START), 1);
    EXPECT_EQ(r.first(LogEvent::START)->b, 1000) << "the head lost, as a positive target";
}

////////////////////////////////////////////////////////////
// c. an equal start is a no-op
////////////////////////////////////////////////////////////
TEST(Selfgate, EqualStartTodInPlayIsANoOp)
{
    SgRig r(48000);
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0 + 10);
    ASSERT_TRUE(r.f->ps() == A::Ps::PLAY);
    const long starts = r.c->starts();
    const size_t from = r.rec.size();
    r.latch(OscEvent::START_TOD, (int32_t)(r.S % dayalign::DAY_MS));   // the same start again
    r.period(6);
    EXPECT_EQ(r.c->starts(), starts) << "no new start";
    EXPECT_EQ(r.c->startsEqual(), 1);
    EXPECT_EQ(r.count(LogEvent::START_EQUAL), 1);
    EXPECT_TRUE(r.f->ps() == A::Ps::PLAY);
    for (size_t i = from; i < r.rec.size(); i++) ASSERT_GT(std::fabs(r.rec[i]), 0.2f) << "a gap at " << i;
    // a different one: today's E4 (a new start)
    r.latch(OscEvent::START_TOD, (int32_t)((r.S + 5) % dayalign::DAY_MS));
    r.period(6);
    EXPECT_EQ(r.c->starts(), starts + 1);
}

TEST(Selfgate, LegacyOffsetSequenceAsToday)
{
    SgRig r(48000);
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0 + 10);
    const long starts = r.c->starts();
    // the legacy per-pass re-anchor sends a new value every time
    r.latch(OscEvent::OFFSET_F, 0, -(double)(r.S + 40));
    r.period(4);
    r.latch(OscEvent::OFFSET_F, 0, -(double)(r.S + 80));
    r.period(4);
    EXPECT_EQ(r.c->starts(), starts + 2);
    EXPECT_EQ(r.c->startsEqual(), 0);
}

////////////////////////////////////////////////////////////
// d. /cancelstart
////////////////////////////////////////////////////////////
TEST(Selfgate, CancelWhileFollowingInHoldIsSilent)
{
    SgRig r(48000);
    const int64_t K0 = r.k + 40;
    r.planTarget(K0, -255);
    r.follow();
    r.runTo(K0 - 5);
    r.cancel(7);
    r.period(30);                    // well past the start
    EXPECT_LT(r.firstNonZero(0, 1e-7), 0) << "silent past the start";
    EXPECT_FALSE(r.c->following());
    EXPECT_TRUE(r.f->ps() == A::Ps::HOLD);
    ASSERT_EQ(r.count(LogEvent::CANCEL), 1);
    const LogEvent* e = r.first(LogEvent::CANCEL);
    EXPECT_EQ(e->a, 7);
    EXPECT_EQ(e->b, 0) << "hold";
    EXPECT_EQ(e->c, 0);
    EXPECT_EQ(r.count(LogEvent::START), 0);
}

// The fade of a cancel in PLAY: ~fade_ms to silence, no step above the content's
// own plus the fade's slope, then HOLD, not following, soxr cleared by a RESET.
static void checkPlayCancel(SgRig& r, int fadeMs, int seq, long at = -1)
{
    const size_t cancelAt = at >= 0 ? (size_t)at : r.rec.size();   // the period that applies the cancel
    const double fadeFrames = fadeMs * (double)OUT / 1000.0;
    r.period(20);
    ASSERT_EQ(r.count(LogEvent::CANCEL), 1);
    const LogEvent* e = r.first(LogEvent::CANCEL);
    EXPECT_EQ(e->a, seq);
    EXPECT_EQ(e->b, 1) << "play";
    EXPECT_GT(e->c, 0) << "ms of output";
    const long last = r.lastNonZero();
    ASSERT_GE(last, (long)cancelAt);
    EXPECT_NEAR((double)(last - (long)cancelAt), fadeFrames, 0.15 * fadeFrames + 4) << "fade_ms honoured";
    // no click: the content's own step (100 Hz, 0.5) plus the fade's (0.5 / fade frames)
    const double bound = 0.5 * 2 * M_PI * 100.0 / OUT * std::max(1.0, r.ratio()) + 0.5 / std::max(1.0, fadeFrames) + 2e-3;
    EXPECT_LE(r.maxStep(cancelAt), bound);
    EXPECT_FALSE(r.c->following());
    EXPECT_TRUE(r.f->ps() == A::Ps::HOLD);
    EXPECT_TRUE(r.f->soxrPristine()) << "the RESET ran";
    EXPECT_EQ(r.f->invariantViolations(), 0);
}

TEST(Selfgate, CancelInPlayFadesToSilence)
{
    for (int afterPeriods : {1, 2, 4}) {     // 1-50 ms after the start (1 = in SETTLE)
        SCOPED_TRACE(afterPeriods);
        SgRig r(48000, 3.0, Content::SINE);
        const int64_t K0 = r.k + 30;
        r.planTarget(K0, -255);
        r.follow();
        r.runTo(K0 + afterPeriods);
        ASSERT_TRUE(r.f->ps() == A::Ps::PLAY);
        r.cancel(11);
        checkPlayCancel(r, 5, 11);
    }
}

TEST(Selfgate, CancelAndStartInTheSamePeriodOrderedByArrival)
{
    {   // the cancel arrives before the period whose callback would start: silent
        SgRig r(48000);
        const int64_t K0 = r.k + 30;
        r.planTarget(K0, -255);
        r.follow();
        r.runTo(K0);
        r.cancel(3);
        r.period(10);
        EXPECT_LT(r.firstNonZero(0, 1e-7), 0);
        ASSERT_EQ(r.count(LogEvent::CANCEL), 1);
        EXPECT_EQ(r.first(LogEvent::CANCEL)->b, 0);
    }
    {   // the follow and the cancel latched together, follow first: silent
        SgRig r(48000);
        const int64_t K0 = r.k + 30;
        r.planTarget(K0, -255);
        r.runTo(K0);
        r.follow();
        r.cancel(4);
        r.period(10);
        EXPECT_LT(r.firstNonZero(0, 1e-7), 0);
        EXPECT_EQ(r.first(LogEvent::CANCEL)->b, 0);
    }
    {   // the cancel arrives right after the starting period: a fade
        SgRig r(48000, 3.0, Content::SINE);
        const int64_t K0 = r.k + 30;
        r.planTarget(K0, -255);
        r.follow();
        r.runTo(K0 + 1);
        r.cancel(5);
        checkPlayCancel(r, 5, 5);
    }
}

TEST(Selfgate, CancelMidFileAtRatioNotOne)
{
    SgRig r(44100, 3.0, Content::SINE);
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0);
    r.ms(1500);                      // past the head cache: the decoder feeds
    ASSERT_TRUE(r.f->ps() == A::Ps::PLAY);
    ASSERT_TRUE(r.f->feed() == A::Feed::DECODER);
    r.cancel(21);
    checkPlayCancel(r, 5, 21);
    EXPECT_NEAR((double)r.first(LogEvent::CANCEL)->c, 1500.0, 30.0);
}

TEST(Selfgate, CancelHonoursFadeMs)
{
    SgRig r(48000, 3.0, Content::SINE);
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0 + 20);
    r.cancel(31, 20);
    checkPlayCancel(r, 20, 31);
}

TEST(Selfgate, CancelDuringTheWrapSwitch)
{
    SgRig r(48000, 2.0, Content::SINE, -1);     // loop = -1, the file longer than the head cache
    r.blockWrap = true;
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0);
    for (int i = 0; i < 400 && r.f->runningOrPendingJob() != A::JobKind::WRAP; i++) r.period(1);
    ASSERT_TRUE(r.f->runningOrPendingJob() == A::JobKind::WRAP) << "the in-player wrap switch";
    ASSERT_TRUE(r.f->ps() == A::Ps::PLAY);
    const long at = (long)r.rec.size();
    r.cancel(41);
    r.period(1);                     // applied with the WRAP still pending: the RESET replaces it
    r.blockWrap = false;
    checkPlayCancel(r, 5, 41, at);
}

TEST(Selfgate, CancelWhileSeekingIsHold)
{
    SgRig r(48000, 3.0, Content::SINE);
    r.runWorker = false;
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 2 * 48000);     // 2 s in: past the cache, a SEEK
    r.runTo(K0);
    r.follow();
    r.period(1);
    ASSERT_TRUE(r.f->ps() == A::Ps::SEEKING);
    r.cancel(51);
    r.period(1);
    r.runWorker = true;
    r.period(10);
    EXPECT_TRUE(r.f->ps() == A::Ps::HOLD);
    EXPECT_FALSE(r.c->following());
    EXPECT_LT(r.firstNonZero(0, 1e-7), 0);
    ASSERT_EQ(r.count(LogEvent::CANCEL), 1);
    EXPECT_EQ(r.first(LogEvent::CANCEL)->b, 0) << "nothing sounded: hold";
}

TEST(Selfgate, CancelDuringAResetJobIsHold)
{
    SgRig r(48000, 3.0, Content::SINE);
    // a first pass, stopped: soxr is not pristine any more
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0 + 5);
    r.follow(false);
    r.period(1);
    ASSERT_FALSE(r.f->soxrPristine());
    // a re-start with the RESET still outstanding
    r.runWorker = false;
    r.planTarget(r.k + 2, 100);
    r.follow();
    r.period(3);
    ASSERT_TRUE(r.f->ps() == A::Ps::SEEKING);
    const size_t from = r.rec.size();
    r.cancel(52);
    r.period(1);
    r.runWorker = true;
    r.period(10);
    EXPECT_TRUE(r.f->ps() == A::Ps::HOLD);
    EXPECT_LT(r.firstNonZero(from, 1e-7), 0);
    ASSERT_EQ(r.count(LogEvent::CANCEL), 1);
    EXPECT_EQ(r.first(LogEvent::CANCEL)->b, 0);
}

TEST(Selfgate, CancelInPastEnd)
{
    SgRig r(48000, 1.0, Content::SINE, 1);       // once, all in the cache
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0);
    r.ms(1100);                                  // past the end, the drain done
    ASSERT_TRUE(r.f->ps() == A::Ps::PASTEND);
    const size_t from = r.rec.size();
    r.cancel(61);
    r.period(5);
    ASSERT_EQ(r.count(LogEvent::CANCEL), 1);
    EXPECT_EQ(r.first(LogEvent::CANCEL)->b, 2) << "pastend";
    EXPECT_FALSE(r.c->following());
    EXPECT_LT(r.firstNonZero(from, 1e-7), 0) << "nothing to fade";
}

TEST(Selfgate, SecondCancelDuringTheFadeChangesNothing)
{
    SgRig r(48000, 3.0, Content::SINE);
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0 + 10);
    r.cancel(71, 20);
    r.period(1);                     // the fade runs (960 frames)
    r.cancel(72, 0);
    r.period(10);
    const auto acks = r.all(LogEvent::CANCEL);
    ASSERT_EQ(acks.size(), 2u);
    EXPECT_EQ(acks[0]->a, 71);
    EXPECT_EQ(acks[1]->a, 72);
    EXPECT_EQ(acks[1]->b, 1) << "acked play";
    // the second cancel did not cut the fade short
    EXPECT_GT(r.lastNonZero(), (long)r.periodAt[(size_t)(K0 + 10)] + 800);
}

TEST(Selfgate, RefollowAfterACancelIsPaddedAgain)
{
    SgRig r(48000);
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, 0);
    r.follow();
    r.runTo(K0 + 10);
    r.cancel(81);
    r.period(10);                    // fade, HOLD, the RESET
    ASSERT_TRUE(r.f->soxrPristine());
    const int64_t K1 = r.k + 30;
    r.planTarget(K1, -300);
    r.follow();
    const size_t from = r.rec.size();
    r.runTo(K1 + 4);
    const long nz = r.firstNonZero(from);
    ASSERT_GE(nz, 0);
    EXPECT_NEAR((double)(nz - (long)r.periodAt[(size_t)K1]), 300.0, 3.0);
    EXPECT_EQ(r.count(LogEvent::START), 2);
}

////////////////////////////////////////////////////////////
// e. the START line; g. LATENCY; a. CAPS
////////////////////////////////////////////////////////////
TEST(Selfgate, StartLineOncePerEligibleStart)
{
    SgRig r(48000);
    const int64_t K0 = r.k + 30;
    r.planTarget(K0, -100);
    r.follow();
    r.runTo(K0 + 10);
    r.latch(OscEvent::START_TOD, (int32_t)((r.S + 100) % dayalign::DAY_MS));   // E4 in PLAY: a new start
    r.period(6);
    r.follow();                       // a repeated /mtcfollow 1 while playing
    r.period(6);
    EXPECT_GE(r.c->starts(), 2);
    EXPECT_EQ(r.count(LogEvent::START), 1) << "only the first start since the follow";
    // E3 then E2 in HOLD: eligible again
    r.follow(false);
    r.period(1);
    r.planTarget(r.k + 20, -50);
    r.follow();
    r.period(30);
    EXPECT_EQ(r.count(LogEvent::START), 2);
}

TEST(Selfgate, Lines)
{
    LogEvent s;
    s.type = LogEvent::START; s.a = 3601234; s.b = -255; s.c = 44100;
    bool so = false;
    int lvl = 0;
    std::string journal;
    EXPECT_EQ(PlayerCore::describe(s, "p1", &so, &lvl, &journal), "[p1] [OK] START 3601234 -255 44100\n");
    EXPECT_TRUE(so);
    EXPECT_FALSE(journal.empty());
    for (int st = 0; st < 3; st++) {
        LogEvent cl;
        cl.type = LogEvent::CANCEL; cl.a = 9; cl.b = st; cl.c = st ? 42 : 0;
        static const char* names[] = {"hold", "play", "pastend"};
        EXPECT_EQ(PlayerCore::describe(cl, "p1", &so, &lvl, &journal),
                  std::string("[p1] [OK] CANCEL 9 ") + names[st] + (st ? " 42\n" : " 0\n"));
        EXPECT_TRUE(so);
        EXPECT_NE(journal.find("cancelstart"), std::string::npos);
    }
    LogEvent fo;
    fo.type = LogEvent::FOLLOW; fo.a = -4800; fo.b = 3601000; fo.c = 48000;
    EXPECT_EQ(PlayerCore::describe(fo, "p1", &so, &lvl),
              "follow at target -4800 samples = 100.0 ms before sample 0 (head 3601000 ms)");
    EXPECT_FALSE(so);
    EXPECT_EQ(lvl, 6);
    LogEvent eq;
    eq.type = LogEvent::START_EQUAL; eq.a = 3601234;
    PlayerCore::describe(eq, "p1", &so, &lvl);
    EXPECT_FALSE(so);
    EXPECT_EQ(lvl, 7);
    EXPECT_EQ(PlayerCore::capsLine("p1"), "[p1] [OK] CAPS: loop varispeed dayalign selfgate\n");
    EXPECT_EQ(PlayerCore::latencyLine("p1", 1536, 48000, 512, false), "[p1] [OK] LATENCY 1536 48000 512\n");
    EXPECT_EQ(PlayerCore::latencyLine("p1", 1680, 48000, 512, true), "[p1] [OK] LATENCY 1680 48000 512 override\n");
}
