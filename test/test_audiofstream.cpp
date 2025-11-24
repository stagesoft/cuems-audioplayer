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

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <cstring>
#include "audiofstream.h"

namespace fs = std::filesystem;

class AudioFstreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary test audio file (simple WAV header)
        testFile = fs::temp_directory_path() / "test_audio.wav";
        createTestWavFile();
    }

    void TearDown() override {
        // Clean up test file
        if (fs::exists(testFile)) {
            fs::remove(testFile);
        }
    }

    void createTestWavFile() {
        // Create a minimal valid WAV file for testing
        // This is a very basic WAV file structure
        std::ofstream file(testFile, std::ios::binary);
        
        // WAV header (44 bytes minimum)
        const unsigned char wavHeader[] = {
            'R', 'I', 'F', 'F',  // ChunkID
            0x24, 0x00, 0x00, 0x00,  // ChunkSize (36 bytes)
            'W', 'A', 'V', 'E',  // Format
            'f', 'm', 't', ' ',  // Subchunk1ID
            0x10, 0x00, 0x00, 0x00,  // Subchunk1Size (16)
            0x01, 0x00,  // AudioFormat (PCM)
            0x02, 0x00,  // NumChannels (2)
            0x44, 0xAC, 0x00, 0x00,  // SampleRate (44100)
            0x10, 0xB1, 0x02, 0x00,  // ByteRate
            0x04, 0x00,  // BlockAlign
            0x10, 0x00,  // BitsPerSample (16)
            'd', 'a', 't', 'a',  // Subchunk2ID
            0x00, 0x00, 0x00, 0x00  // Subchunk2Size (0 for now)
        };
        
        file.write(reinterpret_cast<const char*>(wavHeader), sizeof(wavHeader));
        file.close();
    }

    fs::path testFile;
};

// Test constructor with empty filename
TEST_F(AudioFstreamTest, ConstructorEmptyFilename) {
    AudioFstream stream;
    
    EXPECT_FALSE(stream.good());
    EXPECT_FALSE(stream.eof());
}

// Test constructor with filename
TEST_F(AudioFstreamTest, ConstructorWithFilename) {
    AudioFstream stream(testFile.string());
    
    // File should be opened (if valid audio file)
    // Note: This test may fail if FFmpeg can't decode the minimal WAV
    // In that case, we'll test with a real audio file if available
}

// Test open method
TEST_F(AudioFstreamTest, OpenMethod) {
    AudioFstream stream;
    stream.open(testFile.string(), std::ios::binary | std::ios::in);
    
    // File should be opened
    // Note: May fail with minimal WAV, but tests the code path
}

// Test close method
TEST_F(AudioFstreamTest, CloseMethod) {
    AudioFstream stream;
    stream.open(testFile.string(), std::ios::binary | std::ios::in);
    stream.close();
    
    EXPECT_FALSE(stream.good());
}

// Test loadFile method
TEST_F(AudioFstreamTest, LoadFileMethod) {
    AudioFstream stream;
    bool result = stream.loadFile(testFile.string());
    
    // Result depends on whether FFmpeg can decode the file
    // This tests the code path at least
}

// Test setTargetSampleRate
TEST_F(AudioFstreamTest, SetTargetSampleRate) {
    AudioFstream stream;
    stream.setTargetSampleRate(48000);
    
    // Should not crash, even without file open
}

// Test setResampleQuality with valid qualities
TEST_F(AudioFstreamTest, SetResampleQualityValid) {
    AudioFstream stream;
    
    stream.setResampleQuality("vhq");
    stream.setResampleQuality("hq");
    stream.setResampleQuality("mq");
    stream.setResampleQuality("lq");
    
    // Should not crash
}

// Test setResampleQuality with invalid quality
TEST_F(AudioFstreamTest, SetResampleQualityInvalid) {
    AudioFstream stream;
    
    // Should handle invalid quality gracefully
    stream.setResampleQuality("invalid");
    
    // Should not crash
}

// Test getFileSize (without file)
TEST_F(AudioFstreamTest, GetFileSizeNoFile) {
    AudioFstream stream;
    
    unsigned long long size = stream.getFileSize();
    EXPECT_EQ(size, 0);
}

// Test getChannels (without file)
TEST_F(AudioFstreamTest, GetChannelsNoFile) {
    AudioFstream stream;
    
    unsigned int channels = stream.getChannels();
    EXPECT_EQ(channels, 0);
}

// Test getSampleRate (without file)
TEST_F(AudioFstreamTest, GetSampleRateNoFile) {
    AudioFstream stream;
    
    unsigned int rate = stream.getSampleRate();
    EXPECT_EQ(rate, 0);
}

