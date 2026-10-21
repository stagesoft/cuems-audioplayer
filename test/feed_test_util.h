// SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
//
// Helpers for the feed-engine tests (plan 869ejxng3 + 1amfwm7e6cw rev 3,
// step 1): the step-0 gate fixtures (STEP0_GATE_DIR), a linear reference
// decode, a rig that drives AudioFstream's feed engine period by period
// with a fake worker clock (threaded = false: the test decides when the
// worker runs), an input tap below the resampler, and synthetic WAVs.
#ifndef FEED_TEST_UTIL_H
#define FEED_TEST_UTIL_H

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include "audiofstream.h"

namespace feedtest {

using A = AudioFstream;

struct Fixture {
    std::string name, codec, cls;   // cls: exact | aac (PNS tolerance) | ms (+-1 ms origins)
    long rate = 0, ffprobeSum = 0, headerSamples = -1;
    bool headerUsed = false;
};

inline std::string gateDir()
{
    const char* d = std::getenv("STEP0_GATE_DIR");
    return d ? std::string(d) : std::string();
}

inline std::vector<Fixture> manifest()
{
    std::vector<Fixture> out;
    std::ifstream m(gateDir() + "/manifest.tsv");
    std::string line;
    std::getline(m, line);
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

inline std::string fixturePath(const std::string& name) { return gateDir() + "/" + name; }

// The legacy linear decode (read() before prepareFeed): the reference content.
inline std::vector<float> linearDecode(const std::string& path, unsigned* channels = nullptr)
{
    A s;
    s.open(path, std::ios::binary | std::ios::in);
    const unsigned ch = s.getChannels();
    if (channels) *channels = ch;
    std::vector<float> all, buf(512 * (ch ? ch : 1));
    for (int guard = 0; guard < 10000000 && ch; guard++) {
        s.read((char*)buf.data(), buf.size() * sizeof(float));
        const long n = s.gcount() / (long)sizeof(float);
        if (n <= 0) break;
        all.insert(all.end(), buf.begin(), buf.begin() + n);
    }
    return all;
}

// A float32 mono/stereo WAV.
inline void writeWavF32(const std::string& path, unsigned rate, unsigned ch, const std::vector<float>& x)
{
    std::ofstream f(path, std::ios::binary);
    const uint32_t bytes = (uint32_t)(x.size() * 4);
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + bytes); f.write("WAVEfmt ", 8); w32(16); w16(3); w16((uint16_t)ch);
    w32(rate); w32(rate * 4 * ch); w16((uint16_t)(4 * ch)); w16(32); f.write("data", 4); w32(bytes);
    f.write((const char*)x.data(), bytes);
}

inline std::string tmpPath(const std::string& name)
{
    const char* t = std::getenv("TMPDIR");
    return std::string(t ? t : "/tmp") + "/" + name + "-" + std::to_string(getpid()) + ".wav";
}

struct TapLog {
    unsigned ch = 1;
    bool rec = true;
    std::vector<float> v;
    static void cb(void* u, const float* p, size_t n, int64_t, int64_t)
    {
        TapLog* t = (TapLog*)u;
        if (t->rec) t->v.insert(t->v.end(), p, p + n * t->ch);
    }
};

// Drives the feed engine like the step-2 callback would, with a fake MTC
// (the target advances by r x N per period) and a fake worker clock.
struct Rig {
    A s;
    TapLog tap;
    A::LiveCtx ctx;
    unsigned ch = 0, outRate = 48000, N = 512;
    double r = 1.0;
    std::vector<float> out;
    bool runWorker = true;       // run the posted job right after each period
    bool blockWrap = false;      // ...except a WRAP (late-handover tests)
    long periods = 0, shortPeriods = 0;
    std::vector<A::FeedEvent> events;

