// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Day alignment of the cue start (cuems-RELATIONS
// Plans/2026-10-25-mtc-day-align-24h-wrap.md, §3.3 row 2, §7.2, §7.4a):
// /start_tod and /offset resolved by PlayerCore into this process's MTC day
// at first use, against a fake head (the 12 h rule itself is mtcreceiver's
// dayShift, tested there). The pending path, the out-of-range normalisation,
// the int64 widening (a head at 30 days) and the three journal lines.

#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <unistd.h>
#include "dayalign.h"
#include "feed_test_util.h"
#include "mtcreceiver.h"
#include "playercore.h"

using A = AudioFstream;

namespace {

const unsigned RATE = 48000, N = 512;
const int64_t P_NS = 1000000000LL * N / RATE;
const int64_t DAY = dayalign::DAY_MS;
const int64_t H = dayalign::HOUR_MS;

// A head this test sets by hand, and whether it is authoritative
struct FakeDay {
    int64_t headMs = 0;
    bool authoritative = true;
};

bool fakeToLocalDay(void* ctx, long engineMs, long& localMs, long& k)
{
    const FakeDay* d = static_cast<const FakeDay*>(ctx);
    if (!d->authoritative) return false;
    k = MtcReceiver::dayShift(engineMs, (long)d->headMs);
    localMs = engineMs - k * MtcReceiver::DAY_MS;
    return true;
}

struct Rig {
    std::string path;
    std::unique_ptr<A> f;
    std::unique_ptr<PlayerCore> c;
    FakeDay day;
    int64_t now = 0, mtcNs = 0;
    TickSample t;
    std::vector<float> out = std::vector<float>(N);
    std::vector<LogEvent> log;

    explicit Rig(int64_t headMs)
    {
        path = feedtest::tmpPath("dayalign");
        std::vector<float> x(3 * RATE);
        for (size_t i = 0; i < x.size(); i++) x[i] = 0.2f * (float)std::sin(2 * M_PI * 440.0 * i / RATE);
        feedtest::writeWavF32(path, RATE, 1, x);
        f.reset(new A());
        f->open(path, std::ios::binary | std::ios::in);
        f->prepareFeed(RATE, N, false);
        c.reset(new PlayerCore(*f, RATE, N));
        c->setEndWaitMs(__LONG_MAX__);
        DayResolver dr;
        dr.fn = &fakeToLocalDay;
        dr.ctx = &day;
        c->setDayResolver(dr);
        setHead(headMs);
        latchLoop(1);
        period(3);
    }
    ~Rig() { unlink(path.c_str()); }
    void setHead(int64_t ms) { day.headMs = ms; mtcNs = ms * 1000000; }
    void latchLoop(int passes)
    {
        OscEvent e;
        e.type = OscEvent::LOOP; e.a = passes; e.nargs = 1;
        c->latch(e);
    }
    void tod(int32_t v) { OscEvent e; e.type = OscEvent::START_TOD; e.a = v; c->latch(e); }
    void offsetF(double f) { OscEvent e; e.type = OscEvent::OFFSET_F; e.f = f; c->latch(e); }
    void follow() { OscEvent e; e.type = OscEvent::MTCFOLLOW; e.a = 1; c->latch(e); }
    // one period with the MTC moving and an anchored tick
    void period(int n = 1)
    {
        for (int i = 0; i < n; i++) {
            now += P_NS;
            mtcNs += P_NS;
            day.headMs = mtcNs / 1000000;
            t.monoNs = now; t.headMs = (long)day.headMs; t.anchored = true; t.complete = true; t.seq++;
            c->callback(out.data(), N, now, (long)day.headMs, t);
            f->workerRunPending();
            LogEvent e;
            while (c->popLog(e)) log.push_back(e);
        }
    }
    int count(LogEvent::Type ty) const { int n = 0; for (auto& e : log) n += e.type == ty; return n; }
    const LogEvent* first(LogEvent::Type ty) const { for (auto& e : log) if (e.type == ty) return &e; return nullptr; }
    // the start this process resolved, in its own day (offset = -start)
    int64_t localStartMs() const { return -c->offsetNs() / 1000000; }
};

}  // namespace

