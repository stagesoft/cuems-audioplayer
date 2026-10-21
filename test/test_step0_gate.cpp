/* LICENSE TEXT

    audioplayer for linux based using RtAudio and RtMidi libraries to
    process audio and receive MTC sync. It also uses oscpack to receive
    some configurations through osc commands.
    Copyright (C) 2026  Stage Lab Coop.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>

// Step-0 gate of plan 869ejxng3 rev 3 (section 4.3g, step 0): before any
// design code, the decoder's output at the FILE rate (below soxr) must equal
// the ffprobe sample sum the engine will use as the loop period, the header
// must equal it where section 4.1 trusts the header, and pass 2 after the
// first-packet seek must be sample-identical to pass 1. If a format fails,
// the period source changes before anything else is built.
//
// The fixtures and their manifest are generated on casas
// (cuems-RELATIONS baselines/1amfwm7e6cw-varispeed-probe/step0/gate/
// gen_fixtures.sh); point STEP0_GATE_DIR at them. Without it the tests skip.

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "audiofstream.h"

namespace {

struct Fixture {
    std::string name, codec, cls;   // cls: exact | aac (PNS tolerance) | ms (+-1 ms origins)
    long rate = 0, ffprobeSum = 0, headerSamples = -1;
    bool headerUsed = false;
};

std::string gateDir()
{
    const char* d = std::getenv("STEP0_GATE_DIR");
    return d ? std::string(d) : std::string();
}

std::vector<Fixture> manifest()
{
    std::vector<Fixture> out;
    std::ifstream m(gateDir() + "/manifest.tsv");
    std::string line;
    std::getline(m, line);  // header
    while (std::getline(m, line)) {
        std::istringstream is(line);
        Fixture f;
        int used = 0;
        is >> f.name >> f.codec >> f.rate >> f.ffprobeSum >> f.headerSamples >> used >> f.cls;
        f.headerUsed = used != 0;
        if (!f.name.empty()) out.push_back(f);
    }
    return out;
}

// Reads in JACK-period-sized chunks (512 frames) until the stream is empty.
std::vector<float> readToEnd(AudioFstream& s)
{
    const unsigned ch = s.getChannels();
    std::vector<float> all, buf(512 * ch);
    for (int guard = 0; guard < 10000000; guard++) {
        s.read((char*)buf.data(), buf.size() * sizeof(float));
        const long n = s.gcount() / (long)sizeof(float);
        if (n <= 0) break;
        all.insert(all.end(), buf.begin(), buf.begin() + n);
    }
    return all;
}

class Step0Gate : public ::testing::Test {
protected:
    void SetUp() override {
        if (gateDir().empty()) GTEST_SKIP() << "STEP0_GATE_DIR not set";
        fixtures = manifest();
        ASSERT_FALSE(fixtures.empty()) << "no manifest.tsv in " << gateDir();
    }
    std::string path(const Fixture& f) { return gateDir() + "/" + f.name; }
    std::vector<Fixture> fixtures;
};

}  // namespace

TEST_F(Step0Gate, DecodedLengthEqualsFfprobeSum) {
    for (const auto& f : fixtures) {
        SCOPED_TRACE(f.name);
        AudioFstream s;
        s.open(path(f), std::ios::binary | std::ios::in);
        ASSERT_TRUE(s.good());
        const auto pcm = readToEnd(s);
        const long frames = (long)(pcm.size() / s.getChannels());
        std::cout << "[gate] " << f.name << " " << f.codec << " decoded " << frames
                  << " ffprobe " << f.ffprobeSum << " header " << f.headerSamples << std::endl;
        EXPECT_EQ(frames, f.ffprobeSum);
        EXPECT_EQ(s.getDecodedFrames(), f.ffprobeSum);
        EXPECT_TRUE(s.eof());
    }
}

TEST_F(Step0Gate, HeaderEqualsDecodedWhereSection41UsesIt) {
    for (const auto& f : fixtures) {
        if (!f.headerUsed) continue;
        SCOPED_TRACE(f.name);
        EXPECT_EQ(f.headerSamples, f.ffprobeSum);
    }
}

TEST_F(Step0Gate, Pass2EqualsPass1AfterFirstPacketSeek) {
    for (const auto& f : fixtures) {
        SCOPED_TRACE(f.name);
        AudioFstream s;
        s.open(path(f), std::ios::binary | std::ios::in);
        ASSERT_TRUE(s.good());
        const auto pass1 = readToEnd(s);
        for (int pass = 2; pass <= 3; pass++) {
            ASSERT_TRUE(s.seekToFirstPacket()) << "wrap seek before pass " << pass;
            const auto again = readToEnd(s);
            ASSERT_EQ(again.size(), pass1.size()) << "pass " << pass;
            double maxd = 0;
            long firstDiff = -1;
            for (size_t i = 0; i < pass1.size(); i++) {
                const double d = std::fabs((double)again[i] - (double)pass1[i]);
                if (d > 0 && firstDiff < 0) firstDiff = (long)(i / s.getChannels());
                if (d > maxd) maxd = d;
            }
            std::cout << "[gate] " << f.name << " pass " << pass << " max|d| " << maxd
                      << " first differing frame " << firstDiff << std::endl;
            if (f.cls == "aac") {
                // The native AAC encoder's PNS noise is not reset by a flush
                // (section 4.3g: tolerance gate, documented)
                EXPECT_LE(maxd, 0.1) << "pass " << pass;
            } else {
                EXPECT_EQ(maxd, 0.0) << "pass " << pass << " first differing frame " << firstDiff;
            }
        }
    }
}

TEST_F(Step0Gate, OriginIsContentSampleZero) {
    for (const auto& f : fixtures) {
        SCOPED_TRACE(f.name);
        // exact for sample-timebase containers, +-1 ms for mkv/webm/Opus (K6)
        const long tol = (f.cls == "ms") ? f.rate / 1000 : 0;
        AudioFstream s;
        s.open(path(f), std::ios::binary | std::ios::in);
        ASSERT_TRUE(s.good());
        readToEnd(s);
        EXPECT_EQ(s.getFirstDeliveredSample(), 0) << "pass 1";
        for (int pass = 2; pass <= 3; pass++) {
            ASSERT_TRUE(s.seekToFirstPacket());
            readToEnd(s);
            const int64_t o = s.getFirstDeliveredSample();
            std::cout << "[gate] " << f.name << " pass " << pass << " origin " << o << std::endl;
            ASSERT_NE(o, INT64_MIN);
            EXPECT_LE(std::llabs(o), tol) << "pass " << pass;
        }
    }
}

// The end of a resampled stream delivers soxr's whole tail (on rc_1 a
// 48 -> 44.1 kHz file came out 304 frames short).
TEST_F(Step0Gate, ResampledDrainDeliversTheTail) {
    for (const auto& f : fixtures) {
        if (f.name != "a.wav" && f.name != "a.m4a" && f.name != "a_44k.wav" && f.name != "a_44k.mp3")
            continue;
        SCOPED_TRACE(f.name);
        const unsigned target = (f.rate == 48000) ? 44100 : 48000;
        AudioFstream s;
        s.open(path(f), std::ios::binary | std::ios::in);
        s.setTargetSampleRate(target);
        ASSERT_TRUE(s.good());
        const auto pcm = readToEnd(s);
        const double frames = (double)(pcm.size() / s.getChannels());
        const double expected = (double)f.ffprobeSum * target / f.rate;
        std::cout << "[gate] " << f.name << " " << f.rate << " -> " << target << " out " << frames
                  << " expected " << expected << std::endl;
        EXPECT_LE(std::fabs(frames - expected), 1.0);
    }
}
