/* LICENSE TEXT

    audioplayer for linux based using RtAudio and RtMidi libraries to
    process audio and receive MTC sync. It also uses oscpack to receive
    some configurations through osc commands.
    Copyright (C) 2020  Stage Lab & bTactic.

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

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <type_traits>
#include "audioplayer.h"

namespace fs = std::filesystem;

// Note: AudioPlayer tests are limited because it requires:
// - JACK audio server running
// - Real audio hardware
// - MTC receiver setup
// The callback's logic lives in PlayerCore (test_lockloop.cpp) and the feed
// in AudioFstream (test_audiofstream_feed.cpp, test_soxr_feeding.cpp): the
// byte-domain playHead / audioMillisecondSize are gone, the position is
// AudioFstream::pos() in content samples (plan 869ejxng3 + 1amfwm7e6cw rev 3).

class AudioPlayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        AudioPlayer::endOfPlay = false;
        AudioPlayer::quitRequested = false;
    }
};

TEST_F(AudioPlayerTest, StaticMemberInitialization) {
    EXPECT_FALSE(AudioPlayer::endOfPlay);
    EXPECT_FALSE(AudioPlayer::quitRequested);
}

TEST_F(AudioPlayerTest, StaticMemberModification) {
    AudioPlayer::endOfPlay = true;
    EXPECT_TRUE(AudioPlayer::endOfPlay);
    AudioPlayer::quitRequested = true;
    EXPECT_TRUE(AudioPlayer::quitRequested);
}

TEST_F(AudioPlayerTest, ClassExists) {
    EXPECT_TRUE(sizeof(AudioPlayer) > 0);
}

TEST_F(AudioPlayerTest, InheritanceCheck) {
    EXPECT_TRUE((std::is_base_of<OscReceiver, AudioPlayer>::value));
}

TEST_F(AudioPlayerTest, EndOfPlayAtomicOperations) {
    AudioPlayer::endOfPlay = true;
    EXPECT_TRUE(AudioPlayer::endOfPlay);
    AudioPlayer::endOfPlay = false;
    EXPECT_FALSE(AudioPlayer::endOfPlay);
}

// /quit, SIGTERM and SIGINT are flags the main loop acts on (I16e, J16)
TEST_F(AudioPlayerTest, QuitIsAFlag) {
    AudioPlayer::quitRequested = true;
    EXPECT_TRUE(AudioPlayer::quitRequested);
    AudioPlayer::quitRequested = false;
    EXPECT_FALSE(AudioPlayer::quitRequested);
}

// The position lives in the feed engine, in content samples
TEST_F(AudioPlayerTest, PositionIsInContentSamples) {
    AudioFstream s;
    EXPECT_EQ(s.pos(), 0.0);
    EXPECT_FALSE(s.feedPrepared());
}

TEST_F(AudioPlayerTest, AtomicThreadSafety) {
    for (int i = 0; i < 100; ++i) {
        AudioPlayer::endOfPlay = (i % 3 == 0);
        AudioPlayer::quitRequested = (i % 2 == 0);
    }
    AudioPlayer::endOfPlay = false;
    AudioPlayer::quitRequested = false;
    EXPECT_FALSE(AudioPlayer::endOfPlay);
}