// --- the wire value -------------------------------------------------------

TEST(DayAlign, TodNormalisedIntoOneDay)
{
    bool ch = true;
    EXPECT_EQ(dayalign::normalizeTod(0, &ch), 0); EXPECT_FALSE(ch);
    EXPECT_EQ(dayalign::normalizeTod(86399999, &ch), 86399999); EXPECT_FALSE(ch);
    EXPECT_EQ(dayalign::normalizeTod(86400000, &ch), 0); EXPECT_TRUE(ch);     // one day: wraps to 0, WARNING
    EXPECT_EQ(dayalign::normalizeTod(-1, &ch), 86399999); EXPECT_TRUE(ch);    // negative: mod DAY, WARNING
    EXPECT_EQ(dayalign::normalizeTod(3 * DAY + 5, &ch), 5); EXPECT_TRUE(ch);
    EXPECT_EQ(dayalign::normalizeTod(-DAY - 7, &ch), DAY - 7); EXPECT_TRUE(ch);
    // a lenient float sender: floored, then normalised (the engine rounds then mods)
    EXPECT_EQ(dayalign::todFromFloat(86399999.6, &ch), 86399999); EXPECT_FALSE(ch);
    EXPECT_EQ(dayalign::todFromFloat(86400000.0, &ch), 0); EXPECT_TRUE(ch);
    EXPECT_EQ(dayalign::todFromFloat(-0.4, &ch), 86399999); EXPECT_TRUE(ch);
}

TEST(DayAlign, OffsetWithKZeroIsTheLegacyValue)
{
    // /offset aligned (k = 0): exactly floor(offset) * 1e6, as the player always did
    for (double f : {-1234.7, -0.2, 0.0, 15.9, -3600000.5}) {
        const int64_t raw = (int64_t)std::floor(f);
        EXPECT_EQ(dayalign::offsetNsFor(dayalign::engineStartMs(dayalign::Wire::OFFSET_F, raw)), raw * 1000000) << f;
    }
}

// --- /start_tod resolved against this process's head ------------------------

TEST(DayAlign, TodAgainstHeadsAtDayZeroOneTwo)
{
    for (int d : {0, 1, 2}) {
        Rig r(d * DAY + 10 * H);
        r.tod((int32_t)(10 * H + 5000));                 // 5 s from now, as a time of day
        r.period();
        EXPECT_FALSE(r.c->offsetPending());
        EXPECT_EQ(r.localStartMs(), d * DAY + 10 * H + 5000) << "day " << d;
        // k = minus the day count: a line for d >= 1 (expected, INFO), none for d = 0
        EXPECT_EQ(r.count(LogEvent::DAY_REALIGN), d == 0 ? 0 : 1);
        if (d > 0) {
            const LogEvent* e = r.first(LogEvent::DAY_REALIGN);
            EXPECT_EQ(e->a, -d);
            EXPECT_EQ(e->kind, 1);                       // phase=tod
            bool so; int lvl;
            const std::string line = PlayerCore::describe(*e, "d1", &so, &lvl);
            EXPECT_EQ(lvl, 6);
            EXPECT_NE(line.find("MTC day realign k=" + std::to_string(-d)), std::string::npos) << line;
            EXPECT_NE(line.find("phase=tod"), std::string::npos) << line;
        }
    }
}

TEST(DayAlign, TheWorkedExampleOfPlanSection71)
{
    const int32_t start = 321040;                        // 00:05:21.040
    {   Rig r(DAY + 319000);                             // day 1 + 00:05:19
        r.tod(start); r.period();
        EXPECT_EQ(r.localStartMs(), DAY + start); }      // day 1 + 00:05:21: in 2 s
    {   Rig r(319000);                                   // day 0 + 00:05:19 (born after the crossing)
        r.tod(start); r.period();
        EXPECT_EQ(r.localStartMs(), start); }            // in 2 s
    {   Rig r(DAY - 1000);                               // day 0 + 23:59:59
        r.tod(start); r.period();
        EXPECT_EQ(r.localStartMs(), DAY + start); }      // day 1 + 00:05:21: in 5 min 22 s
}

