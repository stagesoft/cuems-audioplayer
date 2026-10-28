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

// S10-1 (step 10 of plan 869ejxng3): a stream-copy-trimmed AAC mp4 (first
// packets at negative pts, the first carrying the cut's skip samples) must
// open on its true first packet and wrap on it. Before the fix the open-time
// seek landed on the second packet (pass 1 played the cut's pre-roll) and
// every wrap seek found no audio packet (the player gave up looping).
// Embedded fixture (fixtures/gen_trim.sh): no external files.

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>
#include "audiofstream.h"
#include "fixtures/trim_fixture.h"

namespace {

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

class TrimmedWrap : public ::testing::Test {
protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/trimmed_wrap_XXXXXX";
        ASSERT_NE(mkdtemp(tmpl), nullptr);
        dir_ = tmpl;
        path_ = dir_ + "/fx_trim_aac.mp4";
        FILE* f = std::fopen(path_.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fwrite(fx_trim_aac_mp4, 1, fx_trim_aac_mp4_len, f), fx_trim_aac_mp4_len);
        std::fclose(f);
    }
    void TearDown() override
    {
        std::remove(path_.c_str());
        rmdir(dir_.c_str());
    }
    std::string dir_, path_;
};

}  // namespace

TEST_F(TrimmedWrap, PassOneIsTheCutsContent) {
    AudioFstream s;
    s.open(path_, std::ios::binary | std::ios::in);
    ASSERT_TRUE(s.good());
    const auto pass1 = readToEnd(s);
    // The skip samples applied: not the cut's pre-roll (about 0.5 s longer)
    EXPECT_EQ((long)(pass1.size() / s.getChannels()), fx_trim_aac_mp4_decoded_frames);
    EXPECT_EQ(s.getFirstDeliveredSample(), 0);
}

TEST_F(TrimmedWrap, WrapSeekLandsOnTheFirstPacketEveryPass) {
    AudioFstream s;
    s.open(path_, std::ios::binary | std::ios::in);
    ASSERT_TRUE(s.good());
    const auto pass1 = readToEnd(s);
    for (int pass = 2; pass <= 4; pass++) {
        ASSERT_TRUE(s.seekToFirstPacket()) << "wrap seek before pass " << pass;
        const auto again = readToEnd(s);
        ASSERT_EQ(again.size(), pass1.size()) << "pass " << pass;
        EXPECT_EQ(s.getFirstDeliveredSample(), 0) << "pass " << pass;
        double maxd = 0;
        for (size_t i = 0; i < pass1.size(); i++) {
            maxd = std::max(maxd, std::fabs((double)again[i] - (double)pass1[i]));
        }
        // The native AAC encoder's PNS noise is not reset by a flush (the
        // step-0 gate's aac tolerance)
        EXPECT_LE(maxd, 0.1) << "pass " << pass;
    }
}
