// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Step 7 (869ejxng3 + 1amfwm7e6cw), finding T7-7: cuems-mediadecoder's createSwrContextExplicit
// declared `AVChannelLayout inChLayout;` uninitialised and passed it to av_channel_layout_copy(), which
// av_channel_layout_uninit()s the destination first. Stack garbage that reads as a CUSTOM-order layout
// is then free()d: the player aborted (SIGABRT in av_channel_layout_uninit <- av_channel_layout_copy)
// at arm for MP3, Vorbis and Opus files on test2 node01 (WAV and AAC take the UNSPEC branch).
// The test makes the garbage deterministic: every 8-byte stack word below the caller is set to
// 0x0000000200000002, so ANY 8-aligned AVChannelLayout there reads order = AV_CHANNEL_ORDER_CUSTOM (2)
// with a bogus u.map; then the resampler is created for each lossy fixture in a forked child.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include "cuems_mediadecoder/AudioDecoder.h"

namespace {

std::string gateDir()
{
    const char* d = std::getenv("STEP0_GATE_DIR");
    return d ? std::string(d) : std::string();
}

__attribute__((noinline)) void dirtyStack()
{
    volatile uint64_t words[4096];
    for (auto& w : words) {
        w = 0x0000000200000002ULL;   // order = CUSTOM (2), nb_channels = 2; u.map = this value
    }
    asm volatile("" : : "r"(words) : "memory");
}

__attribute__((noinline)) int createOnDirtyStack(const std::string& path)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) {
        return 4;
    }
    avformat_find_stream_info(fc, nullptr);
    int si = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) {
        return 5;
    }
    cuems_mediadecoder::AudioDecoder decoder;
    if (!decoder.openCodec(fc->streams[si]->codecpar)) {
        return 6;
    }
    dirtyStack();
    SwrContext* swr = decoder.createSwrContextExplicit(2, 48000, AV_SAMPLE_FMT_FLT);
    int rc = swr ? 0 : 7;
    swr_free(&swr);
    avformat_close_input(&fc);
    return rc;
}

}  // namespace

TEST(MediaDecoderLayout, ResamplerSetupSurvivesAGarbageStack)
{
    if (gateDir().empty()) {
        GTEST_SKIP() << "STEP0_GATE_DIR not set";
    }
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    for (const char* f : {"a.mp3", "a.ogg", "a.opus", "a.weba", "a_44k.mp3"}) {
        const std::string p = gateDir() + "/" + f;
        EXPECT_EXIT(std::exit(createOnDirtyStack(p)), ::testing::ExitedWithCode(0), "") << p;
    }
}
