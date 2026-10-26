/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * Day alignment of engine /offset values (src/dayalign.h). AudioPlayer itself
 * cannot be built in a test (JACK, device, file), so the conversion lives in
 * pure helpers and is pinned here: k = 0 byte-identical to the legacy float
 * expression, k != 0 exact, the OSC -> callback mailbox, the log line.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <cmath>
#include <thread>
#include "dayalign.h"

namespace {

constexpr long long DAY = MtcReceiver::DAY_MS;

// The pre-fix /offset handler, statement for statement: the reference the
// k = 0 path must reproduce byte for byte.
long int legacyHeadNewOffset(float offsetOSC, long int latencyMs, unsigned int audioMillisecondSize) {
    std::atomic<long int> outputLatencyMs_{latencyMs};
    std::atomic<long int> headNewOffset{0};
    headNewOffset.store(( offsetOSC + outputLatencyMs_.load() ) * audioMillisecondSize);  // To bytes
    return headNewOffset.load();
}

// What the OSC thread sees: the engine's ms as an OSC float32, floor()ed.
float oscOffset(double engineOffsetMs) {
    return std::floor(static_cast<float>(engineOffsetMs));
}

// seekPosition at the first correction, as the callback computes it.
long long seekAt(long long headMs, long long offsetBytes, unsigned int ams) {
    return headMs * static_cast<long long>(ams) + offsetBytes;
}

} // namespace

TEST(DayAlign, KZeroIsByteIdenticalToLegacy) {
    const double offsets[] = {0, 1234, -40, -5000, -321040, -1468000, -86721040,
                              -(DAY + 1468000.0), -(2.0 * DAY) - 17, -(30.0 * DAY) - 5 * 3600000.0 - 40};
    const long int latencies[] = {0, 10, 21, 42};
    const unsigned int amsList[] = {192, 352, 384, 768};
    for (double off : offsets) {
        for (long int lat : latencies) {
            for (unsigned int ams : amsList) {
                const float f = oscOffset(off);
                const DayAlign::EngineOffset o = DayAlign::fromOsc(f, lat, ams);
                EXPECT_EQ(DayAlign::localOffsetBytes(o, 0, ams), legacyHeadNewOffset(f, lat, ams))
                    << "offset " << off << " latency " << lat << " ams " << ams;
            }
        }
    }
}

// lleida-diposit P2 (granadella 10-25): the player was spawned at wire 00:24:26
// after the engine passed 24 h; the engine sent its day-1 start.
TEST(DayAlign, DipositFreshPlayerAfterTheCrossing) {
    const unsigned int ams = 384;                      // 48 kHz, 2 ch float32
    const long int lat = 10;
    const long long head = 1466000;                    // 00:24:26.000, this process
    const float f = oscOffset(-(DAY + 1468000.0));     // start = day 1 + 00:24:28
    const DayAlign::EngineOffset o = DayAlign::fromOsc(f, lat, ams);
    ASSERT_EQ(o.rawMs, -(DAY + 1468000));              // exact in float32 (multiple of 8)

    // Before the fix: the legacy bytes against our head = a day before the start.
    const long long legacySeek = seekAt(head, o.legacyBytes, ams);
    EXPECT_TRUE(DayAlign::implausibleSeek(legacySeek, 600000ULL * ams, ams))
        << "the silent relevo: seek ~ -24 h";

    const long k = MtcReceiver::dayShift(DayAlign::engineStartMs(o), head);
    ASSERT_EQ(k, 1);
    const long long bytes = DayAlign::localOffsetBytes(o, k, ams);
    EXPECT_EQ(bytes, (-1468000LL + lat) * ams);
    EXPECT_EQ(seekAt(head, bytes, ams), (-2000LL + lat) * ams) << "2 s before the start, held";
    EXPECT_FALSE(DayAlign::implausibleSeek(seekAt(head, bytes, ams), 600000ULL * ams, ams));
}

TEST(DayAlign, OtherDayShifts) {
    const unsigned int ams = 352;                      // 44.1 kHz stereo (truncated ms size)
    const long int lat = 21;
    // k = 2: process born two days after the engine's reset
    {
        const long long head = 7200000;                // 02:00:00
        const float f = oscOffset(-(2.0 * DAY + 7205000));
        const DayAlign::EngineOffset o = DayAlign::fromOsc(f, lat, ams);
        const long k = MtcReceiver::dayShift(DayAlign::engineStartMs(o), head);
        ASSERT_EQ(k, 2);
        EXPECT_EQ(DayAlign::localOffsetBytes(o, k, ams), (o.rawMs + 2 * DAY + lat) * ams);
        EXPECT_EQ(seekAt(head, DayAlign::localOffsetBytes(o, k, ams), ams),
                  (head + o.rawMs + 2 * DAY + lat) * ams);
    }
    // k = -1: process alive across midnight, engine reset since
    {
        const long long head = DAY + 600000;
        const float f = oscOffset(-605000);
        const DayAlign::EngineOffset o = DayAlign::fromOsc(f, lat, ams);
        const long k = MtcReceiver::dayShift(DayAlign::engineStartMs(o), head);
        ASSERT_EQ(k, -1);
        EXPECT_EQ(seekAt(head, DayAlign::localOffsetBytes(o, k, ams), ams), (-5000LL + lat) * ams);
    }
}