TEST(DayAlign, TodAcrossTheWrapEdge)
{
    // fresh process 10 ms past midnight, a start 10 ms before it: local -10 ms (k = +1)
    {   Rig r(10);
        // the rig ran 3 periods: re-anchor the head exactly, then resolve
        r.setHead(10); r.tod((int32_t)(DAY - 10)); r.period();
        EXPECT_LT(r.localStartMs(), 0);
        EXPECT_EQ(r.localStartMs(), -10); }
    // a process at day 1 - 10 ms, a start 10 ms after midnight: day 1 + 10 ms
    {   Rig r(DAY - 10);
        r.setHead(DAY - 10); r.tod(10); r.period();
        EXPECT_EQ(r.localStartMs(), DAY + 10); }
}

TEST(DayAlign, TiesAtTwelveHours)
{
    // engine - head exactly +12 h -> k = 1; exactly -12 h -> k = 0 (mtcreceiver's rule)
    {   Rig r(0); r.setHead(H);                          // head 01:00
        r.tod((int32_t)(13 * H));                        // +12 h
        // resolved inside the latched event's callback, against the head before it advances
        r.c->callback(r.out.data(), N, r.now + P_NS, (long)r.day.headMs, r.t);
        EXPECT_EQ(r.localStartMs(), 13 * H - DAY); }
    {   Rig r(0); r.setHead(13 * H);                     // head 13:00
        r.tod((int32_t)H);                               // -12 h
        r.c->callback(r.out.data(), N, r.now + P_NS, (long)r.day.headMs, r.t);
        EXPECT_EQ(r.localStartMs(), H); }
}

TEST(DayAlign, HeadAtThirtyDaysWidensToInt64)
{
    const int64_t head = 30 * DAY + H;                   // 2 592 000 000 + ... > 2^31 ms
    Rig r(head);
    r.tod((int32_t)(H + 2000));
    r.period();
    EXPECT_EQ(r.localStartMs(), 30 * DAY + H + 2000);
    EXPECT_EQ(r.c->offsetNs(), -(30 * DAY + H + 2000) * 1000000);
    EXPECT_GT(-r.c->offsetNs() / 1000000, (int64_t)INT32_MAX);
}

// --- /offset (phase ext): a released engine's extended domain -------------

TEST(DayAlign, OffsetFromAnEngineADayAhead)
{
    // the Dipòsit case: the engine counted a wrap this process never saw
    Rig r(10 * H);
    const double f = -(double)(DAY + 10 * H + 3000);     // engine start = day 1 + 10:00:03
    r.offsetF(f);
    r.period();
    EXPECT_EQ(r.localStartMs(), 10 * H + 3000);          // in 3 s, in this process's day
    ASSERT_EQ(r.count(LogEvent::DAY_REALIGN), 1);
    const LogEvent* e = r.first(LogEvent::DAY_REALIGN);
    EXPECT_EQ(e->a, 1);
    EXPECT_EQ(e->kind, 0);                               // phase=ext
    bool so; int lvl;
    PlayerCore::describe(*e, "d1", &so, &lvl);
    EXPECT_EQ(lvl, 6);                                   // k >= 1 on ext: expected
}

TEST(DayAlign, OffsetWithNegativeKIsAWarning)
{
    Rig r(DAY + 10 * H);
    r.offsetF(-(double)(10 * H + 3000));                 // engine a day behind: nothing explains it
    r.period();
    EXPECT_EQ(r.localStartMs(), DAY + 10 * H + 3000);
    const LogEvent* e = r.first(LogEvent::DAY_REALIGN);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->a, -1);
    bool so; int lvl;
    PlayerCore::describe(*e, "d1", &so, &lvl);
    EXPECT_EQ(lvl, 4);
}

