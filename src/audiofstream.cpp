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
// Stage Lab Cuems audio file stream class source file
// Now using FFmpeg for comprehensive audio format support
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "audiofstream.h"
#include <cstring>
#include <sstream>
#include <cmath>
#include <chrono>
#include <algorithm>

extern "C" {
#include <libavutil/intreadwrite.h>
}

////////////////////////////////////////////
// Constructor
////////////////////////////////////////////
AudioFstream::AudioFstream( const string filename, 
                            ios_base::openmode openmode ) 
{
    // Initialize FFmpeg members
    packet = nullptr;
    frame = nullptr;
    swrContext = nullptr;
    audioStreamIndex = -1;
    
    // Initialize file state
    fileOpen = false;
    eofReached = false;
    demuxEof = false;
    decoderDrained = false;
    resamplerDrained = false;
    decodedFrames = 0;
    firstAudioTs = AV_NOPTS_VALUE;
    firstAudioTsKnown = false;
    audioStartTime = AV_NOPTS_VALUE;
    contentStartTs = AV_NOPTS_VALUE;
    calibratePending = false;
    hasDecoded = false;
    pendingDiscard = 0;
    lastFrameTs = AV_NOPTS_VALUE;
    firstDeliveredSample = INT64_MIN;
    firstDeliveredKnown = false;
    errorState = false;
    lastBytesRead = 0;
    
    // Initialize audio properties
    fileChannels = 0;
    fileSampleRate = 0;
    fileBitsPerSample = 32;  // Output is 32-bit float
    totalSamples = 0;
    fileSize = 0;
    currentSamplePos = 0;
    
    // Initialize conversion buffer
    conversionBuffer = nullptr;
    conversionBufferSize = 0;
    conversionBufferUsed = 0;
    conversionBufferPos = 0;
    
    // Initialize resampling members
    targetSampleRate = 0;
    targetChannels = 0;  // 0 means use file's channel count (no downmixing)
    resampler = nullptr;
    resamplingEnabled = false;
    qualitySpec = soxr_quality_spec(SOXR_HQ, 0);  // Default to high quality
    resampleInputBuffer = nullptr;
    resampleOutputBuffer = nullptr;
    resampleBufferSize = 0;

    convStartSample = 0;
    convKnown = false;
    fromFirstPacket = false;
    seekStreamIndex = -1;
    gopSeconds = 2.0;
    discardSet = false;
    isMatroska = false;

    // Feed engine
    feedReady = false;
    threadedWorker = false;
    outRateFeed = 0;
    periodFramesFeed = 0;
    rNom = 1.0;
    vrs = nullptr;
    posC = 0.0;
    drainEndContent = 0.0;
    pendingIdx.store(-1);
    holdingIdx.store(-1);
    resultReady.store(false);
    gen.store(0);
    liveTarget.store(0);
    workerQuit.store(false);
    failNext.store(0);
    clearsOutsideWorker.store(0);
    seekRetries.store(0);
    lastSeekLead.store(INT64_MIN);
    inWorker = false;
    semInit = false;
    resetFeedState();

    if ( !filename.empty() ) {
        open(filename, openmode);
    }
}

////////////////////////////////////////////
// Destructor
////////////////////////////////////////////
AudioFstream::~AudioFstream (void)
{
    close();
}

////////////////////////////////////////////
// FFmpeg error translation
////////////////////////////////////////////
string AudioFstream::getFFmpegError(int errnum)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return string(errbuf);
}

