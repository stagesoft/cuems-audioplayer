// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// The table-driven test of plan 869ejxng3 + 1amfwm7e6cw rev 3, section 4.3h
// v2.1, h.3 (step 2): one entry per (event, position state) pair of the
// table, E1-E26 x HOLD / PLAY / SEEKING / PASTEND, driven through
// PlayerCore and the real AudioFstream feed engine with a fake MTC source
// and the fake worker clock. A pair missing from the registry fails the
// build's test (the table cannot drift silently); a pair the table marks
// "-" asserts that the event has no effect there. Seeded from the step-0
// model (cuems-RELATIONS baselines/1amfwm7e6cw-varispeed-probe/step0/
// h3_model_v2.py) and its scenario names.

#include <gtest/gtest.h>
#include <functional>
#include <map>
#include <set>
#include "audioplayer.h"
#include "feed_test_util.h"
#include "mtcreceiver.h"
#include "playercore.h"

using A = AudioFstream;
using lockloop::Mtc;

namespace {

const unsigned RATE = 48000, N = 512;
const int64_t P_NS = 1000000000LL * N / RATE;   // one period
const long FILE_S = 5;

// A fake MTC: the head advances with time while `moving`; every period a
// tick is published, anchored or not; `fresh = false` publishes none (the
// trusted tick ages: SUSPENDED).
struct FakeMtc {
    bool moving = true, anchored = true, fresh = true;
    int64_t mtcNs = 3600LL * 1000000000LL;
    TickSample t;
    void step(int64_t now)
    {
        if (moving) mtcNs += P_NS;
        if (moving && fresh) {
            t.monoNs = now;
            t.headMs = (long)(mtcNs / 1000000);
            t.anchored = anchored;
            t.complete = true;
            t.seq++;
        }
    }
    long head() const { return (long)(mtcNs / 1000000); }
};

// Day alignment against the fake head: always authoritative, the real 12 h rule
bool fakeToLocalDay(void* ctx, long engineMs, long& localMs, long& k)
{
    const FakeMtc* m = static_cast<const FakeMtc*>(ctx);
    k = MtcReceiver::dayShift(engineMs, m->head());
    localMs = engineMs - k * MtcReceiver::DAY_MS;
    return true;
}

struct Rig {
    std::string path;
    std::unique_ptr<A> f;
    std::unique_ptr<PlayerCore> c;
    FakeMtc m;
    int64_t now = 0;
    bool runWorker = true, blockWrap = false;
    std::vector<float> out = std::vector<float>(N);
    std::vector<LogEvent> log;

    Rig(int passes = 1)
    {
        path = feedtest::tmpPath("h3table");
        std::vector<float> x(FILE_S * RATE);
        for (size_t i = 0; i < x.size(); i++) x[i] = 0.2f * (float)std::sin(2 * M_PI * 440.0 * i / RATE);
        feedtest::writeWavF32(path, RATE, 1, x);
        f.reset(new A());
        f->open(path, std::ios::binary | std::ios::in);
        f->prepareFeed(RATE, N, false);
        c.reset(new PlayerCore(*f, RATE, N));
        c->setEndWaitMs(__LONG_MAX__);
        DayResolver dr;
        dr.fn = &fakeToLocalDay;
        dr.ctx = &m;
        c->setDayResolver(dr);
        loop(passes);
        period(3);          // anchored: LIVE
    }
    ~Rig() { unlink(path.c_str()); }
    void latch(OscEvent::Type t, int32_t a = 0, double fv = 0, int32_t b = 0, int32_t cc = 0, int nargs = 1)
    {
        OscEvent e;
        e.type = t; e.a = a; e.f = fv; e.b = b; e.c = cc; e.nargs = nargs;
        c->latch(e);
    }
    void loop(int passes, int32_t num = 0, int32_t den = 0) { latch(OscEvent::LOOP, passes, 0, num, den, num ? 3 : 1); }
    void period(int n = 1)
    {
        for (int i = 0; i < n; i++) {
            now += P_NS;
            m.step(now);
            c->callback(out.data(), N, now, m.head(), m.t);
            if (runWorker && !(blockWrap && f->runningOrPendingJob() == A::JobKind::WRAP)) f->workerRunPending();
            LogEvent e;
            while (c->popLog(e)) log.push_back(e);
        }
    }
    void ms(double msec) { period((int)std::ceil(msec * 1e6 / P_NS)); }
    // the offset such that the target is `cueMs` into the cue now (latency 0)
    void offsetTo(double cueMs) { latch(OscEvent::OFFSET_F, 0, std::floor(cueMs - (double)m.mtcNs / 1e6) - 0); }
    int count(LogEvent::Type t) const { int n = 0; for (auto& e : log) n += e.type == t; return n; }
    void clearLog() { log.clear(); }
    bool playing() const { return f->ps() == A::Ps::PLAY && f->outPlaying(); }

