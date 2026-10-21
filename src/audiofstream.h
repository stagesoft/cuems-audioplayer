/* LICENSE TEXT

    audioplayer for linux based using RtAudio and RtMidi libraries to
    process audio and receive MTC sync. It also uses oscpack to receive
    some configurations through osc commands.
    Copyright (C) 2020  Stage Lab Coop.

    Authors:
        Alex Ramos <alex@stagelab.coop>
        Ion Reguera <ion@stagelab.coop>

    Portions of the file reading and audio decoding code are based on mpv
    (https://mpv.io/), which is licensed under GPLv2+ and LGPLv2.1+.

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

//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
// Stage Lab Cuems audio file stream class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
#ifndef AUDIOFSTREAM_H
#define AUDIOFSTREAM_H

#include <iostream>
#include <vector>
#include <iomanip>
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>
#include <semaphore.h>
#include <soxr.h>

#include "cuems_mediadecoder/MediaFileReader.h"
#include "cuems_mediadecoder/AudioDecoder.h"
#include "cuems_mediadecoder/FFmpegUtils.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include "cuemslogger.h"
#include "cuems_errors.h"

using namespace std;

class AudioFstream
{
    public:
        AudioFstream(   const string filename = "", 
                        ios_base::openmode openmode = ios_base::in | ios_base::binary );
        ~AudioFstream();

        // Stream-like interface for compatibility
        void read(char* buffer, size_t bytes);  // Outputs 32-bit float samples
        void seekg(long long pos, ios_base::seekdir dir);
        streamsize gcount() const;
        bool eof() const;
        bool good() const;
        bool bad() const;
        void clear();
        void close();
        void open(const string path, ios_base::openmode mode);
        bool loadFile( const string path );

        // Methods for resampling
        void setTargetSampleRate(unsigned int rate);
        void setResampleQuality(const string& quality);
        void setTargetChannels(unsigned int channels);  // Set target channel count for downmixing
        
        // File information accessors (for compatibility with audioplayer.cpp)
        unsigned long long getFileSize() const;
        unsigned int getChannels() const;
        unsigned int getSampleRate() const;
        unsigned int getBitsPerSample() const;

        // Frames delivered by the decoder (after swr, at the FILE rate, below
        // soxr) since open() or the last seek. At the true end of the stream
        // this is the decoded length L (plan 869ejxng3 rev 3, step-0 gate).
        int64_t getDecodedFrames() const;

        // The loop's wrap seek: back to the audio stream's first packet, with
        // no soxr_clear (soxr sees one continuous input). False when the seek
        // fails or lands anywhere but the first packet (plan rev 3 section
        // 4.3h, h.2 WRAP steps 1-4; the caller takes the failed-wrap path).
        bool seekToFirstPacket();
        // File-sample index (content sample 0 = the first sample delivered
        // after open(), at the file rate) of the first sample delivered since
        // open() or the last seek; INT64_MIN if unknown.
        int64_t getFirstDeliveredSample() const;

        ////////////////////////////////////////////////////////////
        // Feed engine (plan 869ejxng3 + 1amfwm7e6cw rev 3, sections
        // 4.3b, 4.3c and the normative 4.3h v2.1). Positions are input
        // samples at the FILE rate; "absolute" = pass index x P_s + the
        // pass-relative content sample. prepareFeed() completes E1 (the
        // head cache, the VR soxr, the worker) before startStream; from
        // then on the legacy read()/seekg() are refused.
        //
        // Threads: everything except the worker's own jobs runs on ONE
        // thread, the audio thread (the step-2 callback), which latches
        // the OSC events and calls, per period and in h.3's order:
        //   serviceWorker(ctx)   E15-E17, the pickup, the SEEK retry
        //   setLoop / start / stopOutput / offsetChanged / reenable / endNow
        //   process(out, n, r, ctx)   E12-E14, E18, E21, the output
        // The MTC state (mtc, followingMtc), target, the lock loop, E10,
        // E11, E22's decision and the start condition live in step 2.
        ////////////////////////////////////////////////////////////
        enum class Ps { HOLD, PLAY, SEEKING, PASTEND };
        enum class Feed { CACHE, DECODER, PAD, DRAIN, ENDED };
        enum class JobKind { NONE, SEEK, WRAP, RESET };
        enum class StartKind { REVEAL, OFFSET, RETURN, CORRECTION, FIFO, REENABLE };
        enum class LoopMode { PENDING, ONCE, WRAP, OFF };
        // What the audio thread knows this period (step 2 fills it)
        struct LiveCtx {
            int64_t target = 0;     // absolute input samples (live, or extrapolated in SUSPENDED)
            bool mtcLive = false;   // mtc in {LIVE, SUSPENDED}
        };
        // Audio-thread events for step 2's ring (h.6); drained with takeEvents()
        enum class EvType { CACHE_START, SEEK_PICKUP, HANDOVER_LATE, PASS_CROSSED, END,
                            DRAINED, SEEK_FAILED, WRAP_FAILED, RESET_FAILED, LP_MISMATCH,
                            TINY_LOOP, HEADER_SHORT, FIRST_EOF, MODE_CHANGED, PAST_END_START,
                            INVARIANT };
        struct FeedEvent { EvType type; int64_t a; int64_t b; int kind; };
        static const int MAX_EVENTS = 64;

        // E1 completion: head cache, VR soxr (create-time ratio = the maximum,
        // set to nominal at once), buffers, the pos constant c, the worker.
        // threaded=false: no worker thread, the test drives the worker with
        // workerRunPending() (the fake worker clock).
        bool prepareFeed(unsigned outRate, unsigned periodFrames, bool threaded = true);
        bool feedPrepared() const { return feedReady; }

        // E5 (applied on the audio thread): passes <= 0 = infinite, 1 = once;
        // num = 0: the period is the decoded length. Tiny-loop guard, header
        // check, the mode (off latched per file).
        void setLoop(int passes, int64_t num, int64_t den);
        LoopMode loopMode() const { return mode; }
        int loopPassesValue() const { return loopPasses; }
        int takeWrapCount();

        // h.4, the one start rule. t absolute.
        void start(int64_t t, StartKind kind);
        // E3 / E7 / E8: out <- SILENCE, ps <- HOLD; a SEEKING job is abandoned.
        void stopOutput();
        // E4 in PLAY or SEEKING (HOLD: step 2's condition does it; PASTEND: reenable()).
        void offsetChanged(int64_t t);
        // E2 / E4 / E5 in PASTEND: the re-enable test (N11). True -> ps = HOLD.
        bool reenable(int64_t target);
        // E22 (decided by step 2): as E13.
        void endNow();
        // E2 / E4 / E20 clear the latch (T3).
        void clearSeekFailed() { seekFailed = false; }
        // E15-E17 and the SEEK retry; the pickup when ctx.mtcLive.
        void serviceWorker(const LiveCtx& ctx);
        // One output period: exactly `frames` interleaved float frames at the
        // output rate. ratio = the lock loop's r (input/output). Returns frames.
        size_t process(float* out, size_t frames, double ratio, const LiveCtx& ctx);
        int takeEvents(FeedEvent* out, int max);

        // State (audio thread)
        Ps ps() const { return psState; }
        Feed feed() const { return feedSrc; }
        bool outPlaying() const { return outPlay; }
        bool soxrPristine() const { return soxrPristineFlag; }
        bool decAtH() const { return decAtHFlag; }
        bool seekFailedLatched() const { return seekFailed; }
        int64_t passIndex() const { return passIdx; }
        int64_t origin() const { return originAbs; }
        double pos() const;                 // origin + sum(r*N) + c
        double fifo() const { return sumIdone - sumRN; }   // FIFO health, 241 steady
        int64_t feedPassIndex() const { return feedPass; }   // the feed's pass (test-build feed trace, T7-9)
        double inputConsumed() const { return sumIdone; }    // input fed to soxr since the last start
        double posConstant() const { return posC; }
        int64_t cursor() const;             // pass-relative feed position
        int64_t cacheLength() const { return cacheH; }
        int64_t periodSamples() const;      // P_s at the pass boundary (L_dec for num = 0 once known)
        int64_t periodForStart() const;     // P_s for t mod P_s (the header length before the first EOF)
        int64_t decodedLength() const { return lDec; }   // -1 until the first EOF
        int64_t endSamples() const;         // end (h.1)
        int64_t wrapsLeft() const;
        unsigned outputRate() const { return outRateFeed; }
        int jobsOutstanding() const { return outstanding; }
        JobKind runningOrPendingJob() const;
        uint64_t generation() const { return gen.load(); }
        int invariantViolations() const { return invViolations; }
        int soxrClearsOutsideWorker() const { return clearsOutsideWorker.load(); }
        // Seek attempts beyond the first (the retry ladder of section 4.3a)
        int seekRetryCount() const { return seekRetries.load(); }
        // The last accepted SEEK landing's distance before its target (samples)
        int64_t lastSeekLeadSamples() const { return lastSeekLead.load(); }
        const std::vector<float>& headCache() const { return cache; }
        bool seekStreamIsVideo() const { return seekStreamIndex != audioStreamIndex; }
        bool demuxDiscardsNonAudio() const { return discardSet; }

        // Test hooks (the fake worker clock)
        void workerRunPending();                      // threaded=false only
        void setWorkerStepHook(std::function<void(JobKind, int)> h) { stepHook = h; }
        void failNextJob(JobKind k) { failNext.store((int)k); }
        // Publish the live target for the worker's chase (process/serviceWorker do it too)
        void publishTarget(int64_t t) { liveTarget.store(t, std::memory_order_release); }
        // Frames soxr produced in the last process() (the rest was silence)
        size_t lastPeriodProduced() const { return lastProduced; }
        // Test: every input block fed to soxr (below the resampler), with the
        // feed's pass and the pass-relative cursor of the block's first sample
        // AFTER consumption is reported as `cursorAfter`
        void setInputTap(void (*tap)(void*, const float*, size_t, int64_t, int64_t), void* user)
        { inputTap = tap; inputTapUser = user; }

    private:
        // cuems-mediadecoder members
        cuems_mediadecoder::MediaFileReader fileReader;
        cuems_mediadecoder::AudioDecoder audioDecoder;
        AVPacket* packet;
        AVFrame* frame;
        SwrContext* swrContext;
        int audioStreamIndex;
        
        // File state
        bool fileOpen;
        bool eofReached;        // decoder AND swr fully drained (below soxr)
        bool demuxEof;          // av_read_frame hit EOF, flush packet sent: draining
        bool decoderDrained;    // the decoder returned AVERROR_EOF after the flush
        bool resamplerDrained;  // soxr's tail delivered after eofReached
        int64_t decodedFrames;

        // First-packet seek state
        int64_t firstAudioTs;        // pts (else dts) of the stream's first audio packet
        bool firstAudioTsKnown;
        int64_t audioStartTime;      // stream start_time: the fallback position reference
        int64_t contentStartTs;      // ts of content sample 0, taken at the first delivery after open
        bool calibratePending;
        bool hasDecoded;             // the decoder has output a frame since open()
        int64_t pendingDiscard;      // frames to drop at the head of the decoded output
        int64_t lastFrameTs;         // timestamp of the last decoded frame (its pts)
        int64_t firstDeliveredSample;
        bool firstDeliveredKnown;
        bool errorState;
        streamsize lastBytesRead;
        
        // Audio properties
        unsigned int fileChannels;
        unsigned int fileSampleRate;
        unsigned int fileBitsPerSample;
        int64_t totalSamples;
        unsigned long long fileSize;  // For boundary checks (in bytes, for 32-bit float output)
        int64_t currentSamplePos;
        
        // Format conversion buffer (FFmpeg decoded → float for libsoxr)
        float* conversionBuffer;
        size_t conversionBufferSize;  // in floats
        size_t conversionBufferUsed;  // floats currently in buffer
        size_t conversionBufferPos;   // read position in buffer

        // Resampling members (libsoxr - unchanged)
        unsigned int targetSampleRate;
        unsigned int targetChannels;  // Target channel count for downmixing (0 = use file's channels)
        soxr_t resampler;
        bool resamplingEnabled;
        soxr_quality_spec_t qualitySpec;
        float* resampleInputBuffer;
        float* resampleOutputBuffer;
        size_t resampleBufferSize;

        // Helper methods
        void initializeResampler();
        void cleanupResampler();
        void cleanupFFmpeg();
        soxr_quality_spec_t parseQualityString(const string& quality);
        bool decodeNextFrame();  // Decode one frame from FFmpeg
        bool refillConversionBuffer();  // false once decoder and swr are drained
        bool firstPacketSeek(bool atOpen);
        int64_t tsToFileSample(int64_t ts) const;
        string getFFmpegError(int errnum);  // Translate FFmpeg error codes

        // Content-sample bookkeeping of conversionBuffer (pass-relative file samples)
        int64_t convStartSample;     // sample index of conversionBuffer[0]
        bool convKnown;
        bool fromFirstPacket;        // the next first delivery follows a first-packet seek
        int64_t nextSampleIndex() const;
        bool discardTo(int64_t s, bool checkGen, uint64_t g);   // false on EOF/error/abandon

        // Matroska index choice by density (M4) and the demux discard (I5)
        int seekStreamIndex;
        double gopSeconds;
        bool discardSet;
        bool isMatroska;
        void chooseSeekIndex();

        // ---- feed engine state (audio thread) ----
        bool feedReady;
        bool threadedWorker;
        unsigned outRateFeed, periodFramesFeed;
        double rNom;
        soxr_t vrs;
        double posC;
        std::vector<float> cache;    // [0, H) x channels, immutable after prepareFeed
        int64_t cacheH;
        int64_t lDec;                // decoded length, -1 until the first EOF
        int64_t headerLen;
        int loopPasses;
        int64_t loopNum, loopDen;
        int64_t pS;                  // P_s for num > 0
        LoopMode mode;
        bool offLatched;
        bool lpChecked;
        Ps psState;
        Feed feedSrc;
        bool outPlay;
        int64_t cur;                 // cursor for CACHE / PAD (DECODER: nextSampleIndex())
        int64_t feedPass;            // pass of the feed (for the alignment invariant)
        bool soxrPristineFlag, decAtHFlag;
        bool decoderAtHTruth;        // the decoder's real position (RESET step 2 reads it)
        bool seekFailed;
        int seekTries;
        int64_t retryFramesLeft;     // SEEK retry after 1 s of output
        int64_t passIdx;
        int wrapCountPending;
        int64_t originAbs;
        double sumRN, sumIdone, owed, consumed;
        int64_t fadeLeft, fadeLen;
        double lastRatio;
        int passesAtEnd;
        StartKind waitingKind;
        bool waitingStart;
        bool pickupPending;
        bool chasing;                // the bounded discard after a pickup
        int64_t chaseLanding;
        int64_t lateAtFrames;        // output frames counter when E18 fired (-1 none)
        int64_t framesOut;           // output frames produced since prepareFeed
        bool resetAfterStart;        // the eventual start comes after a RESET (fades)
        std::vector<float> zeros;
        FeedEvent events[MAX_EVENTS];
        int nEvents;
        int invViolations;
        void ev(EvType t, int64_t a = 0, int64_t b = 0, int kind = 0);
        void violate(int64_t code);

        // ---- worker (h.2) ----
        struct Job {
            JobKind kind = JobKind::NONE;
            int64_t tPass = 0;
            int64_t endAbs = INT64_MAX;
            int64_t pS = 0;
            uint64_t gen = 0;
        };
        enum class JobStatus { DONE, FAILED, ABANDONED };
        struct JobResult {
            JobKind kind = JobKind::NONE;
            JobStatus status = JobStatus::DONE;
            uint64_t gen = 0;
            int64_t landing = 0;     // SEEK: first sample available (pass-relative)
            bool hitEof = false;
            bool pastEnd = false;
            int64_t lDec = -1;
            double ms = 0;
        };
        Job jobs[3];
        std::atomic<int> pendingIdx;
        std::atomic<int> holdingIdx;
        JobResult result;
        std::atomic<bool> resultReady;
        int outstanding;             // audio thread: posted, result not consumed
        int outstandingKind[4];
        std::atomic<uint64_t> gen;
        std::atomic<int64_t> liveTarget;
        std::atomic<bool> workerQuit;
        std::atomic<int> failNext;
        std::atomic<int> clearsOutsideWorker;
        std::atomic<int> seekRetries;
        std::atomic<int64_t> lastSeekLead;
        bool inWorker;               // written by the worker thread only
        std::function<void(JobKind, int)> stepHook;
        std::thread workerThread;
        sem_t workerSem;
        bool semInit;

        void post(JobKind k, int64_t tPass = 0);
        void workerLoop();
        void runPendingJobs();
        void runJob(const Job& j);
        bool stepOk(const Job& j, int step);
        void publishResult(const JobResult& r);
        bool midFileSeek(const Job& j, JobResult& r);
        void clearSoxr();
        void stopWorker();
        void resetFeedState();

        // ---- audio-thread helpers ----
        void beginOutput(int64_t t, int64_t tPass, Feed src, StartKind kind, bool clean);
        void enterPastEnd(bool drain);
        void onE12orE13();
        void pickup(const LiveCtx& ctx, const JobResult& r);
        bool feedInput(const float*& ptr, size_t& avail, const LiveCtx& ctx);
        void consumeInput(size_t n);
        bool cacheOnly() const;
        int64_t boundaryLimit() const;
        void calibratePosConstant();
        void checkInvariants();
        bool looping() const;
        bool wrapFailed;
        int64_t fadeOutLeft, fadeOutLen;
        double drainEndContent;     // E13: absolute content index one past the last fed sample (T7-1)
        size_t lastProduced;
        JobResult pendingPickup;
        void (*inputTap)(void*, const float*, size_t, int64_t, int64_t);
        void* inputTapUser;
};

#endif // AUDIOFSTREAM_H