// 30 days of engine MTC: float32 carries the value to its 256 ms quantum; the
// k != 0 bytes are computed in int64 with no overflow.
TEST(DayAlign, ThirtyDayEngineFreshPlayer) {
    const unsigned int ams = 768;
    const long int lat = 42;
    const long long head = 5 * 3600000LL;              // fresh player at 05:00:00
    const double startMs = 30.0 * DAY + 5 * 3600000.0 + 3000;
    const float f = oscOffset(-startMs);
    const DayAlign::EngineOffset o = DayAlign::fromOsc(f, lat, ams);
    const long k = MtcReceiver::dayShift(DayAlign::engineStartMs(o), head);
    ASSERT_EQ(k, 30);
    const long long seek = seekAt(head, DayAlign::localOffsetBytes(o, k, ams), ams);
    EXPECT_EQ(seek, (head + o.rawMs + 30 * DAY + lat) * ams);
    EXPECT_LE(std::llabs(seek / ams - (-3000 + lat)), 256) << "within the float32 quantum";

    // Same engine, a process alive the whole time (head in day 30): k = 0, legacy.
    const long long head30 = 30 * DAY + 5 * 3600000LL;
    EXPECT_EQ(MtcReceiver::dayShift(DayAlign::engineStartMs(o), head30), 0);
}

TEST(DayAlign, MailboxLatestWins) {
    DayAlign::OffsetMailbox box;
    DayAlign::EngineOffset o;
    unsigned long seq = 0, applied = 0;
    EXPECT_FALSE(box.readNew(applied, o, seq)) << "nothing published";

    box.publish({-1000, -1000 * 384, 0});
    ASSERT_TRUE(box.readNew(applied, o, seq));
    EXPECT_EQ(o.rawMs, -1000);
    applied = seq;
    EXPECT_FALSE(box.readNew(applied, o, seq)) << "already applied";

    box.publish({-2000, 1, 2});
    box.publish({-3000, 3, 4});
    ASSERT_TRUE(box.readNew(applied, o, seq));
    EXPECT_EQ(o.rawMs, -3000);
    EXPECT_EQ(o.legacyBytes, 3);
    EXPECT_EQ(o.latencyMs, 4);
    EXPECT_GT(seq, applied);
}

// One writer, one reader: every value read is a complete one.
TEST(DayAlign, MailboxNeverTorn) {
    DayAlign::OffsetMailbox box;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (long long x = 1; x <= 200000; ++x) box.publish({-x, 2 * x, 3 * x});
        done.store(true);
    });
    unsigned long applied = 0, seq = 0, reads = 0, torn = 0, backwards = 0;
    long long last = 0;
    DayAlign::EngineOffset o;
    while (!done.load() || box.readNew(applied, o, seq)) {
        if (box.readNew(applied, o, seq)) {
            if (o.legacyBytes != -2 * o.rawMs || o.latencyMs != -3 * o.rawMs) ++torn;
            if (-o.rawMs <= last) ++backwards;
            last = -o.rawMs;
            applied = seq;
            ++reads;
        }
    }
    writer.join();
    EXPECT_EQ(torn, 0UL);
    EXPECT_EQ(backwards, 0UL) << "values only move forward";
    EXPECT_GT(reads, 0UL);
    EXPECT_EQ(last, 200000) << "the reader ends on the latest value";
}

TEST(DayAlign, ImplausibleThresholds) {
    const unsigned int ams = 384;
    const long long hour = 3600000LL * ams;
    const unsigned long long file = 906000ULL * ams;   // 15:06
    EXPECT_FALSE(DayAlign::implausibleSeek(0, file, ams));
    EXPECT_FALSE(DayAlign::implausibleSeek(-5000LL * ams, file, ams));
    EXPECT_FALSE(DayAlign::implausibleSeek(-hour, file, ams));
    EXPECT_TRUE(DayAlign::implausibleSeek(-hour - 1, file, ams));
    EXPECT_FALSE(DayAlign::implausibleSeek((long long)file + hour, file, ams));
    EXPECT_TRUE(DayAlign::implausibleSeek((long long)file + hour + 1, file, ams));
}

TEST(DayAlign, RealignLine) {
    EXPECT_EQ(DayAlign::realignLine(1, 87868000, 1466000, 1),
              "MTC day realign k=1 (engine 87868000 ms, local head 1466000 ms, phase=ext); 1 conversions");
    EXPECT_FALSE(DayAlign::realignIsWarning(1));
    EXPECT_FALSE(DayAlign::realignIsWarning(2));
    EXPECT_TRUE(DayAlign::realignIsWarning(-1));
}