    // the four position states
    // Each returns whether the state was reached; every call site asserts it, so a
    // cell can never pass in a state other than its column.
    bool toPlay(double atMs = 100) { offsetTo(atMs); latch(OscEvent::MTCFOLLOW, 1); period(2); return f->ps() == A::Ps::PLAY; }
    bool toSeeking(double atMs = 3000)
    {
        runWorker = false; offsetTo(atMs); latch(OscEvent::MTCFOLLOW, 1); period(1);
        return f->ps() == A::Ps::SEEKING;
    }
    // past the rig's real end (passes x P_s): needs a finite number of passes
    bool toPastEnd()
    {
        const int64_t e = f->endSamples();
        if (e >= INT64_MAX / 8) return false;
        offsetTo((double)e * 1000.0 / RATE + 800); latch(OscEvent::MTCFOLLOW, 1); period(2);
        return f->ps() == A::Ps::PASTEND;
    }
    void freeze(double msec) { m.moving = false; ms(msec); }
};

struct Entry {
    std::string ev;
    A::Ps ps;
    std::function<void()> fn;
};

const char* EVENTS[] = {"E1", "E2", "E3", "E4", "E5", "E6", "E7", "E8", "E9", "E10", "E11", "E12", "E13",
                        "E14", "E15", "E16", "E17", "E18", "E19", "E20", "E21", "E22", "E23", "E24", "E25", "E26"};
const A::Ps STATES[] = {A::Ps::HOLD, A::Ps::PLAY, A::Ps::SEEKING, A::Ps::PASTEND};

std::vector<Entry> table()
{
    std::vector<Entry> t;
    auto add = [&](const char* ev, A::Ps ps, std::function<void()> fn) { t.push_back({ev, ps, fn}); };
    using P = A::Ps;

    // E1 open(): the initial state; the other columns are unreachable (open() precedes them)
    add("E1", P::HOLD, [] {
        A f;
        std::string p = feedtest::tmpPath("h3e1");
        feedtest::writeWavF32(p, RATE, 1, std::vector<float>(RATE * 2, 0.1f));
        f.open(p, std::ios::binary | std::ios::in);
        ASSERT_TRUE(f.prepareFeed(RATE, N, false));
        PlayerCore c(f, RATE, N);
        EXPECT_EQ(f.ps(), A::Ps::HOLD);
        EXPECT_FALSE(f.outPlaying());
        EXPECT_TRUE(f.soxrPristine());
        EXPECT_TRUE(f.decAtH());
        EXPECT_EQ(c.mtc(), Mtc::LOST);   // the player starts in LOST (J16)
        unlink(p.c_str());
    });
    for (P ps : {P::PLAY, P::SEEKING, P::PASTEND}) add("E1", ps, [] { SUCCEED() << "open() precedes every other state"; });

    // E2 /mtcfollow 1
    add("E2", P::HOLD, [] { Rig r; r.offsetTo(100); r.period(); EXPECT_EQ(r.f->ps(), A::Ps::HOLD);
        r.latch(OscEvent::MTCFOLLOW, 1); r.period(); EXPECT_TRUE(r.c->following()); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });
    add("E2", P::PLAY, [] { Rig r; ASSERT_TRUE(r.toPlay()); long s = r.c->starts(); r.latch(OscEvent::MTCFOLLOW, 1); r.period(3);
        EXPECT_EQ(r.c->starts(), s); EXPECT_TRUE(r.playing()); });
    add("E2", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); uint64_t g = r.f->generation(); r.latch(OscEvent::MTCFOLLOW, 1); r.period();
        EXPECT_EQ(r.f->ps(), A::Ps::SEEKING); EXPECT_EQ(r.f->generation(), g); });
    add("E2", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPastEnd()); ASSERT_EQ(r.f->ps(), A::Ps::PASTEND);
        r.latch(OscEvent::MTCFOLLOW, 0); r.period(); EXPECT_FALSE(r.c->following());
        r.latch(OscEvent::MTCFOLLOW, 1); r.period(); EXPECT_TRUE(r.c->following());   // T4
        EXPECT_EQ(r.f->ps(), A::Ps::PASTEND);                                         // target past the end: no re-enable
        r.offsetTo(1000); r.period(3); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });          // re-enable by E4, then the condition

    // E3 /mtcfollow 0
    add("E3", P::HOLD, [] { Rig r; r.latch(OscEvent::MTCFOLLOW, 0); r.period(); EXPECT_FALSE(r.c->following()); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); });
    add("E3", P::PLAY, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.latch(OscEvent::MTCFOLLOW, 0); r.period();
        EXPECT_EQ(r.f->ps(), A::Ps::HOLD); EXPECT_FALSE(r.f->outPlaying()); EXPECT_FALSE(r.f->soxrPristine()); });
    add("E3", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); uint64_t g = r.f->generation(); r.latch(OscEvent::MTCFOLLOW, 0); r.period();
        EXPECT_EQ(r.f->ps(), A::Ps::HOLD); EXPECT_GT(r.f->generation(), g);
        r.f->workerRunPending(); r.period(2); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); });   // the abandoned result picks nothing up
    add("E3", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPastEnd()); r.latch(OscEvent::MTCFOLLOW, 0); r.period();
        EXPECT_FALSE(r.c->following()); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); });

    // E4 /offset, /start_tod (day alignment §7.2: /start_tod replaced /offset_i)
    add("E4", P::HOLD, [] { Rig r; r.latch(OscEvent::MTCFOLLOW, 1); r.offsetTo(-2000); r.period(2); EXPECT_EQ(r.f->ps(), A::Ps::HOLD);
        r.latch(OscEvent::START_TOD, (int32_t)(-(int64_t)std::floor(200 - (double)r.m.mtcNs / 1e6))); r.period(2);
        EXPECT_EQ(r.f->ps(), A::Ps::PLAY); ASSERT_GE(r.count(LogEvent::CACHE_START), 1); });
    add("E4", P::PLAY, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(200); r.clearLog(); r.offsetTo(300); r.period(4);   // into the head: RESET, cache start
        EXPECT_EQ(r.f->ps(), A::Ps::PLAY); EXPECT_EQ(r.count(LogEvent::CACHE_START), 1);
        r.offsetTo(-500); r.period(); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); });                            // t < 0: h.4 step 0
    add("E4", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); uint64_t g = r.f->generation(); r.offsetTo(200); r.period();
        EXPECT_GT(r.f->generation(), g); EXPECT_EQ(r.f->ps(), A::Ps::SEEKING);
        r.runWorker = true; r.period(4); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); EXPECT_EQ(r.f->feed(), A::Feed::CACHE); });
    add("E4", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPastEnd()); r.offsetTo(1000); r.period(3); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });

    // E5 /loop
    add("E5", P::HOLD, [] { Rig r; r.loop(-1); r.period(); EXPECT_EQ(r.f->loopMode(), A::LoopMode::WRAP); EXPECT_GE(r.count(LogEvent::MODE), 1); });
    add("E5", P::PLAY, [] { Rig r(-1); ASSERT_TRUE(r.toPlay()); r.loop(1); r.period(); EXPECT_EQ(r.f->loopMode(), A::LoopMode::ONCE); EXPECT_TRUE(r.playing()); });
    add("E5", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); r.loop(3); r.period(); EXPECT_EQ(r.f->loopMode(), A::LoopMode::WRAP); EXPECT_EQ(r.f->ps(), A::Ps::SEEKING); });
    add("E5", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPastEnd()); r.loop(3); r.period(4);   // passes increased since the end: re-enable
        EXPECT_NE(r.f->ps(), A::Ps::PASTEND); });

    // E6 an anchored tick while LOST / INVALID and the head moves
    add("E6", P::HOLD, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.freeze(200); ASSERT_EQ(r.c->mtc(), Mtc::LOST); ASSERT_EQ(r.f->ps(), A::Ps::HOLD);
        r.m.moving = true; r.period(2); EXPECT_EQ(r.c->mtc(), Mtc::LIVE); r.period(4); EXPECT_EQ(r.f->ps(), A::Ps::PLAY);
        EXPECT_GE(r.count(LogEvent::MTC_RETURN), 1); });
    add("E6", P::PLAY, [] { Rig r; ASSERT_TRUE(r.toPlay()); for (int i = 0; i < 200; i++) { r.period(); if (r.playing()) ASSERT_TRUE(r.c->mtc() == Mtc::LIVE || r.c->mtc() == Mtc::SUSPENDED); } });
    add("E6", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); r.freeze(200); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); });   // E7 leaves SEEKING first
    add("E6", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPastEnd()); r.freeze(200); EXPECT_EQ(r.c->mtc(), Mtc::LOST); r.m.moving = true; r.period(2);
        EXPECT_EQ(r.c->mtc(), Mtc::LIVE); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); });

    // E7 the head frozen for 150 ms
    add("E7", P::HOLD, [] { Rig r; r.freeze(200); EXPECT_EQ(r.c->mtc(), Mtc::LOST); EXPECT_EQ(r.count(LogEvent::MTC_INVALID), 0); });
    add("E7", P::PLAY, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.m.moving = false; int k = 0; while (r.playing() && k++ < 30) r.period();
        EXPECT_EQ(r.c->mtc(), Mtc::LOST); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); EXPECT_LE(k * P_NS / 1000000, 150 + 2 * P_NS / 1000000);
        const double I = r.c->loop().integral(); r.period(5); EXPECT_EQ(r.c->loop().integral(), I); });   // u frozen, I kept
    add("E7", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); uint64_t g = r.f->generation(); r.freeze(200);
        EXPECT_EQ(r.f->ps(), A::Ps::HOLD); EXPECT_GT(r.f->generation(), g); });
    add("E7", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPastEnd()); r.freeze(200); EXPECT_EQ(r.c->mtc(), Mtc::LOST); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); });

    // E8 invalid: 800 ms moving without an anchored tick; ERROR once
    auto invalid = [](Rig& r) { r.m.anchored = false; r.ms(900); };
    add("E8", P::HOLD, [=] { Rig r; invalid(r); EXPECT_EQ(r.c->mtc(), Mtc::INVALID); EXPECT_EQ(r.count(LogEvent::MTC_INVALID), 1); });
    add("E8", P::PLAY, [=] { Rig r; ASSERT_TRUE(r.toPlay()); invalid(r); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); EXPECT_FALSE(r.f->outPlaying());
        EXPECT_EQ(r.count(LogEvent::MTC_INVALID), 1); r.ms(900); EXPECT_EQ(r.count(LogEvent::MTC_INVALID), 1); });
    add("E8", P::SEEKING, [=] { Rig r; ASSERT_TRUE(r.toSeeking()); invalid(r); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); });
    add("E8", P::PASTEND, [=] { Rig r; ASSERT_TRUE(r.toPastEnd()); invalid(r); EXPECT_EQ(r.c->mtc(), Mtc::INVALID); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); });

    // E9 the trusted tick's age crosses 20 ms
    auto suspend = [](Rig& r) { r.m.fresh = false; r.period(3); };
    add("E9", P::HOLD, [=] { Rig r; suspend(r); EXPECT_EQ(r.c->mtc(), Mtc::SUSPENDED); r.m.fresh = true; r.period(); EXPECT_EQ(r.c->mtc(), Mtc::LIVE); });
    add("E9", P::PLAY, [=] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(500); suspend(r);
        ASSERT_EQ(r.c->mtc(), Mtc::SUSPENDED); const double u = r.c->loop().u(); r.period(5);
        EXPECT_TRUE(r.playing()); EXPECT_EQ(r.c->loop().u(), u); });   // the loop holds while SUSPENDED
    add("E9", P::SEEKING, [=] { Rig r; ASSERT_TRUE(r.toSeeking()); suspend(r); EXPECT_EQ(r.c->mtc(), Mtc::SUSPENDED); EXPECT_EQ(r.f->ps(), A::Ps::SEEKING); });
    add("E9", P::PASTEND, [=] { Rig r; ASSERT_TRUE(r.toPastEnd()); suspend(r); EXPECT_EQ(r.c->mtc(), Mtc::SUSPENDED); });

    // E10 raw error > 80 ms on 3 periods (PLAY, not SETTLE, LIVE)
    auto jump = [](Rig& r, double msec) { r.m.mtcNs += (int64_t)(msec * 1e6); };
    add("E10", P::PLAY, [=] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(300); r.clearLog(); jump(r, 200); r.period(5);
        EXPECT_EQ(r.count(LogEvent::RESYNC), 1); EXPECT_TRUE(r.playing()); EXPECT_EQ(r.c->resyncs(), 1); });
    for (P ps : {P::HOLD, P::SEEKING, P::PASTEND}) add("E10", ps, [=] { Rig r;
        if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd()); jump(r, 200); r.period(5);
        EXPECT_EQ(r.count(LogEvent::RESYNC), 0); });

    // E11 FIFO out of band: unreachable with demand feeding; asserted silent in every state
    for (P ps : STATES) add("E11", ps, [=] { Rig r(ps == P::PASTEND ? 1 : -1); if (ps == P::PLAY) ASSERT_TRUE(r.toPlay()); if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking());
        if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd()); r.ms(3000); EXPECT_EQ(r.count(LogEvent::FIFO_DRIFT), 0); });

    // E12 the pass boundary with wraps left: PLAY only (no feed elsewhere)
    add("E12", P::PLAY, [] { Rig r(-1); ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 * 2 + 500); EXPECT_EQ(r.f->passIndex(), 2); EXPECT_TRUE(r.playing());
        EXPECT_EQ(r.c->starts(), 1); });
    for (P ps : {P::HOLD, P::SEEKING, P::PASTEND}) add("E12", ps, [=] { Rig r(ps == P::PASTEND ? 1 : -1);
        if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd());
        const int64_t pi0 = r.f->passIndex(); r.ms(FILE_S * 1000 + 500);
        EXPECT_EQ(r.f->passIndex(), pi0); EXPECT_EQ(r.f->ps(), ps); });

    // E13 the end: PLAY drains, a SEEK whose chase crossed the end ends too
    add("E13", P::PLAY, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 300); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); EXPECT_EQ(r.f->feed(), A::Feed::ENDED);
        EXPECT_EQ(r.count(LogEvent::END), 1); });
    add("E13", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking(4900)); r.ms(300); r.runWorker = true; r.period(3); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); });
    for (P ps : {P::HOLD, P::PASTEND}) add("E13", ps, [=] { Rig r; if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd()); r.ms(500); EXPECT_EQ(r.count(LogEvent::END), 0); });

    // E14 the handover at H (PLAY only)
    add("E14", P::PLAY, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(1300); EXPECT_EQ(r.f->feed(), A::Feed::DECODER); EXPECT_EQ(r.count(LogEvent::HANDOVER_LATE), 0); });
    for (P ps : {P::HOLD, P::SEEKING, P::PASTEND}) add("E14", ps, [=] { Rig r; if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd());
        r.ms(1300); EXPECT_NE(r.f->feed(), A::Feed::DECODER); });

    // E15 job done
    add("E15", P::HOLD, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(100); r.offsetTo(-500); r.period(3);   // step 0: RESET in HOLD
        EXPECT_EQ(r.f->ps(), A::Ps::HOLD); EXPECT_TRUE(r.f->soxrPristine()); EXPECT_TRUE(r.f->decAtH()); });
    add("E15", P::PLAY, [] { Rig r(-1); ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 100); EXPECT_EQ(r.f->passIndex(), 1); EXPECT_TRUE(r.playing()); });   // WRAP done
    add("E15", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); r.runWorker = true; r.period(3); EXPECT_EQ(r.f->ps(), A::Ps::PLAY);
        EXPECT_EQ(r.count(LogEvent::SEEK_PICKUP), 1); });
    add("E15", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 300); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); EXPECT_TRUE(r.f->soxrPristine()); });

    // E16 job failed
    add("E16", P::HOLD, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(100); r.f->failNextJob(A::JobKind::RESET); r.offsetTo(-500); r.period(3);
        EXPECT_EQ(r.count(LogEvent::RESET_FAILED), 1); EXPECT_EQ(r.f->ps(), A::Ps::HOLD); });
    add("E16", P::PLAY, [] { Rig r(-1); r.f->failNextJob(A::JobKind::WRAP); ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 1500);
        EXPECT_EQ(r.f->loopMode(), A::LoopMode::OFF); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); EXPECT_EQ(r.count(LogEvent::WRAP_FAILED), 1); });
    add("E16", P::SEEKING, [] { Rig r; r.f->failNextJob(A::JobKind::SEEK); ASSERT_TRUE(r.toSeeking()); r.runWorker = true; r.period(3);
        EXPECT_EQ(r.count(LogEvent::SEEK_FAILED), 1); EXPECT_EQ(r.f->ps(), A::Ps::SEEKING); r.ms(1200); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });
    add("E16", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 - 500); r.f->failNextJob(A::JobKind::RESET); r.ms(800);
        EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); EXPECT_EQ(r.count(LogEvent::RESET_FAILED), 1); });

    // E17 a job superseded
    add("E17", P::HOLD, [] { Rig r; ASSERT_TRUE(r.toSeeking()); r.latch(OscEvent::MTCFOLLOW, 0); r.period(); r.f->workerRunPending(); r.period(2);
        EXPECT_EQ(r.f->ps(), A::Ps::HOLD); EXPECT_EQ(r.count(LogEvent::SEEK_PICKUP), 0); });
    add("E17", P::PLAY, [] { Rig r(-1); r.blockWrap = true; ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 100); ASSERT_EQ(r.f->runningOrPendingJob(), A::JobKind::WRAP);
        r.blockWrap = false; r.offsetTo(FILE_S * 1000 + 3000); r.period(5); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); EXPECT_EQ(r.count(LogEvent::HANDOVER_LATE), 0); });
    add("E17", P::SEEKING, [] { Rig r; ASSERT_TRUE(r.toSeeking()); r.offsetTo(2500); r.period(); r.runWorker = true; r.period(4); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });
    add("E17", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 100); r.runWorker = false; r.ms(200); r.loop(2); r.offsetTo(500); r.period(); r.runWorker = true; r.period(5);
        EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });

    // E18 late handover (PLAY only)
    add("E18", P::PLAY, [] { Rig r(-1); r.blockWrap = true; ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 1200); EXPECT_GE(r.count(LogEvent::HANDOVER_LATE), 1);
        r.blockWrap = false; r.period(5); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });
    for (P ps : {P::HOLD, P::SEEKING, P::PASTEND}) add("E18", ps, [=] { Rig r(ps == P::PASTEND ? 1 : -1); if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd());
        r.blockWrap = true; r.ms(FILE_S * 1000 + 1200); EXPECT_EQ(r.count(LogEvent::HANDOVER_LATE), 0); });

    // E19 /quit and E20 /load are the main loop's and the OSC thread's (any state)
    for (P ps : STATES) add("E19", ps, [] { AudioPlayer::quitRequested = true; EXPECT_TRUE(AudioPlayer::quitRequested); AudioPlayer::quitRequested = false; });
    for (P ps : STATES) add("E20", ps, [] { SUCCEED() << "/load is refused by the OSC thread while a stream is open (K4)"; });

    // E21 the drain completed (PASTEND only)
    add("E21", P::PASTEND, [] { Rig r; ASSERT_TRUE(r.toPlay()); r.ms(FILE_S * 1000 + 300); EXPECT_EQ(r.f->feed(), A::Feed::ENDED);
        EXPECT_GE(r.count(LogEvent::END_WAIT), 1); EXPECT_TRUE(r.f->soxrPristine()); });
    for (P ps : {P::HOLD, P::PLAY, P::SEEKING}) add("E21", ps, [=] { Rig r; if (ps == P::PLAY) ASSERT_TRUE(r.toPlay()); if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking());
        r.ms(500); EXPECT_EQ(r.count(LogEvent::END_WAIT), 0); });

    // E22 the safety-net end (PLAY, LIVE): target > endEff + 80 ms, before E10's 3 periods
    add("E22", P::PLAY, [=] { Rig r(2); ASSERT_TRUE(r.toPlay()); r.ms(500); jump(r, 2 * FILE_S * 1000 + 500); r.period(2);
        EXPECT_EQ(r.count(LogEvent::SAFETY_END), 1); EXPECT_EQ(r.f->ps(), A::Ps::PASTEND); EXPECT_EQ(r.count(LogEvent::RESYNC), 0); });
    for (P ps : {P::HOLD, P::SEEKING, P::PASTEND}) add("E22", ps, [=] { Rig r(2); if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd());
        jump(r, 2 * FILE_S * 1000 + 500); r.period(3); EXPECT_EQ(r.count(LogEvent::SAFETY_END), 0); });

    // E23 the target crosses 0 upward
    add("E23", P::HOLD, [] { Rig r; r.latch(OscEvent::MTCFOLLOW, 1); r.offsetTo(-100); r.period(); EXPECT_EQ(r.f->ps(), A::Ps::HOLD);
        r.ms(150); EXPECT_EQ(r.f->ps(), A::Ps::PLAY); });
    for (P ps : {P::PLAY, P::SEEKING, P::PASTEND}) add("E23", ps, [=] { Rig r; if (ps == P::PLAY) ASSERT_TRUE(r.toPlay()); if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking());
        if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd()); const A::Ps before = r.f->ps(); r.period(); EXPECT_EQ(r.f->ps(), before); });

    // E24 the first EOF
    add("E24", P::PLAY, [] { Rig r(-1); ASSERT_TRUE(r.toPlay()); EXPECT_EQ(r.f->decodedLength(), -1); r.ms(FILE_S * 1000 + 100);
        EXPECT_EQ(r.f->decodedLength(), (int64_t)FILE_S * RATE); });
    for (P ps : {P::HOLD, P::SEEKING, P::PASTEND}) add("E24", ps, [=] { Rig r; if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd());
        r.ms(300); SUCCEED(); });

    // E25 /play, /stop and E26 /stoponlost: refused / ignored with a WARNING once, any state
    for (P ps : STATES) {
        add("E25", ps, [=] { Rig r; if (ps == P::PLAY) ASSERT_TRUE(r.toPlay()); if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd());
            const A::Ps before = r.f->ps(); r.latch(OscEvent::PLAY_STOP); r.latch(OscEvent::PLAY_STOP); r.period();
            EXPECT_EQ(r.count(LogEvent::PLAY_STOP_REFUSED), 1); EXPECT_EQ(r.f->ps(), before); });
        add("E26", ps, [=] { Rig r; if (ps == P::PLAY) ASSERT_TRUE(r.toPlay()); if (ps == P::SEEKING) ASSERT_TRUE(r.toSeeking()); if (ps == P::PASTEND) ASSERT_TRUE(r.toPastEnd());
            const A::Ps before = r.f->ps(); r.latch(OscEvent::STOPONLOST, 0); r.latch(OscEvent::STOPONLOST, 1); r.period();
            EXPECT_EQ(r.count(LogEvent::STOPONLOST_IGNORED), 1); EXPECT_EQ(r.f->ps(), before); });
    }
    return t;
}

const char* psName(A::Ps p)
{
    switch (p) {
    case A::Ps::HOLD: return "HOLD";
    case A::Ps::PLAY: return "PLAY";
    case A::Ps::SEEKING: return "SEEKING";
    case A::Ps::PASTEND: return "PASTEND";
    }
    return "?";
}

}  // namespace

// Every (event, state) pair of h.3 has an entry: the table cannot drift.
TEST(H3Table, EveryPairIsCovered) {
    std::set<std::pair<std::string, int>> have;
    for (const auto& e : table()) have.insert({e.ev, (int)e.ps});
    for (const char* ev : EVENTS)
        for (A::Ps ps : STATES) EXPECT_TRUE(have.count({ev, (int)ps})) << ev << " x " << psName(ps) << " has no entry";
    EXPECT_EQ(have.size(), sizeof(EVENTS) / sizeof(EVENTS[0]) * 4);
}

// Each pair, in its own fresh rig.
TEST(H3Table, EveryPairBehavesAsTheCellSays) {
    for (const auto& e : table()) {
        SCOPED_TRACE(e.ev + " x " + psName(e.ps));
        e.fn();
        // the feed engine's own invariants hold after every pair (checked inside each rig's lifetime by the engine counter)
    }
}