////////////////////////////////////////////
// Open audio file using FFmpeg
////////////////////////////////////////////
void AudioFstream::open(const string path, ios_base::openmode mode)
{
    close();  // Close any existing file
    
    // Open using MediaFileReader
    if (!fileReader.open(path)) {
        CuemsLogger::getLogger()->logError("Couldn't open file: " + path);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Find audio stream
    audioStreamIndex = fileReader.findStream(AVMEDIA_TYPE_AUDIO);
    if (audioStreamIndex < 0) {
        CuemsLogger::getLogger()->logError("No audio stream found in file");
        fileReader.close();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Get codec parameters
    AVCodecParameters* codecParams = fileReader.getCodecParameters(audioStreamIndex);
    if (!codecParams) {
        CuemsLogger::getLogger()->logError("Failed to get codec parameters");
        fileReader.close();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Open audio decoder
    if (!audioDecoder.openCodec(codecParams)) {
        CuemsLogger::getLogger()->logError("Failed to open audio codec");
        fileReader.close();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Allocate packet and frame
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        CuemsLogger::getLogger()->logError("Failed to allocate packet/frame");
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Extract audio properties from decoder
    AVSampleFormat sampleFmt;
    int channels, sampleRate;
    if (!audioDecoder.getAudioProperties(channels, sampleRate, sampleFmt)) {
        CuemsLogger::getLogger()->logError("Failed to get audio properties");
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // One consolidated stream-info line (was four bare stderr lines per spawn)
    const char* codec_name = avcodec_get_name(codecParams->codec_id);
    {
        std::ostringstream oss;
        oss << "Audio stream " << audioStreamIndex
            << ": codec=" << (codec_name ? codec_name : "unknown")
            << " channels=" << channels << " rate=" << sampleRate << "Hz"
            << " fmt=" << av_get_sample_fmt_name(sampleFmt);
        CuemsLogger::getLogger()->logInfo(oss.str());
    }
    fileChannels = (unsigned int)channels;
    fileSampleRate = (unsigned int)sampleRate;
    
    // Get codec context for additional info
    AVCodecContext* codecContext = audioDecoder.getCodecContext();
    
    // Calculate total samples from duration
    AVFormatContext* formatContext = fileReader.getFormatContext();
    AVStream* audioStream = formatContext->streams[audioStreamIndex];
    audioStartTime = audioStream->start_time;
    // Without the packet time base libavcodec cannot shift frame->pts by the
    // samples it skips (MP3 delay, Opus pre-skip), and positions come out
    // 1105 / 312 samples early
    if (codecContext) {
        codecContext->pkt_timebase = audioStream->time_base;
    }
    if (audioStream->duration != AV_NOPTS_VALUE) {
        totalSamples = av_rescale_q(audioStream->duration, audioStream->time_base, 
                                    (AVRational){1, (int)fileSampleRate});
    } else if (formatContext->duration != AV_NOPTS_VALUE) {
        totalSamples = (int64_t)((double)formatContext->duration / AV_TIME_BASE * fileSampleRate);
    } else {
        // Unknown duration, estimate from file size (will be inaccurate for compressed formats)
        totalSamples = 0;  // Will be updated as we decode
    }
    
    // Setup libswresample for format conversion to float (and optional channel up/downmixing)
    // Use AudioDecoder's createSwrContext which handles unknown channel layouts properly
    unsigned int outputChannels = (targetChannels > 0) ? targetChannels : fileChannels;

    // File size in bytes of what this stream DELIVERS: 32-bit float frames of
    // the OUTPUT channel count. Computing it with the file's own count made a
    // mono file reopened as stereo report half its length, so the first MTC
    // re-sync past the middle was taken as "out of file" (1amfwm7e4wv).
    fileSize = totalSamples * outputChannels * 4;  // 4 bytes per sample (32-bit float)

    // fileChannels still holds the file's own count here (reassigned below).
    if (targetChannels > 0 && targetChannels != fileChannels) {
        const char* kind = (targetChannels > fileChannels) ? "Upmixing" : "Downmixing";
        CuemsLogger::getLogger()->logInfo(std::string(kind) + " audio: " + std::to_string(fileChannels) +
                                          " -> " + std::to_string(targetChannels) + " channels");
    }
    
    swrContext = audioDecoder.createSwrContextExplicit(outputChannels, fileSampleRate, AV_SAMPLE_FMT_FLT);
    
    if (!swrContext) {
        CuemsLogger::getLogger()->logError("Failed to create swresample context");
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Allocate conversion buffer (decode → float)
    // Use outputChannels (may be downmixed) instead of fileChannels
    // Increase buffer size for formats like DTS that may have larger frames
    conversionBufferSize = 16384 * outputChannels;  // 16384 frames worth (larger for DTS/complex codecs)
    conversionBuffer = new float[conversionBufferSize];
    conversionBufferUsed = 0;
    conversionBufferPos = 0;
    
    // Update fileChannels to reflect output channel count (for reading logic)
    fileChannels = outputChannels;
    
    fileOpen = true;
    errorState = false;
    eofReached = false;
    demuxEof = false;
    decoderDrained = false;
    resamplerDrained = false;
    decodedFrames = 0;
    currentSamplePos = 0;
    hasDecoded = false;
    firstAudioTsKnown = false;

    // The open-time seek to the first audio packet: records the first pts
    // every wrap seek must land on, and makes matroska parse its cues now
    // rather than inside the first real seek (plan rev 3, section 4.3h E1).
    // Only the audio stream is demuxed (I5): the audio callback no longer
    // reads every interleaved video packet, and a seek re-reads no GOP
    for (unsigned i = 0; i < formatContext->nb_streams; i++) {
        if ((int)i != audioStreamIndex) {
            formatContext->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    discardSet = true;
    headerLen = totalSamples;

    if (!firstPacketSeek(true)) {
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    // After the open-time seek: matroska's cues are parsed now (M4)
    chooseSeekIndex();
    
    CuemsLogger::getLogger()->logOK("File open OK! : " + path);
    CuemsLogger::getLogger()->logOK("Sample rate: " + std::to_string(fileSampleRate) + " Hz");
    CuemsLogger::getLogger()->logOK("Channels: " + std::to_string(fileChannels));
    CuemsLogger::getLogger()->logOK("Format: " + string(av_get_sample_fmt_name(sampleFmt)));
    if (totalSamples > 0) {
        CuemsLogger::getLogger()->logOK("Duration: " + std::to_string(totalSamples / (double)fileSampleRate) + " seconds");
    }
    
    // Initialize resampler if target sample rate is already set
    if (targetSampleRate > 0 && targetSampleRate != fileSampleRate) {
        initializeResampler();
    }
}

////////////////////////////////////////////
// Set target channel count for downmixing (like mpv)
////////////////////////////////////////////
void AudioFstream::setTargetChannels(unsigned int channels)
{
    targetChannels = channels;
    // Note: Downmixing is applied when file is opened, not dynamically
    // This matches mpv's behavior where channel layout is set at demuxer initialization
}

////////////////////////////////////////////
// Load file (wrapper for open)
////////////////////////////////////////////
bool AudioFstream::loadFile( const string path ) {
    open( path, ios_base::binary | ios_base::in );
    return fileOpen && !errorState;
}

////////////////////////////////////////////
// Decode next frame from FFmpeg
////////////////////////////////////////////
// Receive first, feed only when the decoder asks for input. After the
// demuxer's EOF the decoder is flushed and drained frame by frame until it
// returns AVERROR_EOF: before, EOF was declared with the first drained frame
// and the rest of the decoder's tail was lost at every end of file
// (869ejxng3, plan rev 2 section 4.2a step 1).
bool AudioFstream::decodeNextFrame()
{
    if (!fileOpen || decoderDrained || errorState) {
        return false;
    }
    
    while (true) {
        int ret = audioDecoder.receiveFrame(frame);
        
        if (ret >= 0) {
            // Successfully decoded a frame
            hasDecoded = true;
            // frame->pts carries the decoder's own skip (MP3 LAME delay, the Opus
            // pre-skip); best_effort_timestamp does not
            lastFrameTs = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : frame->best_effort_timestamp;
            return true;
        } else if (ret == AVERROR_EOF) {
            decoderDrained = true;
            return false;
        } else if (ret != AVERROR(EAGAIN)) {
            CuemsLogger::getLogger()->logError(std::string("Error receiving frame from decoder: ") + getFFmpegError(ret));
            errorState = true;
            return false;
        }
        
        // EAGAIN: the decoder needs input
        if (demuxEof) {
            decoderDrained = true;  // flushed and still asking for input: nothing left
            return false;
        }
        
        ret = fileReader.readPacket(packet);
        if (ret == AVERROR_EOF) {
            demuxEof = true;
            audioDecoder.sendPacket(nullptr);  // enter draining mode
            continue;
        } else if (ret < 0) {
            CuemsLogger::getLogger()->logError(std::string("Error reading frame: ") + getFFmpegError(ret));
            errorState = true;
            return false;
        }
        
        // Skip non-audio packets
        if (packet->stream_index != audioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        
        ret = audioDecoder.sendPacket(packet);
        av_packet_unref(packet);
        if (ret < 0) {
            CuemsLogger::getLogger()->logError(std::string("Error sending packet to decoder: ") + getFFmpegError(ret));
            errorState = true;
            return false;
        }
    }
}

////////////////////////////////////////////
// Refill the float conversion buffer
////////////////////////////////////////////
// Decodes and converts until conversionBuffer holds data. At the end, drains
// swr's tail; only when the decoder and swr are both empty is eofReached set
// and false returned. Shared by both read paths.
bool AudioFstream::refillConversionBuffer()
{
    while (conversionBufferPos >= conversionBufferUsed) {
        if (eofReached || errorState) {
            return false;
        }
        
        uint8_t* out = (uint8_t*)conversionBuffer;
        int max_out_samples = conversionBufferSize / fileChannels;
        int out_samples = 0;
        
        if (decodeNextFrame()) {
            // Account for libswresample's internal buffering delay
            int64_t delay = swr_get_delay(swrContext, fileSampleRate);
            int64_t estimated_out_samples = av_rescale_rnd(delay + frame->nb_samples,
                                                            fileSampleRate, fileSampleRate,
                                                            AV_ROUND_UP);
            max_out_samples = std::min((int64_t)max_out_samples, estimated_out_samples);
            out_samples = swr_convert(swrContext, &out, max_out_samples,
                                      (const uint8_t**)frame->data, frame->nb_samples);
            av_frame_unref(frame);
        } else {
            if (errorState) {
                return false;
            }
            // Decoder drained: drain swr, then the stream is at its true end
            if (swr_get_delay(swrContext, fileSampleRate) > 0) {
                out_samples = swr_convert(swrContext, &out, max_out_samples, nullptr, 0);
            }
            if (out_samples == 0) {
                eofReached = true;
                return false;
            }
        }
        
        if (out_samples < 0) {
            CuemsLogger::getLogger()->logError(std::string("Error converting audio samples: ") + getFFmpegError(out_samples));
            errorState = true;
            return false;
        }
        
        const int64_t prevStart = convStartSample;
        const int64_t prevFrames = (int64_t)(conversionBufferUsed / fileChannels);
        conversionBufferUsed = out_samples * fileChannels;
        conversionBufferPos = 0;

        // Drop a pending head discard (Opus pre-skip after a wrap seek)
        int64_t discarded = 0;
        if (pendingDiscard > 0) {
            discarded = std::min(pendingDiscard, (int64_t)out_samples);
            pendingDiscard -= discarded;
            conversionBufferPos = discarded * fileChannels;
        }
        if (!firstDeliveredKnown && out_samples > discarded) {
            firstDeliveredKnown = true;
            if (calibratePending) {
                // Content sample 0: the first sample out of the open-time seek
                calibratePending = false;
                AVStream* st = fileReader.getFormatContext()->streams[audioStreamIndex];
                contentStartTs = (lastFrameTs == AV_NOPTS_VALUE) ? AV_NOPTS_VALUE :
                    lastFrameTs + av_rescale_q(discarded, (AVRational){1, (int)fileSampleRate}, st->time_base);
            }
            // After a first-packet seek the first delivered sample IS content
            // sample 0 (the same first packet, the same decode, the landing pts
            // asserted): never re-derived from a pts, which a 1 ms timebase
            // (webm) puts up to 24 samples off at every wrap
            const int64_t at = fromFirstPacket ? -discarded : tsToFileSample(lastFrameTs);
            fromFirstPacket = false;
            firstDeliveredSample = (at == INT64_MIN) ? INT64_MIN : at + discarded;
            // conversionBuffer[0] is content sample `at` (the discard sits at its head)
            convStartSample = at;
            convKnown = (at != INT64_MIN);
        } else if (firstDeliveredKnown && convKnown && out_samples > 0) {
            convStartSample = prevStart + prevFrames;
        }
        decodedFrames += out_samples - discarded;
    }
    return true;
}

////////////////////////////////////////////
// File-sample index of a stream timestamp
////////////////////////////////////////////
// Relative to content sample 0. A raw pts is off by the container's start (an
// MP3 with a LAME tag starts 1105 samples in), and start_time is not always
// that start: matroska's includes the Opus pre-skip (-7 ms on a webm), so the
// reference is the pts of the first sample delivered after the open-time
// seek, with start_time only until that first delivery.
int64_t AudioFstream::tsToFileSample(int64_t ts) const
{
    if (ts == AV_NOPTS_VALUE || audioStreamIndex < 0 || fileSampleRate == 0) {
        return INT64_MIN;
    }
    AVStream* st = fileReader.getFormatContext()->streams[audioStreamIndex];
    const int64_t start = (contentStartTs != AV_NOPTS_VALUE) ? contentStartTs :
                          (audioStartTime == AV_NOPTS_VALUE) ? 0 : audioStartTime;
    return av_rescale_q(ts - start, st->time_base, (AVRational){1, (int)fileSampleRate});
}

////////////////////////////////////////////
// Seek to the audio stream's first packet
////////////////////////////////////////////
// Seeks on the AUDIO stream (a video index drops the first audio blocks of
// an A/V matroska), flushes, then reads up to the first audio packet itself:
// its pts must be the stream's first, recorded by the open-time call (else
// the wrap failed, M9), and an Opus decoder that has already decoded gets its
// pre-skip discarded when the packet carries no skip side data (libavcodec
// applies the pre-skip only once per decoder, M10). No soxr_clear.
bool AudioFstream::firstPacketSeek(bool atOpen)
{
    AVFormatContext* fc = fileReader.getFormatContext();
    int ret = avformat_seek_file(fc, audioStreamIndex, INT64_MIN, INT64_MIN, INT64_MIN, 0);
    if (ret < 0 && !atOpen) {
        CuemsLogger::getLogger()->logError("Wrap seek failed: " + getFFmpegError(ret));
        return false;
    }
    // At open a refused seek is harmless: nothing has been read yet
    audioDecoder.flush();
    
    conversionBufferUsed = 0;
    conversionBufferPos = 0;
    eofReached = false;
    demuxEof = false;
    decoderDrained = false;
    resamplerDrained = false;
    decodedFrames = 0;
    pendingDiscard = 0;
    firstDeliveredKnown = false;
    firstDeliveredSample = INT64_MIN;
    convKnown = false;
    fromFirstPacket = true;
    
    while (true) {
        ret = fileReader.readPacket(packet);
        if (ret < 0) {
            CuemsLogger::getLogger()->logError("No audio packet after the first-packet seek: " +
                                               getFFmpegError(ret));
            return false;
        }
        if (packet->stream_index == audioStreamIndex) {
            break;
        }
        av_packet_unref(packet);
    }
    
    const int64_t ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
    if (atOpen) {
        firstAudioTs = ts;
        firstAudioTsKnown = true;
        contentStartTs = AV_NOPTS_VALUE;
        calibratePending = true;
    } else if (!firstAudioTsKnown || ts != firstAudioTs) {
        CuemsLogger::getLogger()->logError("Wrap seek landed at ts " + std::to_string(ts) +
                                           ", the first audio packet is at " + std::to_string(firstAudioTs));
        av_packet_unref(packet);
        return false;
    }
    
    size_t sdSize = 0;
    const uint8_t* sd = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &sdSize);
    const bool skipSideData = sd && sdSize >= 4 && AV_RL32(sd) > 0;
    AVCodecParameters* par = fc->streams[audioStreamIndex]->codecpar;
    if (!atOpen && hasDecoded && par->codec_id == AV_CODEC_ID_OPUS && !skipSideData) {
        pendingDiscard = par->initial_padding;
    }
    
    ret = audioDecoder.sendPacket(packet);
    av_packet_unref(packet);
    if (ret < 0) {
        CuemsLogger::getLogger()->logError("Decoder refused the first packet: " + getFFmpegError(ret));
        return false;
    }
    return true;
}

bool AudioFstream::seekToFirstPacket()
{
    if (!fileOpen || errorState) {
        return false;
    }
    return firstPacketSeek(false);
}

int64_t AudioFstream::getFirstDeliveredSample() const
{
    return firstDeliveredKnown ? firstDeliveredSample : INT64_MIN;
}

////////////////////////////////////////////
// Read data from audio file
////////////////////////////////////////////
void AudioFstream::read(char* buffer, size_t bytes)
{
    lastBytesRead = 0;
    
    if (!fileOpen || errorState || feedReady) {
        return;  // with the feed engine prepared, process() is the only reader
    }
    
    size_t bytesRemaining = bytes;
    float* outputPtr = (float*)buffer;
    
    // Calculate how many samples we need (32-bit float output)
    size_t samplesNeeded = bytes / 4;  // 4 bytes per 32-bit float sample
    
    // If resampling is enabled, we need to work with floats through the resampling pipeline
    if (resamplingEnabled && resampler) {
        // samplesNeeded is the total number of samples across all channels (for interleaved audio)
        // Calculate how many complete frames we need
        size_t framesNeeded = samplesNeeded / fileChannels;
        size_t resampledFloatsNeeded = framesNeeded * fileChannels;
        
        float* tempFloatBuffer = new float[resampledFloatsNeeded];
        size_t floatsResampled = 0;
        
        // Runs until soxr's own tail is out too, across as many reads as it
        // takes: stopping on eofReached dropped the rest of the tail.
        while (floatsResampled < resampledFloatsNeeded && !resamplerDrained) {
            // First, ensure we have decoded float data in conversionBuffer
            refillConversionBuffer();
            if (errorState) {
                break;
            }
            
            if (conversionBufferPos >= conversionBufferUsed) {
                // Decoder and swr are drained: drain libsoxr's internal buffer
                size_t outputFramesGenerated = 0;
                soxr_error_t soxr_err = soxr_process(resampler,
                                                     nullptr, 0, nullptr,  // NULL input to drain
                                                     tempFloatBuffer + floatsResampled, 
                                                     framesNeeded - (floatsResampled / fileChannels), 
                                                     &outputFramesGenerated);
                
                if (soxr_err) {
                    CuemsLogger::getLogger()->logError(std::string("SOXR drain error: ") + std::string(soxr_strerror(soxr_err)));
                    break;
                }
                
                floatsResampled += outputFramesGenerated * fileChannels;
                
                if (outputFramesGenerated == 0) {
                    resamplerDrained = true;  // Fully drained
                }
            } else {
                // Feed available float data to libsoxr
                size_t floatsAvailable = conversionBufferUsed - conversionBufferPos;
                size_t floatsToResample = std::min(floatsAvailable, resampledFloatsNeeded - floatsResampled);
                size_t framesInThisPass = floatsToResample / fileChannels;
                
                size_t inputFramesUsed = 0;
                size_t outputFramesGenerated = 0;
                
                soxr_error_t soxr_err = soxr_process(resampler,
                                                     conversionBuffer + conversionBufferPos, framesInThisPass, &inputFramesUsed,
                                                     tempFloatBuffer + floatsResampled, framesNeeded - (floatsResampled / fileChannels), &outputFramesGenerated);
                
                if (soxr_err) {
                    CuemsLogger::getLogger()->logError("Resampling error: " + string(soxr_strerror(soxr_err)));
                    errorState = true;
                    break;
                }
                
                conversionBufferPos += inputFramesUsed * fileChannels;
                floatsResampled += outputFramesGenerated * fileChannels;
                
                if (outputFramesGenerated == 0 && inputFramesUsed == 0) {
                    break;  // No progress
                }
            }
        }
        
        // Copy resampled floats directly to output (JACK native format)
        size_t floatsToCopy = std::min(floatsResampled, samplesNeeded);
        memcpy(outputPtr, tempFloatBuffer, floatsToCopy * sizeof(float));
        lastBytesRead += floatsToCopy * 4;  // 4 bytes per float
        
        delete[] tempFloatBuffer;
        
    } else {
        // No resampling - direct decode and output as float
        while (bytesRemaining > 0) {
            // Ensure we have decoded float data
            if (!refillConversionBuffer()) {
                break;  // No more data
            }
            
            // Copy floats directly to output (JACK native format)
            size_t floatsAvailable = conversionBufferUsed - conversionBufferPos;
            size_t floatsToCopy = std::min(floatsAvailable, samplesNeeded - (lastBytesRead / 4));
            
            memcpy(outputPtr, conversionBuffer + conversionBufferPos, floatsToCopy * sizeof(float));
            outputPtr += floatsToCopy;
            lastBytesRead += floatsToCopy * 4;  // 4 bytes per float
            bytesRemaining -= floatsToCopy * 4;
            
            conversionBufferPos += floatsToCopy;
        }
    }
    
    currentSamplePos += lastBytesRead / 4;  // Track position in samples (4 bytes per float)
}

////////////////////////////////////////////
// Seek to position
////////////////////////////////////////////
void AudioFstream::seekg(long long pos, ios_base::seekdir dir)
{
    if (!fileOpen || !fileReader.isReady() || feedReady) {
        return;
    }
    
    long long targetBytePos = 0;
    
    // Calculate absolute byte position
    if (dir == ios_base::beg) {
        targetBytePos = pos;
    } else if (dir == ios_base::cur) {
        targetBytePos = currentSamplePos * 4 + pos;  // currentSamplePos is in samples, *4 for bytes (float)
    } else if (dir == ios_base::end) {
        // Use getFileSize() to get the correct size (accounting for resampling)
        targetBytePos = (long long)getFileSize() + pos;
    }
    
    // Convert byte position to sample position (32-bit float samples)
    int64_t targetSamplePos = targetBytePos / 4;
    
    // Account for resampling ratio
    int64_t fileSamplePos = targetSamplePos;
    if (resamplingEnabled && targetSampleRate > 0) {
        fileSamplePos = (int64_t)((double)targetSamplePos * fileSampleRate / targetSampleRate);
    }
    
    // Convert to time in seconds
    double timeSeconds = (double)(fileSamplePos / fileChannels) / fileSampleRate;
    
    // Seek using MediaFileReader
    if (!fileReader.seekToTime(timeSeconds, audioStreamIndex, AVSEEK_FLAG_BACKWARD)) {
        CuemsLogger::getLogger()->logError("Seek error");
        errorState = true;
        return;
    }
    
    // Flush decoder buffers
    audioDecoder.flush();
    
    // Reset conversion buffer
    conversionBufferUsed = 0;
    conversionBufferPos = 0;
    
    // Clear EOF and draining state
    eofReached = false;
    demuxEof = false;
    decoderDrained = false;
    resamplerDrained = false;
    decodedFrames = 0;
    pendingDiscard = 0;
    firstDeliveredKnown = false;
    calibratePending = false;
    convKnown = false;
    fromFirstPacket = false;
    
    // Reset resampler state if active (critical for looping/seeking)
    if (resamplingEnabled && resampler) {
        soxr_clear(resampler);
    }
    // Update current position
    currentSamplePos = targetSamplePos;
}

////////////////////////////////////////////
// Get count of last read
////////////////////////////////////////////
streamsize AudioFstream::gcount() const
{
    return lastBytesRead;
}

////////////////////////////////////////////
// Check if EOF reached
////////////////////////////////////////////
bool AudioFstream::eof() const
{
    return eofReached || (conversionBufferPos >= conversionBufferUsed && eofReached);
}

////////////////////////////////////////////
// Check if stream is good
////////////////////////////////////////////
bool AudioFstream::good() const
{
    return fileOpen && !errorState && !eofReached;
}

////////////////////////////////////////////
// Check if stream has error
////////////////////////////////////////////
bool AudioFstream::bad() const
{
    return errorState;
}

////////////////////////////////////////////
// Clear error states
////////////////////////////////////////////
void AudioFstream::clear()
{
    errorState = false;
    eofReached = false;
    resamplerDrained = false;
}

////////////////////////////////////////////
// Cleanup FFmpeg resources
////////////////////////////////////////////
void AudioFstream::cleanupFFmpeg()
{
    if (frame) {
        av_frame_free(&frame);
    }
    if (packet) {
        av_packet_free(&packet);
    }
    if (swrContext) {
        swr_free(&swrContext);
    }
    if (conversionBuffer) {
        delete[] conversionBuffer;
        conversionBuffer = nullptr;
    }
    
    // Close cuems-mediadecoder components
    audioDecoder.close();
    fileReader.close();
    
    audioStreamIndex = -1;
    conversionBufferSize = 0;
    conversionBufferUsed = 0;
    conversionBufferPos = 0;
}

////////////////////////////////////////////
// Close file
////////////////////////////////////////////
void AudioFstream::close()
{
    // E19: the worker is joined before anything it may touch goes away (K4)
    stopWorker();
    if (vrs) {
        soxr_delete(vrs);
        vrs = nullptr;
    }
    feedReady = false;
    cache.clear();
    resetFeedState();
    cleanupFFmpeg();
    cleanupResampler();
    
    fileOpen = false;
    eofReached = false;
    demuxEof = false;
    decoderDrained = false;
    resamplerDrained = false;
    decodedFrames = 0;
    errorState = false;
    fileChannels = 0;
    fileSampleRate = 0;
    totalSamples = 0;
    fileSize = 0;
    currentSamplePos = 0;
    lastBytesRead = 0;
}

////////////////////////////////////////////
// File information accessors
////////////////////////////////////////////
unsigned long long AudioFstream::getFileSize() const
{
    // If resampling is enabled, return the effective output size at target sample rate
    // This ensures boundary checks compare values at the same sample rate
    if (resamplingEnabled && targetSampleRate > 0 && fileSampleRate > 0 && totalSamples > 0) {
        // Calculate output samples at target sample rate (same duration, different sample count)
        int64_t outputSamples = (int64_t)((double)totalSamples * targetSampleRate / fileSampleRate);
        return outputSamples * fileChannels * 4;  // 4 bytes per sample (32-bit float)
    }
    
    // No resampling - fileSize is already in output-channel bytes (see open())
    return fileSize;
}

int64_t AudioFstream::getDecodedFrames() const
{
    return decodedFrames;
}

unsigned int AudioFstream::getChannels() const
{
    return fileChannels;
}

unsigned int AudioFstream::getSampleRate() const
{
    return fileSampleRate;
}

unsigned int AudioFstream::getBitsPerSample() const
{
    return 32;  // Always output 32-bit float for JACK
}

////////////////////////////////////////////
// Set target sample rate (for resampling)
////////////////////////////////////////////
void AudioFstream::setTargetSampleRate(unsigned int rate)
{
    if (targetSampleRate == rate) {
        return;  // No change
    }
    
    targetSampleRate = rate;
    
    if (!fileOpen) {
        return;  // Will initialize when file is opened
    }
    
    // Reinitialize resampler if needed
    if (targetSampleRate > 0 && targetSampleRate != fileSampleRate) {
        initializeResampler();
    } else {
        // No resampling needed
        cleanupResampler();
        resamplingEnabled = false;
    }
}

////////////////////////////////////////////
// Set resampling quality
////////////////////////////////////////////
void AudioFstream::setResampleQuality(const string& quality)
{
    qualitySpec = parseQualityString(quality);
    
    // If resampler is already initialized, recreate it with new quality
    if (resampler != nullptr) {
        initializeResampler();
    }
}

////////////////////////////////////////////
// Parse quality string
////////////////////////////////////////////
soxr_quality_spec_t AudioFstream::parseQualityString(const string& quality)
{
    if (quality == "vhq") {
        return soxr_quality_spec(SOXR_VHQ, 0);
    } else if (quality == "hq") {
        return soxr_quality_spec(SOXR_HQ, 0);
    } else if (quality == "mq") {
        return soxr_quality_spec(SOXR_MQ, 0);
    } else if (quality == "lq") {
        return soxr_quality_spec(SOXR_LQ, 0);
    } else {
        CuemsLogger::getLogger()->logError("Invalid resample quality: " + quality + ", using HQ");
        return soxr_quality_spec(SOXR_HQ, 0);
    }
}

////////////////////////////////////////////
// Initialize resampler
////////////////////////////////////////////
void AudioFstream::initializeResampler()
{
    cleanupResampler();
    
    if (targetSampleRate == 0 || targetSampleRate == fileSampleRate) {
        resamplingEnabled = false;
        return;
    }
    
    soxr_error_t error;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    
    resampler = soxr_create(fileSampleRate, targetSampleRate, fileChannels,
                           &error, &io_spec, &qualitySpec, nullptr);
    
    if (error || !resampler) {
        CuemsLogger::getLogger()->logError("Failed to create resampler");
        resamplingEnabled = false;
        return;
    }
    
    resamplingEnabled = true;
    
    // Allocate resampling buffers
    // Increase buffer size for formats like DTS that may need more headroom
    resampleBufferSize = 16384 * fileChannels;  // 16384 frames worth (larger for complex codecs)
    resampleInputBuffer = new float[resampleBufferSize];
    resampleOutputBuffer = new float[resampleBufferSize];
    
    CuemsLogger::getLogger()->logOK("Resampler initialized: " + std::to_string(fileSampleRate) + 
                                    " Hz -> " + std::to_string(targetSampleRate) + " Hz");
}

////////////////////////////////////////////
// Cleanup resampler
////////////////////////////////////////////
void AudioFstream::cleanupResampler()
{
    if (resampler) {
        soxr_delete(resampler);
        resampler = nullptr;
    }
    if (resampleInputBuffer) {
        delete[] resampleInputBuffer;
        resampleInputBuffer = nullptr;
    }
    if (resampleOutputBuffer) {
        delete[] resampleOutputBuffer;
        resampleOutputBuffer = nullptr;
    }
    resampleBufferSize = 0;
    resamplingEnabled = false;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// Feed engine: plan 869ejxng3 + 1amfwm7e6cw rev 3, sections 4.3b
// (always-on VR soxr, demand feeding), 4.3c (the wrap below the
// resampler, on P) and the normative 4.3h v2.1 (jobs, start rule,
// events, invariants). Cell and finding names (E12, T1, ...) refer to
// that plan.
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace {
const int64_t INF_SAMPLES = INT64_MAX / 4;
const double FEED_LEAD = 64.0;             // demand feeding's initial lead (input frames)
const size_t MAX_CHUNK = 8192;             // frames offered to soxr per call
const double DRAIN_TAIL = 32.0;            // input samples of ring-out kept after the content end (T7-1)

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void AudioFstream::resetFeedState()
{
    cacheH = 0;
    lDec = -1;
    headerLen = 0;
    loopPasses = 1;
    loopNum = 0;
    loopDen = 0;
    pS = 0;
    mode = LoopMode::PENDING;
    offLatched = false;
    lpChecked = false;
    psState = Ps::HOLD;
    feedSrc = Feed::CACHE;
    outPlay = false;
    cur = 0;
    feedPass = 0;
    soxrPristineFlag = true;
    decAtHFlag = true;
    decoderAtHTruth = true;
    seekFailed = false;
    seekTries = 0;
    retryFramesLeft = 0;
    passIdx = 0;
    wrapCountPending = 0;
    originAbs = 0;
    sumRN = sumIdone = 0.0;
    owed = FEED_LEAD;
    consumed = 0.0;
    fadeLeft = fadeLen = 0;
    passesAtEnd = 1;
    waitingKind = StartKind::REVEAL;
    waitingStart = false;
    pickupPending = false;
    chasing = false;
    chaseLanding = 0;
    lateAtFrames = -1;
    framesOut = 0;
    resetAfterStart = false;
    wrapFailed = false;
    fadeOutLeft = fadeOutLen = 0;
    drainEndContent = 0.0;
    nEvents = 0;
    invViolations = 0;
    lastProduced = 0;
    outstanding = 0;
    for (int i = 0; i < 4; i++) outstandingKind[i] = 0;
    inputTap = nullptr;
    inputTapUser = nullptr;
}

////////////////////////////////////////////
// Matroska: the seek index by density (M4)
////////////////////////////////////////////
// Mid-file seeks on an A/V matroska use the video stream's index when it is
// dense (>= 2 entries and >= one per 10 s): the audio track carries almost
// no cues there. A file written to a pipe has none and seeks on the audio
// index. Every other container seeks on the audio stream.
void AudioFstream::chooseSeekIndex()
{
    AVFormatContext* fc = fileReader.getFormatContext();
    seekStreamIndex = audioStreamIndex;
    gopSeconds = 2.0;
    isMatroska = fc->iformat && fc->iformat->name && strstr(fc->iformat->name, "matroska") != nullptr;
    if (!isMatroska) {
        return;
    }
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || (st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            continue;
        }
        const int n = avformat_index_get_entries_count(st);
        const double dur = (fc->duration != AV_NOPTS_VALUE) ? fc->duration / (double)AV_TIME_BASE : 0.0;
        if (n >= 2 && n >= dur / 10.0) {
            seekStreamIndex = (int)i;
            double maxGap = 0.0;
            for (int k = 1; k < n; k++) {
                const AVIndexEntry* a = avformat_index_get_entry(st, k - 1);
                const AVIndexEntry* b = avformat_index_get_entry(st, k);
                if (a && b) maxGap = std::max(maxGap, (b->timestamp - a->timestamp) * av_q2d(st->time_base));
            }
            if (maxGap > 0) gopSeconds = maxGap;
        }
        CuemsLogger::getLogger()->logInfo("Matroska seek index: " +
            std::string(seekStreamIndex == audioStreamIndex ? "audio" : "video") +
            " (" + std::to_string(n) + " video cue entries)");
        break;
    }
}

int64_t AudioFstream::nextSampleIndex() const
{
    if (!convKnown || fileChannels == 0) {
        return INT64_MIN;
    }
    return convStartSample + (int64_t)(conversionBufferPos / fileChannels);
}

// Decode-and-discard until the next delivered sample is s. False on EOF (with
// the stream not at s), an error, or a superseded job.
bool AudioFstream::discardTo(int64_t s, bool checkGen, uint64_t g)
{
    while (true) {
        if (checkGen && gen.load(std::memory_order_acquire) != g) {
            return false;
        }
        if (conversionBufferPos >= conversionBufferUsed) {
            const int64_t endIdx = convKnown ? convStartSample + (int64_t)(conversionBufferUsed / fileChannels) : INT64_MIN;
            if (!refillConversionBuffer()) {
                return endIdx == s;   // EOF exactly at s (a file that lives in the cache)
            }
            continue;
        }
        const int64_t nx = nextSampleIndex();
        if (nx == INT64_MIN) {
            return false;
        }
        if (nx >= s) {
            return true;
        }
        const int64_t avail = (int64_t)((conversionBufferUsed - conversionBufferPos) / fileChannels);
        conversionBufferPos += (size_t)std::min(avail, s - nx) * fileChannels;
    }
}

////////////////////////////////////////////
// E1 completion
////////////////////////////////////////////
bool AudioFstream::prepareFeed(unsigned outRate, unsigned periodFrames, bool threaded)
{
    if (!fileOpen || errorState || outRate == 0 || periodFrames == 0) {
        return false;
    }
    stopWorker();
    if (vrs) {
        soxr_delete(vrs);
        vrs = nullptr;
    }
    const int64_t keepHeader = headerLen;
    resetFeedState();
    headerLen = keepHeader;
    outRateFeed = outRate;
    periodFramesFeed = periodFrames;
    rNom = (double)fileSampleRate / (double)outRate;
    const unsigned ch = fileChannels;

    // The head cache: the first H = min(1 s, L) samples, rounded up to the
    // end of a decoded frame, so the handover is a frame boundary (K1, K5,
    // L12a). The decoder is left at H; the cache is a linear decode.
    cache.clear();
    cache.reserve((size_t)(fileSampleRate + 16384) * ch);
    while (true) {
        if (!refillConversionBuffer()) {
            if (errorState) {
                return false;
            }
            lDec = (int64_t)(cache.size() / ch);   // the whole file lives in the cache
            break;
        }
        cache.insert(cache.end(), conversionBuffer + conversionBufferPos, conversionBuffer + conversionBufferUsed);
        conversionBufferPos = conversionBufferUsed;
        if ((int64_t)(cache.size() / ch) >= (int64_t)fileSampleRate) {
            break;
        }
    }
    cacheH = (int64_t)(cache.size() / ch);
    decoderAtHTruth = true;
    decAtHFlag = true;

    // Always-on variable-ratio soxr. The create-time ratio is the MAXIMUM:
    // set the nominal ratio at once, or the first period glides from +1 %.
    soxr_error_t err = nullptr;
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
    vrs = soxr_create(fileSampleRate * 1.01, outRate, ch, &err, &io, &q, nullptr);
    if (!err && vrs) {
        err = soxr_set_io_ratio(vrs, rNom, 0);
    }
    if (err || !vrs) {
        CuemsLogger::getLogger()->logError(std::string("Failed to create the VR resampler: ") + (err ? err : "null"));
        if (vrs) {
            soxr_delete(vrs);
            vrs = nullptr;
        }
        return false;
    }
    lastRatio = rNom;
    zeros.assign(MAX_CHUNK * ch, 0.0f);
    calibratePosConstant();

    feedReady = true;
    threadedWorker = threaded;
    if (threaded) {
        if (sem_init(&workerSem, 0, 0) == 0) {
            semInit = true;
            workerQuit.store(false);
            workerThread = std::thread(&AudioFstream::workerLoop, this);
        } else {
            threadedWorker = false;
        }
    }
    CuemsLogger::getLogger()->logInfo("Feed ready: head cache " + std::to_string(cacheH) + " samples" +
        (lDec >= 0 ? " (whole file)" : "") + ", VR soxr " + std::to_string(fileSampleRate) + " -> " +
        std::to_string(outRate) + ", pos constant " + std::to_string(posC));
    return true;
}

// c of pos = origin + sum(r N) + c: an impulse train through a scratch soxr
// with the same spec, demand-fed at the nominal ratio (section 4.3a).
void AudioFstream::calibratePosConstant()
{
    posC = 0.0;
    soxr_error_t err = nullptr;
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
    soxr_t s = soxr_create(fileSampleRate * 1.01, outRateFeed, 1, &err, &io, &q, nullptr);
    if (err || !s) {
        if (s) soxr_delete(s);
        return;
    }
    soxr_set_io_ratio(s, rNom, 0);
    const size_t inLen = 40000;
    const int64_t imp[4] = {4000, 12000, 20000, 28000};
    std::vector<float> in(inLen, 0.0f), outv;
    for (int64_t p : imp) in[p] = 1.0f;
    const size_t N = periodFramesFeed;
    std::vector<float> buf(N);
    double want = FEED_LEAD, used = 0.0;
    size_t ipos = 0;
    while (ipos < inLen && outv.size() < (size_t)(inLen / rNom) + N) {
        want += rNom * N;
        size_t done = 0;
        for (int guard = 0; done < N && guard < 64 && ipos < inLen; guard++) {
            size_t w = (want - used >= 1.0) ? (size_t)(want - used) : 0;
            if (guard > 0) w = std::max(w, (size_t)std::ceil((N - done) * rNom) + 1);
            const size_t n = std::min(w, inLen - ipos);
            size_t idone = 0, odone = 0;
            soxr_process(s, in.data() + ipos, n, &idone, buf.data() + done, N - done, &odone);
            ipos += idone; used += idone; done += odone;
        }
        outv.insert(outv.end(), buf.begin(), buf.begin() + done);
    }
    soxr_delete(s);
    double sum = 0.0;
    int n = 0;
    for (int64_t p : imp) {
        const long e = (long)std::llround(p / rNom);
        long best = -1;
        float bv = 0.0f;
        for (long k = std::max(1L, e - 300); k < std::min((long)outv.size() - 1, e + 300); k++) {
            if (outv[k] > bv) { bv = outv[k]; best = k; }
        }
        if (best < 1) continue;
        const double y0 = outv[best - 1], y1 = outv[best], y2 = outv[best + 1];
        const double den = y0 - 2 * y1 + y2;
        const double qk = best + ((den != 0.0) ? 0.5 * (y0 - y2) / den : 0.0);
        sum += (double)p - qk * rNom;
        n++;
    }
    if (n > 0) posC = sum / n;
}

////////////////////////////////////////////
// Loop configuration (E5)
////////////////////////////////////////////
void AudioFstream::setLoop(int passes, int64_t num, int64_t den)
{
    loopPasses = passes;
    loopNum = num;
    loopDen = den;
    pS = (num > 0 && den > 0) ?
        (int64_t)(((__int128)num * fileSampleRate + den / 2) / den) : 0;
    const LoopMode before = mode;
    if (offLatched) {
        mode = LoopMode::OFF;   // latched per file: a re-send never re-arms it (N8)
    } else if (passes == 1) {
        mode = LoopMode::ONCE;
    } else {
        const int64_t p = periodForStart();
        AVCodecParameters* par = fileReader.getFormatContext() ?
            fileReader.getFormatContext()->streams[audioStreamIndex]->codecpar : nullptr;
        const int64_t codecFrame = (par && par->frame_size > 0) ? par->frame_size : 0;
        const int64_t minLoop = std::max<int64_t>(fileSampleRate / 10,
            (int64_t)std::ceil(2.0 * periodFramesFeed * rNom) + 2 * codecFrame);
        if (p > 0 && p < INF_SAMPLES && p < minLoop) {
            offLatched = true;
            mode = LoopMode::OFF;
            ev(EvType::TINY_LOOP, p, minLoop);
        } else {
            mode = LoopMode::WRAP;
            if (pS > 0 && headerLen > 0 && pS < headerLen - (int64_t)fileSampleRate) {
                ev(EvType::HEADER_SHORT, pS, headerLen);   // P_s more than 1 s short of the header (L12h)
            }
        }
    }
    if (mode != before) {
        ev(EvType::MODE_CHANGED, (int64_t)mode);
    }
}

int AudioFstream::takeWrapCount()
{
    const int n = wrapCountPending;
    wrapCountPending = 0;
    return n;
}

int64_t AudioFstream::periodSamples() const
{
    if (pS > 0) return pS;
    if (lDec >= 0) return lDec;
    return INF_SAMPLES;   // num = 0 before the first EOF: the boundary is the true EOF (E24)
}

// The period for the start arithmetic (t mod P_s, passIndex, origin, end):
// before the first EOF with num = 0 it is the container's header length
// (section 4.3h, h.1); the pass BOUNDARY stays at the true EOF then
// (periodSamples()), so a header shorter than the decode never cuts pass 1.
int64_t AudioFstream::periodForStart() const
{
    if (pS > 0) return pS;
    if (lDec >= 0) return lDec;
    if (headerLen > 0) return headerLen;
    return INF_SAMPLES;
}

int64_t AudioFstream::endSamples() const
{
    if (loopPasses <= 0) return INF_SAMPLES;
    const int64_t p = periodForStart();
    if (p >= INF_SAMPLES) return INF_SAMPLES;
    return (int64_t)loopPasses * p;
}

int64_t AudioFstream::wrapsLeft() const
{
    if (mode == LoopMode::OFF) return 0;
    if (loopPasses <= 0) return INF_SAMPLES;
    return std::max<int64_t>(0, (int64_t)loopPasses - 1 - passIdx);
}

bool AudioFstream::cacheOnly() const
{
    return periodSamples() <= cacheH || (lDec >= 0 && cacheH == lDec);
}

int64_t AudioFstream::cursor() const
{
    return (feedSrc == Feed::DECODER) ? nextSampleIndex() : cur;
}

double AudioFstream::pos() const
{
    return (double)originAbs + sumRN + posC;
}

AudioFstream::JobKind AudioFstream::runningOrPendingJob() const
{
    for (int k : {1, 3, 2}) {   // SEEK, RESET, WRAP
        if (outstandingKind[k] > 0) return (JobKind)k;
    }
    return JobKind::NONE;
}

void AudioFstream::ev(EvType t, int64_t a, int64_t b, int kind)
{
    if (nEvents < MAX_EVENTS) {
        events[nEvents++] = FeedEvent{t, a, b, kind};
    }
}

int AudioFstream::takeEvents(FeedEvent* out, int max)
{
    const int n = std::min(max, nEvents);
    for (int i = 0; i < n; i++) out[i] = events[i];
    // keep any overflow in order
    for (int i = n; i < nEvents; i++) events[i - n] = events[i];
    nEvents -= n;
    return n;
}

void AudioFstream::violate(int64_t code)
{
    if (invViolations++ == 0) {
        ev(EvType::INVARIANT, code);
    }
}

////////////////////////////////////////////
// Posting a job (audio thread)
////////////////////////////////////////////
void AudioFstream::post(JobKind k, int64_t tPass)
{
    if (k == JobKind::SEEK || k == JobKind::RESET) {
        // T1: a start must never trust the values from before the post
        soxrPristineFlag = false;
        decAtHFlag = false;
    }
    if (k == JobKind::WRAP && mode == LoopMode::OFF) {
        violate(5);
        return;
    }
    const int p = pendingIdx.load(std::memory_order_acquire);
    const int h = holdingIdx.load(std::memory_order_acquire);
    int idx = 0;
    while (idx == p || idx == h) idx++;
    jobs[idx].kind = k;
    jobs[idx].tPass = tPass;
    jobs[idx].endAbs = endSamples();
    jobs[idx].pS = periodForStart();
    jobs[idx].gen = gen.load(std::memory_order_relaxed);
    const int old = pendingIdx.exchange(idx, std::memory_order_acq_rel);
    if (old >= 0) {
        outstandingKind[(int)jobs[old].kind]--;   // never taken: dropped, no result
    } else {
        outstanding++;
    }
    outstandingKind[(int)k]++;
    if (threadedWorker && semInit) {
        sem_post(&workerSem);
    }
}

////////////////////////////////////////////
// The worker (h.2)
////////////////////////////////////////////
void AudioFstream::workerLoop()
{
    while (!workerQuit.load()) {
        sem_wait(&workerSem);
        if (workerQuit.load()) break;
        runPendingJobs();
    }
}

void AudioFstream::runPendingJobs()
{
    while (!workerQuit.load()) {
        int idx = pendingIdx.load(std::memory_order_acquire);
        if (idx < 0) return;
        holdingIdx.store(idx, std::memory_order_release);
        if (!pendingIdx.compare_exchange_strong(idx, -1, std::memory_order_acq_rel)) {
            holdingIdx.store(-1, std::memory_order_release);
            continue;   // replaced meanwhile: take the newer one
        }
        const Job j = jobs[idx];
        runJob(j);
        holdingIdx.store(-1, std::memory_order_release);
    }
}

void AudioFstream::workerRunPending()
{
    if (threadedWorker) return;
    int idx = pendingIdx.load(std::memory_order_acquire);
    if (idx < 0 || resultReady.load(std::memory_order_acquire)) return;
    holdingIdx.store(idx, std::memory_order_release);
    if (!pendingIdx.compare_exchange_strong(idx, -1, std::memory_order_acq_rel)) {
        holdingIdx.store(-1, std::memory_order_release);
        return;
    }
    const Job j = jobs[idx];
    runJob(j);
    holdingIdx.store(-1, std::memory_order_release);
}

// Every step checks the generation first; a superseded job stops at the end
// of the step it is in (no interrupt_callback, L1).
bool AudioFstream::stepOk(const Job& j, int step)
{
    if (stepHook) stepHook(j.kind, step);
    return gen.load(std::memory_order_acquire) == j.gen;
}

void AudioFstream::clearSoxr()
{
    if (!inWorker) clearsOutsideWorker.fetch_add(1);
    // soxr_clear resets the ratio to the create-time maximum: set it again (G12)
    soxr_clear(vrs);
    soxr_set_io_ratio(vrs, rNom, 0);
}

void AudioFstream::publishResult(const JobResult& r)
{
    while (resultReady.load(std::memory_order_acquire) && !workerQuit.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    result = r;
    resultReady.store(true, std::memory_order_release);
}

void AudioFstream::runJob(const Job& j)
{
    inWorker = true;
    const double t0 = nowMs();
    JobResult r;
    r.kind = j.kind;
    r.gen = j.gen;
    int fk = (int)j.kind;
    const bool inject = failNext.load() == fk && failNext.compare_exchange_strong(fk, 0);
    bool ok = true, abandoned = false;

    switch (j.kind) {
    case JobKind::SEEK:
        ok = midFileSeek(j, r);
        abandoned = (r.status == JobStatus::ABANDONED);
        decoderAtHTruth = false;
        break;
    case JobKind::WRAP:
        // 1-4: first-packet seek on the audio stream, flush, landing pts
        // asserted, Opus pre-skip (firstPacketSeek); 5: decode-and-discard to H
        if (!stepOk(j, 1)) { abandoned = true; break; }
        decoderAtHTruth = false;
        if (inject || !firstPacketSeek(false)) { ok = false; break; }
        if (!stepOk(j, 5)) { abandoned = true; break; }
        if (!discardTo(cacheH, true, j.gen)) {
            if (gen.load() != j.gen) abandoned = true; else ok = false;
            break;
        }
        decoderAtHTruth = true;
        break;
    case JobKind::RESET:
        // 1: soxr_clear + ratio; 2: the decoder back at H if it is not there
        if (!stepOk(j, 1)) { abandoned = true; break; }
        clearSoxr();
        if (inject) { ok = false; break; }
        if (!decoderAtHTruth) {
            if (!stepOk(j, 2)) { abandoned = true; break; }
            if (!firstPacketSeek(false)) { ok = false; break; }
            if (!discardTo(cacheH, true, j.gen)) {
                if (gen.load() != j.gen) abandoned = true; else ok = false;
                break;
            }
            decoderAtHTruth = true;
        }
        break;
    case JobKind::NONE:
        break;
    }
    if (inject && j.kind == JobKind::SEEK && !abandoned) ok = false;
    r.status = abandoned ? JobStatus::ABANDONED : (ok ? JobStatus::DONE : JobStatus::FAILED);
    r.ms = nowMs() - t0;
    r.lDec = lDec;
    static const char* names[] = {"none", "seek", "wrap", "reset"};
    if (r.status == JobStatus::FAILED) {
        CuemsLogger::getLogger()->logError(std::string(names[(int)j.kind]) + " failed");
    } else {
        CuemsLogger::getLogger()->logDebug(std::string("job ") + names[(int)j.kind] +
            (r.status == JobStatus::DONE ? " done" : " abandoned") + " in " + std::to_string((int)r.ms) + " ms");
    }
    inWorker = false;
    publishResult(r);
}

// SEEK(t_pass): sample-exact seek in the file domain with pre-roll, the
// live-target chase, then the soxr clear (h.2; section 4.3a).
bool AudioFstream::midFileSeek(const Job& j, JobResult& r)
{
    AVFormatContext* fc = fileReader.getFormatContext();
    AVStream* ast = fc->streams[audioStreamIndex];
    AVCodecParameters* par = ast->codecpar;
    const int64_t frameSize = par->frame_size > 0 ? par->frame_size : 0;
    const int64_t codecPreroll = std::max<int64_t>(0, par->seek_preroll);
    // Twice the codec's declared pre-roll: Opus does not converge in its 80 ms
    // (up to 0.06 off on casas), it does in 160 ms (1e-5)
    const int64_t preroll = std::max<int64_t>(2 * codecPreroll, 4 * frameSize);
    const int64_t tp = j.tPass;
    const int64_t s0 = std::max<int64_t>(0, tp - preroll);
    const int64_t gop2 = (int64_t)(2.0 * gopSeconds * fileSampleRate);
    const int64_t msTol = isMatroska ? fileSampleRate / 1000 : 0;   // matroska's 1 ms index
    // Acceptance: the landing leaves the codec twice its declared pre-roll
    // before the target (seek_preroll, in samples: 0 for PCM and most codecs,
    // 3840 for Opus at 48 kHz, so 7680 = 160 ms). `preroll` above is the
    // margin of the request. A target inside that from the start accepts sample 0.
    const int64_t bound = std::max<int64_t>(0, tp - 2 * codecPreroll);
    const bool videoIndex = (seekStreamIndex != audioStreamIndex);
    auto toTs = [&](int si, int64_t sample) -> int64_t {
        const int64_t base = (contentStartTs != AV_NOPTS_VALUE) ? contentStartTs :
                             (audioStartTime == AV_NOPTS_VALUE ? 0 : audioStartTime);
        const int64_t ats = base + av_rescale_q(sample, (AVRational){1, (int)fileSampleRate}, ast->time_base);
        return (si == audioStreamIndex) ? ats : av_rescale_q(ats, ast->time_base, fc->streams[si]->time_base);
    };
    struct Attempt { int si; int64_t at; };
    const Attempt attempts[3] = {
        {seekStreamIndex, s0},
        // landed after the bound: 2 GOPs earlier (audio-only: 2 pre-rolls + 1 s earlier)
        {seekStreamIndex, std::max<int64_t>(0, videoIndex ? tp - gop2 : tp - 2 * preroll - (int64_t)fileSampleRate)},
        {audioStreamIndex, s0},                                // then the audio index
    };
    bool landed = false;
    int64_t landing = 0;
    for (int a = 0; a < 3 && !landed; a++) {
        if (!stepOk(j, 1)) { r.status = JobStatus::ABANDONED; return false; }
        if (a > 0) seekRetries.fetch_add(1);
        const Attempt& at = attempts[a];
        const int ret = avformat_seek_file(fc, at.si, INT64_MIN, toTs(at.si, at.at), toTs(at.si, std::max(at.at, tp)),
                                           AVSEEK_FLAG_BACKWARD);
        if (!stepOk(j, 2)) { r.status = JobStatus::ABANDONED; return false; }
        audioDecoder.flush();
        conversionBufferUsed = conversionBufferPos = 0;
        eofReached = demuxEof = decoderDrained = resamplerDrained = false;
        pendingDiscard = 0;
        firstDeliveredKnown = false;
        calibratePending = false;
        convKnown = false;
        fromFirstPacket = false;
        if (ret < 0) continue;
        if (!refillConversionBuffer() || !convKnown) {
            if (errorState) return false;
            continue;
        }
        landing = nextSampleIndex();
        landed = landing <= bound + msTol;
        if (!landed) {
            CuemsLogger::getLogger()->logWarning("Seek to " + std::to_string(tp) + " landed at " +
                                                 std::to_string(landing) + " (attempt " + std::to_string(a + 1) + ")");
        } else if (tp - landing > gop2) {
            CuemsLogger::getLogger()->logWarning("Seek landed more than one GOP early: " + std::to_string(tp - landing) + " samples");
        }
    }
    if (!landed) {
        return false;
    }
    lastSeekLead.store(tp - landing);
    // 3: decode-and-discard to (live target mod P_s) - 1 period, re-reading the live target
    const int64_t lead = (int64_t)std::ceil(periodFramesFeed * rNom);
    const int64_t pp = (j.pS > 0 && j.pS < INF_SAMPLES) ? j.pS : INF_SAMPLES;
    while (true) {
        if (!stepOk(j, 3)) { r.status = JobStatus::ABANDONED; return false; }
        const int64_t live = liveTarget.load(std::memory_order_acquire);
        if (live >= j.endAbs) { r.pastEnd = true; break; }       // the chase crossed end: E13
        const int64_t lt = (pp < INF_SAMPLES && live >= 0) ? live % pp : live;
        const int64_t nx = nextSampleIndex();
        if (nx == INT64_MIN) return false;
        if (lt < landing) break;                 // crossed P_s: the pickup re-runs h.4 (T7)
        const int64_t want = lt - lead;
        if (nx >= want) break;
        if (!discardTo(std::min(want, nx + 4096), true, j.gen)) {
            if (gen.load() != j.gen) { r.status = JobStatus::ABANDONED; return false; }
            if (errorState) return false;
            r.hitEof = true;
            break;
        }
    }
    // 4: soxr_clear + set ratio, on the worker (L10)
    if (!stepOk(j, 4)) { r.status = JobStatus::ABANDONED; return false; }
    clearSoxr();
    // 5: stage
    r.landing = r.hitEof ? (lDec >= 0 ? lDec : landing) : nextSampleIndex();
    if (r.hitEof && lDec < 0 && convKnown) {
        lDec = convStartSample + (int64_t)(conversionBufferUsed / fileChannels);
    }
    if (!r.hitEof && !r.pastEnd) r.landing = nextSampleIndex();
    r.status = JobStatus::DONE;
    return true;
}

void AudioFstream::stopWorker()
{
    if (workerThread.joinable()) {
        workerQuit.store(true);
        gen.fetch_add(1);
        if (semInit) sem_post(&workerSem);
        workerThread.join();
    }
    if (semInit) {
        sem_destroy(&workerSem);
        semInit = false;
    }
    pendingIdx.store(-1);
    holdingIdx.store(-1);
    resultReady.store(false);
    workerQuit.store(false);
}

////////////////////////////////////////////
// The audio thread: hand-back (E15-E17)
////////////////////////////////////////////
void AudioFstream::serviceWorker(const LiveCtx& ctx)
{
    if (!feedReady) return;
    publishTarget(ctx.target);
    if (resultReady.load(std::memory_order_acquire)) {
        const JobResult r = result;
        resultReady.store(false, std::memory_order_release);
        outstanding--;
        outstandingKind[(int)r.kind]--;
        if (r.lDec >= 0 && lDec < 0) {
            lDec = r.lDec;
        }
        const bool current = (r.gen == gen.load()) && r.status != JobStatus::ABANDONED;
        if (!current) {
            // E17: superseded. h.2: an abandoned SEEK/RESET leaves soxr FED, dec MID;
            // an abandoned WRAP leaves dec MID
            if (r.kind == JobKind::SEEK || r.kind == JobKind::RESET) {
                soxrPristineFlag = false;
                decAtHFlag = false;
            } else if (r.kind == JobKind::WRAP) {
                decAtHFlag = false;
            }
            // E4 in SEEKING: the waiting start re-runs h.4 when the worker hands back
            if (psState == Ps::SEEKING && waitingStart && outstanding == 0) {
                waitingStart = false;
                start(ctx.target, waitingKind);
            }
        } else if (r.status == JobStatus::DONE) {
            switch (r.kind) {
            case JobKind::RESET:
                soxrPristineFlag = true;
                decAtHFlag = true;
                if (psState == Ps::SEEKING && waitingStart) {
                    // N10: re-run from step 0 with the live target
                    waitingStart = false;
                    start(ctx.target, waitingKind);
                }
                break;
            case JobKind::SEEK:
                soxrPristineFlag = true;
                decAtHFlag = false;
                if (psState != Ps::SEEKING) {
                    violate(15);
                } else if (r.pastEnd) {
                    enterPastEnd(false);
                    feedSrc = Feed::ENDED;
                    ev(EvType::DRAINED);
                    post(JobKind::RESET);
                } else if (ctx.mtcLive) {
                    pickup(ctx, r);
                } else {
                    pickupPending = true;   // N14: waits for liveness
                    pendingPickup = r;
                }
                break;
            case JobKind::WRAP:
                decAtHFlag = true;          // waits for E14 in PLAY
                break;
            case JobKind::NONE:
                break;
            }
        } else {
            // E16
            switch (r.kind) {
            case JobKind::SEEK:
                decAtHFlag = false;
                if (psState == Ps::SEEKING) {
                    ev(EvType::SEEK_FAILED, ++seekTries);
                    if (seekTries == 1) {
                        retryFramesLeft = outRateFeed;          // one retry after 1 s
                    } else {
                        psState = Ps::HOLD;                     // T3: latched until E2/E4
                        outPlay = false;
                        seekFailed = true;
                    }
                }
                break;
            case JobKind::RESET:
                ev(EvType::RESET_FAILED);
                soxrPristineFlag = false;
                decAtHFlag = false;
                if (psState == Ps::SEEKING && waitingStart) {
                    waitingStart = false;
                    const int64_t p = periodForStart();
                    const int64_t t = ctx.target;
                    gen.fetch_add(1);
                    post(JobKind::SEEK, (looping() && p < INF_SAMPLES && t >= 0) ? t % p : std::max<int64_t>(0, t));
                }
                break;
            case JobKind::WRAP:
                // mode off latched; in PLAY the cursor reaching H takes the E13 path (T5)
                offLatched = true;
                if (mode != LoopMode::OFF) {
                    mode = LoopMode::OFF;
                    ev(EvType::MODE_CHANGED, (int64_t)mode);
                }
                ev(EvType::WRAP_FAILED);
                decAtHFlag = false;
                if (psState == Ps::PLAY) {
                    wrapFailed = true;
                }
                break;
            case JobKind::NONE:
                break;
            }
        }
    }
    if (pickupPending && ctx.mtcLive && psState == Ps::SEEKING) {
        pickupPending = false;
        pickup(ctx, pendingPickup);
    }
}

bool AudioFstream::looping() const
{
    return loopPasses != 1;
}

// The pickup (h.2 SEEK on done), with the T6 and T7 checks first.
void AudioFstream::pickup(const LiveCtx& ctx, const JobResult& r)
{
    const int64_t t = ctx.target;
    if (t < 0) {                                  // T6
        start(t, StartKind::OFFSET);
        return;
    }
    if (t >= endSamples()) {
        enterPastEnd(false);
        feedSrc = Feed::ENDED;
        post(JobKind::RESET);
        return;
    }
    const int64_t p = periodForStart();
    const int64_t tp = (looping() && p < INF_SAMPLES) ? t % p : t;
    if (lDec >= 0 && p > lDec && tp >= lDec && p < INF_SAMPLES) {
        beginOutput(t, tp, Feed::PAD, StartKind::OFFSET, false);   // the target is in the pad (T8)
        return;
    }
    if (tp < r.landing) {                         // T7: crossed P_s during the job
        psState = Ps::HOLD;
        outPlay = false;
        start(t, StartKind::OFFSET);
        return;
    }
    chasing = true;                               // the bounded discard finishes the chase
    chaseLanding = r.landing;
}

////////////////////////////////////////////
// h.4, the one start rule
////////////////////////////////////////////
void AudioFstream::start(int64_t t, StartKind kind)
{
    if (!feedReady) return;
    const bool wasHold = (psState == Ps::HOLD);
    chasing = false;
    pickupPending = false;
    retryFramesLeft = 0;
    // step 0
    if (t < 0) {
        outPlay = false;
        psState = Ps::HOLD;
        if (!soxrPristineFlag || !decAtHFlag) {
            post(JobKind::RESET);
        }
        return;
    }
    // step 1 (end, not endEff: a new start never extends the passes)
    if (t >= endSamples()) {
        const bool playing = (psState == Ps::PLAY && outPlay);
        if (playing) {
            feedSrc = Feed::DRAIN;
            enterPastEnd(true);
        } else {
            enterPastEnd(false);
            feedSrc = Feed::ENDED;
        }
        ev(EvType::PAST_END_START, t);
        return;
    }
    // step 2
    const int64_t p = periodForStart();
    const bool loop = looping() && p < INF_SAMPLES;
    const int64_t tp = loop ? t % p : t;
    passIdx = loop ? t / p : 0;
    // step 3
    const int64_t M = (int64_t)fileSampleRate / 4;   // 250 ms
    bool cacheStart;
    if (cacheOnly()) {
        const int64_t lim = (lDec >= 0) ? std::min(p, lDec) : p;
        cacheStart = tp < lim;
    } else {
        cacheStart = tp < cacheH - M;
    }
    const bool pad = (lDec >= 0 && p < INF_SAMPLES && p > lDec && tp >= lDec);
    if (cacheStart || pad) {
        if (soxrPristineFlag && (decAtHFlag || pad) && outstandingKind[1] == 0 && outstandingKind[3] == 0) {
            const bool clean = (kind == StartKind::REVEAL && wasHold && !resetAfterStart);
            beginOutput(t, tp, pad ? Feed::PAD : Feed::CACHE, kind, clean);
        } else {
            outPlay = false;
            psState = Ps::SEEKING;
            gen.fetch_add(1);
            post(JobKind::RESET);
            waitingStart = true;
            waitingKind = kind;
            resetAfterStart = true;
        }
        return;
    }
    // step 4
    outPlay = false;
    psState = Ps::SEEKING;
    gen.fetch_add(1);
    seekTries = 0;
    post(JobKind::SEEK, tp);
}

void AudioFstream::beginOutput(int64_t t, int64_t tPass, Feed src, StartKind kind, bool clean)
{
    const int64_t p = periodForStart();
    const bool loop = looping() && p < INF_SAMPLES;
    passIdx = loop ? t / p : 0;
    originAbs = (loop ? passIdx * p : 0) + tPass;
    feedSrc = src;
    cur = tPass;
    feedPass = passIdx;
    outPlay = true;
    psState = Ps::PLAY;
    owed = FEED_LEAD;
    consumed = 0.0;
    sumRN = sumIdone = 0.0;
    fadeLen = clean ? 0 : (int64_t)std::llround(0.002 * outRateFeed);
    fadeLeft = fadeLen;
    soxrPristineFlag = false;
    wrapFailed = false;
    seekTries = 0;
    ev(src == Feed::DECODER ? EvType::SEEK_PICKUP : EvType::CACHE_START, t, originAbs, (int)kind);
    resetAfterStart = false;
}

void AudioFstream::enterPastEnd(bool drain)
{
    psState = Ps::PASTEND;
    passesAtEnd = loopPasses;
    chasing = false;
    if (!drain) {
        outPlay = false;
    } else {
        ev(EvType::END, (int64_t)pos());
        // everything fed since the start (cache, decoder, pad) is content: the drain plays
        // until the output's content position has passed it (E13 "the output still playing the tail")
        drainEndContent = (double)originAbs + sumIdone;
        if (wrapFailed) {
            fadeOutLen = fadeOutLeft = std::max<int64_t>(64, (int64_t)(fifo() / rNom));
        }
    }
}

void AudioFstream::stopOutput()
{
    switch (psState) {
    case Ps::PLAY:
        outPlay = false;
        psState = Ps::HOLD;   // the feed stops where it is (N6)
        break;
    case Ps::SEEKING:
        gen.fetch_add(1);     // the job is abandoned
        psState = Ps::HOLD;
        waitingStart = false;
        chasing = false;
        pickupPending = false;
        retryFramesLeft = 0;
        break;
    case Ps::HOLD:
    case Ps::PASTEND:
        break;
    }
}

void AudioFstream::offsetChanged(int64_t t)
{
    if (psState == Ps::PLAY) {
        start(t, StartKind::OFFSET);
    } else if (psState == Ps::SEEKING) {
        gen.fetch_add(1);
        chasing = false;
        pickupPending = false;
        retryFramesLeft = 0;
        if (outstanding == 0) {
            start(t, StartKind::OFFSET);   // nothing to hand back: re-run now
        } else {
            waitingStart = true;
            waitingKind = StartKind::OFFSET;
        }
    }
}

bool AudioFstream::reenable(int64_t target)
{
    if (psState != Ps::PASTEND) return false;
    const bool morePasses = (loopPasses <= 0 && passesAtEnd > 0) || (passesAtEnd > 0 && loopPasses > passesAtEnd);
    const int64_t margin = 341 + (int64_t)fileSampleRate * 80 / 1000;
    const int64_t e = endSamples();
    if (morePasses || (e < INF_SAMPLES && target < e - margin) || e >= INF_SAMPLES) {
        psState = Ps::HOLD;
        outPlay = false;
        return true;
    }
    return false;
}

void AudioFstream::endNow()
{
    if (psState == Ps::PLAY && outPlay) {
        feedSrc = Feed::DRAIN;
        enterPastEnd(true);
    }
}

////////////////////////////////////////////
// Feeding (audio thread)
////////////////////////////////////////////
// E12 / E13 at the pass boundary.
void AudioFstream::onE12orE13()
{
    if (wrapsLeft() > 0) {
        feedPass++;
        cur = 0;
        feedSrc = Feed::CACHE;            // soxr continuous, no clear
        if (!cacheOnly()) {
            post(JobKind::WRAP);
        }
    } else {
        feedSrc = Feed::DRAIN;
        enterPastEnd(true);
    }
}

// Points ptr/avail at the next input of the current source, firing the
// boundary events on the way. False when no input this round (a boundary
// fired, or the output stopped).
bool AudioFstream::feedInput(const float*& ptr, size_t& avail, const LiveCtx& ctx)
{
    const unsigned ch = fileChannels;
    const int64_t p = periodSamples();
    switch (feedSrc) {
    case Feed::CACHE: {
        const bool co = cacheOnly();
        const int64_t lim = co ? std::min(p, cacheH) : cacheH;
        if (cur < lim) {
            ptr = cache.data() + (size_t)cur * ch;
            avail = (size_t)(lim - cur);
            return true;
        }
        if (!co) {
            if (wrapFailed) {                       // h.2 WRAP failure at H: fade out, E13 path (T5)
                feedSrc = Feed::DRAIN;
                enterPastEnd(true);
                return false;
            }
            if (decAtHFlag && outstandingKind[2] == 0 && outstandingKind[3] == 0 && outstanding == 0) {
                feedSrc = Feed::DECODER;            // E14: continuous, no fade, no clear
                return false;
            }
            // E18: late handover
            outPlay = false;
            psState = Ps::SEEKING;
            gen.fetch_add(1);
            lateAtFrames = framesOut;
            ev(EvType::HANDOVER_LATE, cur);
            const int64_t t = ctx.target;
            const int64_t ps = periodForStart();
            post(JobKind::SEEK, (looping() && ps < INF_SAMPLES && t >= 0) ? t % ps : std::max<int64_t>(0, t));
            return false;
        }
        if (cur >= p) {
            onE12orE13();
            return false;
        }
        feedSrc = Feed::PAD;                        // the whole file is in the cache and P_s > L
        return false;
    }
    case Feed::DECODER: {
        if (outstanding > 0) {
            violate(1);                             // the decoder belongs to the worker
            outPlay = false;
            psState = Ps::SEEKING;
            gen.fetch_add(1);
            const int64_t t = ctx.target;
            const int64_t ps = periodForStart();
            post(JobKind::SEEK, (looping() && ps < INF_SAMPLES && t >= 0) ? t % ps : std::max<int64_t>(0, t));
            return false;
        }
        const int64_t nx0 = nextSampleIndex();
        if (pS > 0 && nx0 != INT64_MIN && nx0 >= pS) {   // the cut at P_s applies from pass 1 (I14)
            onE12orE13();
            return false;
        }
        if (conversionBufferPos >= conversionBufferUsed) {
            const int64_t endIdx = convKnown ? convStartSample + (int64_t)(conversionBufferUsed / ch) : INT64_MIN;
            if (!refillConversionBuffer()) {
                decoderAtHTruth = false;
                if (errorState) {
                    feedSrc = Feed::DRAIN;
                    enterPastEnd(true);
                    return false;
                }
                // E24, the first EOF
                if (lDec < 0) {
                    lDec = endIdx;
                    ev(EvType::FIRST_EOF, lDec);
                }
                if (!lpChecked && pS > 0) {
                    lpChecked = true;
                    if (std::llabs(pS - lDec) > (int64_t)fileSampleRate / 1000) {
                        ev(EvType::LP_MISMATCH, pS, lDec);   // ERROR once per file; pad / cut
                    }
                }
                cur = lDec;
                if (periodSamples() > lDec) {
                    feedSrc = Feed::PAD;
                } else {
                    onE12orE13();
                }
                return false;
            }
        }
        const int64_t nx = nextSampleIndex();
        avail = (conversionBufferUsed - conversionBufferPos) / ch;
        if (pS > 0 && nx != INT64_MIN) {
            avail = (size_t)std::min<int64_t>((int64_t)avail, pS - nx);
        }
        ptr = conversionBuffer + conversionBufferPos;
        return true;
    }
    case Feed::PAD: {
        if (cur < p) {
            ptr = zeros.data();
            avail = (size_t)std::min<int64_t>(p - cur, (int64_t)MAX_CHUNK);
            return true;
        }
        onE12orE13();
        return false;
    }
    case Feed::DRAIN:
    case Feed::ENDED:
        return false;
    }
    return false;
}

void AudioFstream::consumeInput(size_t n)
{
    if (feedSrc == Feed::DECODER) {
        conversionBufferPos += n * fileChannels;
        decoderAtHTruth = false;
        decAtHFlag = false;
    } else {
        cur += (int64_t)n;
    }
}

size_t AudioFstream::process(float* out, size_t frames, double ratio, const LiveCtx& ctx)
{
    const unsigned ch = fileChannels;
    size_t done = 0;
    lastProduced = 0;
    if (!feedReady) {
        memset(out, 0, frames * ch * sizeof(float));
        return frames;
    }
    publishTarget(ctx.target);

    // the SEEK retry, 1 s after the first failure
    if (psState == Ps::SEEKING && retryFramesLeft > 0) {
        retryFramesLeft -= (int64_t)frames;
        if (retryFramesLeft <= 0) {
            retryFramesLeft = 0;
            const int64_t p = periodForStart();
            const int64_t t = ctx.target;
            post(JobKind::SEEK, (looping() && p < INF_SAMPLES && t >= 0) ? t % p : std::max<int64_t>(0, t));
        }
    }
    // the bounded discard after a pickup: at most 8 codec frames (or 4 periods) per period
    if (psState == Ps::SEEKING && chasing && outstanding == 0) {
        AVCodecParameters* par = fileReader.getFormatContext()->streams[audioStreamIndex]->codecpar;
        const int64_t bound = std::max<int64_t>(8 * (int64_t)std::max(par->frame_size, 0),
                                                (int64_t)(4 * frames * rNom));
        const int64_t p = periodForStart();
        const int64_t t = ctx.target;
        if (t < 0) {
            start(t, StartKind::OFFSET);
        } else {
            const int64_t tp = (looping() && p < INF_SAMPLES) ? t % p : t;
            const int64_t nx = nextSampleIndex();
            if (nx == INT64_MIN || tp < chaseLanding) {
                psState = Ps::HOLD;
                start(t, StartKind::OFFSET);
            } else {
                const int64_t to = std::min(tp, nx + bound);
                const bool ok = discardTo(to, false, 0);
                if (ok && to == tp) {
                    chasing = false;
                    const bool late = lateAtFrames >= 0;
                    beginOutput(t, tp, Feed::DECODER, StartKind::OFFSET, false);
                    if (late) {
                        ev(EvType::HANDOVER_LATE, (framesOut - lateAtFrames) * 1000 / std::max(1u, outRateFeed), 1);
                        lateAtFrames = -1;
                    }
                } else if (!ok) {
                    // EOF inside the chase: the target is past the decoded length
                    chasing = false;
                    if (lDec < 0 && convKnown) {
                        lDec = convStartSample + (int64_t)(conversionBufferUsed / ch);
                    }
                    if (lDec >= 0 && p > lDec && tp >= lDec) {
                        beginOutput(t, tp, Feed::PAD, StartKind::OFFSET, false);
                    } else {
                        enterPastEnd(false);
                        feedSrc = Feed::ENDED;
                        post(JobKind::RESET);
                    }
                }
            }
        }
    }

    const bool playedThisCall = (psState == Ps::PLAY && outPlay);
    if (playedThisCall) {
        // the working ratio, every period (G12: also restores it after a clear)
        soxr_set_io_ratio(vrs, ratio, frames);
        lastRatio = ratio;
        owed += ratio * (double)frames;
        sumRN += ratio * (double)frames;
        for (int guard = 0; done < frames && guard < 64 && psState == Ps::PLAY && outPlay; guard++) {
            const float* in = nullptr;
            size_t avail = 0;
            if (!feedInput(in, avail, ctx)) {
                continue;
            }
            size_t w = (owed - consumed >= 1.0) ? (size_t)(owed - consumed) : 0;
            if (guard > 0) {
                w = std::max(w, (size_t)std::ceil((frames - done) * ratio) + 1);
            }
            const size_t n = std::min(std::min(avail, w), MAX_CHUNK);
            size_t idone = 0, odone = 0;
            const soxr_error_t e = soxr_process(vrs, in, n, &idone, out + done * ch, frames - done, &odone);
            if (e) {
                ev(EvType::INVARIANT, 100);
                errorState = true;
                feedSrc = Feed::ENDED;
                enterPastEnd(false);
                break;
            }
            if (inputTap && idone > 0) {
                inputTap(inputTapUser, in, idone, feedPass, cursor());
            }
            consumeInput(idone);
            consumed += (double)idone;
            sumIdone += (double)idone;
            done += odone;
        }
    }
    // the drain after a true end (E13): the output still plays soxr's tail. soxr's NULL flush
    // is not used: in VR mode it stops short of the content (at r ~ 1 the last ~8 samples never
    // came out, T7-1). Silence is fed instead, the ratio and pos() keep running, and E21 fires
    // where the output's content position has passed the end plus a short ring-out.
    if (psState == Ps::PASTEND && feedSrc == Feed::DRAIN) {
        if (!playedThisCall) {
            soxr_set_io_ratio(vrs, ratio, frames);
            lastRatio = ratio;
            sumRN += ratio * (double)frames;
        }
        const double p0 = pos() - ratio * (double)frames;   // content at this period's first frame
        const double stopAt = drainEndContent + DRAIN_TAIL;
        const double kStopD = std::ceil((stopAt - p0) / std::max(ratio, 1e-9));
        const size_t kStop = kStopD <= (double)done ? done : (size_t)std::min(kStopD, (double)frames);
        for (int guard = 0; done < kStop && guard < 64; guard++) {
            size_t idone = 0, odone = 0;
            soxr_process(vrs, zeros.data(), MAX_CHUNK, &idone, out + done * ch, kStop - done, &odone);
            if (fadeOutLeft > 0) {
                for (size_t k = 0; k < odone; k++) {
                    const float g = fadeOutLeft > 0 ? (float)fadeOutLeft / (float)fadeOutLen : 0.0f;
                    for (unsigned c = 0; c < ch; c++) out[(done + k) * ch + c] *= g;
                    if (fadeOutLeft > 0) fadeOutLeft--;
                }
            } else if (wrapFailed) {
                memset(out + done * ch, 0, odone * ch * sizeof(float));
            }
            done += odone;
            if (odone == 0 && idone == 0) break;
        }
        if (kStopD <= (double)frames) {
            // E21
            feedSrc = Feed::ENDED;
            outPlay = false;
            wrapFailed = false;
            ev(EvType::DRAINED);
            post(JobKind::RESET);
        }
    }
    lastProduced = done;
    // 2 ms fade-in after a start (none for the clean reveal)
    if (fadeLeft > 0) {
        for (size_t k = 0; k < done && fadeLeft > 0; k++, fadeLeft--) {
            const float g = (float)(fadeLen - fadeLeft) / (float)fadeLen;
            for (unsigned c = 0; c < ch; c++) out[k * ch + c] *= g;
        }
    }
    if (done < frames) {
        memset(out + done * ch, 0, (frames - done) * ch * sizeof(float));
    }
    framesOut += (int64_t)frames;
    // passIndex = the output's wrap count (E12's output crossing)
    const int64_t p = periodForStart();
    if (psState == Ps::PLAY && looping() && p < INF_SAMPLES) {
        while (pos() >= (double)(passIdx + 1) * (double)p) {
            passIdx++;
            wrapCountPending++;
            ev(EvType::PASS_CROSSED, passIdx);
        }
    }
    checkInvariants();
    return frames;
}

void AudioFstream::checkInvariants()
{
    if (outPlay && !(psState == Ps::PLAY || (psState == Ps::PASTEND && feedSrc == Feed::DRAIN))) violate(3);
    if (feedSrc == Feed::DRAIN && psState != Ps::PASTEND) violate(4);
    if (psState == Ps::PLAY && feedSrc == Feed::DECODER && outstanding > 0) violate(1);
    if (psState == Ps::PLAY && outstandingKind[2] > 0 && feedSrc != Feed::CACHE) violate(6);
    if (mode == LoopMode::OFF && wrapsLeft() != 0) violate(7);
    if (originAbs < 0 || passIdx < 0) violate(8);
    if (outstanding < 0 || outstanding > 2) violate(9);
}