    bool open(const std::string& path, bool threaded = false, unsigned rate = 48000)
    {
        outRate = rate;
        s.open(path, std::ios::binary | std::ios::in);
        if (!s.good() || !s.prepareFeed(outRate, N, threaded)) return false;
        ch = s.getChannels();
        r = (double)s.getSampleRate() / outRate;
        tap.ch = ch;
        s.setInputTap(&TapLog::cb, &tap);
        out.assign((size_t)N * ch, 0.0f);
        ctx.mtcLive = true;
        ctx.target = 0;
        return true;
    }
    void drain()
    {
        A::FeedEvent e[A::MAX_EVENTS];
        int n;
        while ((n = s.takeEvents(e, A::MAX_EVENTS)) > 0) events.insert(events.end(), e, e + n);
    }
    void period(double ratio = 0.0)
    {
        const double rr = ratio > 0 ? ratio : r;
        s.serviceWorker(ctx);
        s.process(out.data(), N, rr, ctx);
        if (s.ps() == A::Ps::PLAY && s.lastPeriodProduced() != N) shortPeriods++;
        if (runWorker && !(blockWrap && s.runningOrPendingJob() == A::JobKind::WRAP)) s.workerRunPending();
        ctx.target += (int64_t)std::llround(N * rr);
        periods++;
        drain();
    }
    // run until the stream has ended (drained, ENDED) or `max` periods
    void runToEnd(long max = 2000000)
    {
        for (long i = 0; i < max && !(s.ps() == A::Ps::PASTEND && s.feed() == A::Feed::ENDED); i++) period();
        for (int i = 0; i < 4; i++) { s.serviceWorker(ctx); s.workerRunPending(); }
        s.serviceWorker(ctx);
        drain();
    }
    // periods until ps == PLAY (after a start that went through a job)
    bool untilPlay(int max = 200)
    {
        for (int i = 0; i < max; i++) {
            if (s.ps() == A::Ps::PLAY) return true;
            period();
        }
        return s.ps() == A::Ps::PLAY;
    }
    int count(A::EvType t) const
    {
        int n = 0;
        for (const auto& e : events) n += (e.type == t);
        return n;
    }
    const A::FeedEvent* last(A::EvType t) const
    {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->type == t) return &*it;
        return nullptr;
    }
};

// max |tap - ref| with the tap starting at pass-relative content index `first`,
// the reference repeating with period P (zeros past L inside a pass: the pad)
inline double maxDiffPeriodic(const std::vector<float>& tap, const std::vector<float>& ref, unsigned ch,
                              int64_t first, int64_t P, long* firstDiff = nullptr)
{
    const long L = (long)(ref.size() / ch);
    const long n = (long)(tap.size() / ch);
    double maxd = 0;
    if (firstDiff) *firstDiff = -1;
    for (long i = 0; i < n; i++) {
        const long fr = (long)((first + i) % P);
        for (unsigned c = 0; c < ch; c++) {
            const double rv = fr < L ? ref[(size_t)fr * ch + c] : 0.0;
            const double d = std::fabs(tap[(size_t)i * ch + c] - rv);
            if (d > 0 && firstDiff && *firstDiff < 0) *firstDiff = i;
            if (d > maxd) maxd = d;
        }
    }
    return maxd;
}

// Best alignment of the first `len` tap frames against ref around `origin`:
// returns the offset (samples) of the content relative to origin.
inline long contentOffset(const std::vector<float>& tap, const std::vector<float>& ref, unsigned ch,
                          int64_t origin, long len = 2048, long range = 200)
{
    const long L = (long)(ref.size() / ch);
    long best = 0;
    double bestErr = 1e300;
    for (long d = -range; d <= range; d++) {
        double e = 0;
        for (long i = 0; i < len && origin + d + i < L && origin + d + i >= 0 && (size_t)(i * ch) < tap.size(); i++)
            for (unsigned c = 0; c < ch; c++) {
                const double x = tap[(size_t)i * ch + c] - ref[(size_t)(origin + d + i) * ch + c];
                e += x * x;
            }
        if (e < bestErr) { bestErr = e; best = d; }
    }
    return best;
}

inline double tolFor(const Fixture& f) { return f.cls == "aac" ? 0.1 : 0.0; }

}  // namespace feedtest

#endif  // FEED_TEST_UTIL_H
