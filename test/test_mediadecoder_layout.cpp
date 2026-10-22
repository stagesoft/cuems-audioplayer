// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Hotfix 0.0.3-13 (cuems-RELATIONS Plans/2026-10-22-audioplayer-chlayout-abort-hotfix.md; finding T7-7 of the
// 869ejxng3 plan). cuems-mediadecoder's createSwrContextExplicit declared `AVChannelLayout inChLayout;`
// uninitialised and passed it to av_channel_layout_copy(), which av_channel_layout_uninit()s its destination
// first: stack garbage that reads as an AV_CHANNEL_ORDER_CUSTOM layout had its bogus u.map free()d. The player
// aborted at arm for every file whose decoder reports a known channel layout (MP3, Vorbis, Opus, FLAC, AAC, ...);
// PCM in WAV reports UNSPEC and takes the default-layout branch. Fixed in mediadecoder 8f5c3c6 (`= {}`).
//
// The test makes the garbage deterministic: every 8-byte stack word below the caller is set to
// 0x0000000200000002, so any 8-aligned AVChannelLayout there reads order = CUSTOM (2) with a bogus map; the
// resampler is then created in a forked child for each fixture. The fixtures are embedded (gen.sh), so the test
// runs wherever the suite runs and never skips. Each fixture is first checked in the parent on a clean stack:
// the decoder must report the expected layout order (so the test cannot go vacuous on a future FFmpeg) and
// the resampler must be created.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include "cuems_mediadecoder/AudioDecoder.h"
#include "fixtures/mediadecoder_fixtures.h"

namespace {

struct Fixture {
    const char* name;
    const unsigned char* data;
    unsigned int len;
    bool knownLayout;   // the decoder reports order != UNSPEC (the crashing branch)
};

const std::vector<Fixture>& fixtures()
{
    static const std::vector<Fixture> f = {
        {"fx_mp3.mp3", fx_mp3_mp3, fx_mp3_mp3_len, true},
        {"fx_vorbis.ogg", fx_vorbis_ogg, fx_vorbis_ogg_len, true},
        {"fx_opus.opus", fx_opus_opus, fx_opus_opus_len, true},
        {"fx_webm_opus.weba", fx_webm_opus_weba, fx_webm_opus_weba_len, true},
        {"fx_flac.flac", fx_flac_flac, fx_flac_flac_len, true},
        {"fx_aac.m4a", fx_aac_m4a, fx_aac_m4a_len, true},
        {"fx_pcm.wav", fx_pcm_wav, fx_pcm_wav_len, false},
    };
    return f;
}

__attribute__((noinline)) void dirtyStack()
{
    volatile uint64_t words[4096];
    for (auto& w : words) {
        w = 0x0000000200000002ULL;   // order = CUSTOM (2), nb_channels = 2; u.map = this value
    }
    asm volatile("" : : "r"(words) : "memory");
}

// Opens `path`, opens its audio decoder and creates the resampler, optionally on a dirtied stack.
// Returns 0 on success; 4-7 name the step that failed. `order` receives the decoder's layout order.
__attribute__((noinline)) int createResampler(const std::string& path, bool dirty, int* order)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) {
        return 4;
    }
    avformat_find_stream_info(fc, nullptr);
    int si = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) {
        avformat_close_input(&fc);
        return 5;
    }
    int rc = 0;
    {
        cuems_mediadecoder::AudioDecoder decoder;
        if (!decoder.openCodec(fc->streams[si]->codecpar) || !decoder.getCodecContext()) {
            rc = 6;
        } else {
            if (order) {
                *order = decoder.getCodecContext()->ch_layout.order;
            }
            if (dirty) {
                dirtyStack();
            }
            SwrContext* swr = decoder.createSwrContextExplicit(2, 48000, AV_SAMPLE_FMT_FLT);
            rc = swr ? 0 : 7;
            swr_free(&swr);
        }
    }
    avformat_close_input(&fc);
    return rc;
}

class TempDir {
public:
    TempDir()
    {
        char tmpl[] = "/tmp/mediadecoder_layout_XXXXXX";
        if (mkdtemp(tmpl)) {
            path_ = tmpl;
        }
    }
    ~TempDir()
    {
        for (const auto& f : files_) {
            std::remove(f.c_str());
        }
        if (!path_.empty()) {
            rmdir(path_.c_str());
        }
    }
    bool ok() const { return !path_.empty(); }
    std::string write(const Fixture& fx)
    {
        std::string p = path_ + "/" + fx.name;
        FILE* f = std::fopen(p.c_str(), "wb");
        if (!f) {
            return std::string();
        }
        size_t n = std::fwrite(fx.data, 1, fx.len, f);
        std::fclose(f);
        files_.push_back(p);
        return n == fx.len ? p : std::string();
    }

private:
    std::string path_;
    std::vector<std::string> files_;
};

}  // namespace

TEST(MediaDecoderLayout, ResamplerSetupSurvivesAGarbageStack)
{
    TempDir dir;
    ASSERT_TRUE(dir.ok()) << "mkdtemp failed";
    const std::string savedStyle = ::testing::FLAGS_gtest_death_test_style;
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    for (const auto& fx : fixtures()) {
        const std::string p = dir.write(fx);
        ASSERT_FALSE(p.empty()) << "could not write " << fx.name;
        // 1. Clean stack, in this process: the fixture decodes, the layout order is the expected one, and
        //    the resampler is created. A failure here is a broken fixture or FFmpeg change, not the bug.
        int order = -1;
        ASSERT_EQ(createResampler(p, false, &order), 0) << fx.name << ": resampler on a clean stack";
        if (fx.knownLayout) {
            ASSERT_NE(order, AV_CHANNEL_ORDER_UNSPEC) << fx.name << ": expected a known channel layout";
        } else {
            ASSERT_EQ(order, AV_CHANNEL_ORDER_UNSPEC) << fx.name << ": expected an unspecified channel layout";
        }
        // 2. Dirty stack, in a forked child: on the unfixed decoder the child is killed by a signal.
        EXPECT_EXIT(std::exit(createResampler(p, true, nullptr)), ::testing::ExitedWithCode(0), "") << fx.name;
    }
    ::testing::FLAGS_gtest_death_test_style = savedStyle;
}