TEST(DayAlign, OneLinePerKValue)
{
    Rig r(DAY + 10 * H);
    for (int i = 0; i < 5; i++) { r.tod((int32_t)(10 * H + 60000 + i * 1000)); r.period(); }
    EXPECT_EQ(r.count(LogEvent::DAY_REALIGN), 1);
    EXPECT_EQ(r.c->dayConversions(), 5);
}

// --- the pending path -----------------------------------------------------

TEST(DayAlign, PendingUntilTheHeadIsAuthoritativeThenPlays)
{
    Rig r(DAY + 10 * H);
    r.day.authoritative = false;
    r.follow();
    const int64_t startMs = r.day.headMs + 200;          // 200 ms from now
    r.tod((int32_t)(startMs % DAY));
    r.period(40);                                        // ~430 ms: past the start, still unresolved
    EXPECT_TRUE(r.c->offsetPending());
    EXPECT_EQ(r.f->ps(), A::Ps::HOLD);                   // never started on the old offset
    EXPECT_EQ(r.count(LogEvent::DAY_REALIGN), 0);
    r.day.authoritative = true;
    r.period(3);
    EXPECT_FALSE(r.c->offsetPending());
    EXPECT_EQ(r.localStartMs(), startMs);
    EXPECT_EQ(r.f->ps(), A::Ps::PLAY);
    EXPECT_GE(r.count(LogEvent::CACHE_START), 1);
}

TEST(DayAlign, LatestPendingValueWins)
{
    Rig r(10 * H);
    r.day.authoritative = false;
    r.tod((int32_t)(10 * H + 5000));
    r.period();
    r.tod((int32_t)(10 * H + 9000));
    r.period();
    r.day.authoritative = true;
    r.period();
    EXPECT_EQ(r.localStartMs(), 10 * H + 9000);
    EXPECT_EQ(r.c->dayConversions(), 1);
}

TEST(DayAlign, UnresolvedWarningAfterTwoSeconds)
{
    Rig r(10 * H);
    r.day.authoritative = false;
    r.tod((int32_t)(10 * H + 5000));
    r.period(150);                                       // 1.6 s
    EXPECT_EQ(r.count(LogEvent::DAY_UNRESOLVED), 0);
    r.period(60);                                        // 2.24 s
    ASSERT_EQ(r.count(LogEvent::DAY_UNRESOLVED), 1);
    r.period(200);
    EXPECT_EQ(r.count(LogEvent::DAY_UNRESOLVED), 1);     // once per pending value
    bool so; int lvl;
    const std::string line = PlayerCore::describe(*r.first(LogEvent::DAY_UNRESOLVED), "d1", &so, &lvl);
    EXPECT_EQ(lvl, 4);
    EXPECT_NE(line.find("/start_tod"), std::string::npos) << line;
}

TEST(DayAlign, ImplausiblyFarStartIsAWarningOnce)
{
    Rig r(10 * H);
    r.follow();
    r.tod((int32_t)(10 * H + 2 * H));                    // 2 h from now: within 12 h, but implausible
    r.period(5);
    EXPECT_EQ(r.f->ps(), A::Ps::HOLD);
    ASSERT_EQ(r.count(LogEvent::DAY_IMPLAUSIBLE), 1);
    r.period(20);
    EXPECT_EQ(r.count(LogEvent::DAY_IMPLAUSIBLE), 1);
    bool so; int lvl;
    PlayerCore::describe(*r.first(LogEvent::DAY_IMPLAUSIBLE), "d1", &so, &lvl);
    EXPECT_EQ(lvl, 4);
}

TEST(DayAlign, NearStartIsNotImplausible)
{
    Rig r(10 * H);
    r.follow();
    r.tod((int32_t)(10 * H + 30000));                    // 30 s from now
    r.period(10);
    EXPECT_EQ(r.count(LogEvent::DAY_IMPLAUSIBLE), 0);
}