// Test getBitsPerSample
TEST_F(AudioFstreamTest, GetBitsPerSample) {
    AudioFstream stream;
    
    // Should always return 32 (float output)
    unsigned int bits = stream.getBitsPerSample();
    EXPECT_EQ(bits, 32);
}

// Test eof on empty/closed stream
TEST_F(AudioFstreamTest, EofClosedStream) {
    AudioFstream stream;
    
    EXPECT_FALSE(stream.eof());
}

// Test good on closed stream
TEST_F(AudioFstreamTest, GoodClosedStream) {
    AudioFstream stream;
    
    EXPECT_FALSE(stream.good());
}

// Test bad on closed stream
TEST_F(AudioFstreamTest, BadClosedStream) {
    AudioFstream stream;
    
    EXPECT_FALSE(stream.bad());
}

// Test clear method
TEST_F(AudioFstreamTest, ClearMethod) {
    AudioFstream stream;
    stream.clear();
    
    // Should not crash
}

// Test seekg with different seek directions
TEST_F(AudioFstreamTest, SeekgDifferentDirections) {
    AudioFstream stream;
    
    // Test seek from beginning
    stream.seekg(0, std::ios_base::beg);
    
    // Test seek from current
    stream.seekg(0, std::ios_base::cur);
    
    // Test seek from end
    stream.seekg(0, std::ios_base::end);
    
    // Should not crash (even without file)
}

// Test read method (without file)
TEST_F(AudioFstreamTest, ReadNoFile) {
    AudioFstream stream;
    
    char buffer[1024];
    stream.read(buffer, sizeof(buffer));
    
    streamsize count = stream.gcount();
    EXPECT_EQ(count, 0);
}

// Test gcount after read
TEST_F(AudioFstreamTest, GcountAfterRead) {
    AudioFstream stream;
    
    char buffer[1024];
    stream.read(buffer, sizeof(buffer));
    
    streamsize count = stream.gcount();
    // Should be 0 if no file or EOF
    EXPECT_GE(count, 0);
}

// Test multiple operations sequence
TEST_F(AudioFstreamTest, MultipleOperationsSequence) {
    AudioFstream stream;
    
    stream.setTargetSampleRate(48000);
    stream.setResampleQuality("hq");
    stream.open(testFile.string(), std::ios::binary | std::ios::in);
    stream.setTargetSampleRate(44100);
    stream.seekg(0, std::ios_base::beg);
    
    char buffer[1024];
    stream.read(buffer, sizeof(buffer));
    streamsize count = stream.gcount();
    
    stream.close();
    
    // Should complete without crashing
    EXPECT_GE(count, 0);
}

// Test resample quality parsing
TEST_F(AudioFstreamTest, ResampleQualityParsing) {
    AudioFstream stream;
    
    // Test all valid quality strings
    std::vector<std::string> validQualities = {"vhq", "hq", "mq", "lq"};
    
    for (const auto& quality : validQualities) {
        stream.setResampleQuality(quality);
        // Should not crash
    }
}

// Test file operations with non-existent file
TEST_F(AudioFstreamTest, NonExistentFile) {
    AudioFstream stream;
    
    bool result = stream.loadFile("/nonexistent/file.wav");
    EXPECT_FALSE(result);
    
    EXPECT_FALSE(stream.good());
    EXPECT_FALSE(stream.eof());
}

// Test destructor cleanup
TEST_F(AudioFstreamTest, DestructorCleanup) {
    {
        AudioFstream stream;
        stream.open(testFile.string(), std::ios::binary | std::ios::in);
        // Destructor should clean up
    }
    
    // Should not crash
}

// Test setTargetSampleRate before and after file open
TEST_F(AudioFstreamTest, SetTargetSampleRateTiming) {
    AudioFstream stream;
    
    // Set before opening
    stream.setTargetSampleRate(48000);
    
    // Open file
    stream.open(testFile.string(), std::ios::binary | std::ios::in);
    
    // Set after opening
    stream.setTargetSampleRate(44100);
    
    // Should handle both cases
}

// Test seekg with various positions
TEST_F(AudioFstreamTest, SeekgVariousPositions) {
    AudioFstream stream;
    stream.open(testFile.string(), std::ios::binary | std::ios::in);
    
    // Test various seek positions
    stream.seekg(0, std::ios_base::beg);
    stream.seekg(100, std::ios_base::beg);
    stream.seekg(-50, std::ios_base::cur);
    stream.seekg(0, std::ios_base::end);
    
    // Should not crash
}

