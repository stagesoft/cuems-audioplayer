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
    fromFirstPacket = false;
    discardSet = false;

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
        std::cerr << "Unable to find or open file: " << path << endl;
        CuemsLogger::getLogger()->logError("Couldn't open file: " + path);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Find audio stream
    audioStreamIndex = fileReader.findStream(AVMEDIA_TYPE_AUDIO);
    if (audioStreamIndex < 0) {
        std::cerr << "Could not find audio stream in file" << endl;
        CuemsLogger::getLogger()->logError("No audio stream found in file");
        fileReader.close();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Get codec parameters
    AVCodecParameters* codecParams = fileReader.getCodecParameters(audioStreamIndex);
    if (!codecParams) {
        std::cerr << "Failed to get codec parameters" << endl;
        CuemsLogger::getLogger()->logError("Failed to get codec parameters");
        fileReader.close();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Open audio decoder
    if (!audioDecoder.openCodec(codecParams)) {
        std::cerr << "Failed to open audio codec" << endl;
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
        std::cerr << "Failed to allocate packet/frame" << endl;
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
        std::cerr << "Failed to get audio properties" << endl;
        CuemsLogger::getLogger()->logError("Failed to get audio properties");
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Log audio stream info for debugging
    std::cerr << "Audio stream index: " << audioStreamIndex << endl;
    const char* codec_name = avcodec_get_name(codecParams->codec_id);
    std::cerr << "Codec: " << (codec_name ? codec_name : "unknown") << endl;
    std::cerr << "Channels: " << channels << ", Sample rate: " << sampleRate << " Hz" << endl;
    std::cerr << "Sample format: " << av_get_sample_fmt_name(sampleFmt) << endl;
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
        std::cerr << kind << " from " << fileChannels << " to " << targetChannels << " channels" << endl;
        CuemsLogger::getLogger()->logInfo(std::string(kind) + " audio: " + std::to_string(fileChannels) +
                                          " -> " + std::to_string(targetChannels) + " channels");
    }
    
    swrContext = audioDecoder.createSwrContextExplicit(outputChannels, fileSampleRate, AV_SAMPLE_FMT_FLT);
    
    if (!swrContext) {
        std::cerr << "Failed to create swresample context" << endl;
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

    if (!firstPacketSeek(true)) {
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    
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
            // Debug: log frame info for first few frames
            static int frame_count = 0;
            if (frame_count < 3) {
                std::cerr << "Decoded frame " << frame_count << ": " << frame->nb_samples 
                          << " samples, format: " << av_get_sample_fmt_name((AVSampleFormat)frame->format) << endl;
                frame_count++;
            }
            return true;
        } else if (ret == AVERROR_EOF) {
            decoderDrained = true;
            return false;
        } else if (ret != AVERROR(EAGAIN)) {
            std::cerr << "Error receiving frame from decoder: " << getFFmpegError(ret) << endl;
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
            std::cerr << "Error reading frame: " << getFFmpegError(ret) << endl;
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
            std::cerr << "Error sending packet to decoder: " << getFFmpegError(ret) << endl;
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
            std::cerr << "Error converting audio samples: " << getFFmpegError(out_samples) << endl;
            errorState = true;
            return false;
        }
        
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
    
    if (!fileOpen || errorState) {
        return;
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
                    std::cerr << "SOXR drain error: " << soxr_strerror(soxr_err) << endl;
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
                    std::cerr << "SOXR error: " << soxr_strerror(soxr_err) << endl;
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
    if (!fileOpen || !fileReader.isReady()) {
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
        std::cerr << "Seek error" << endl;
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
        std::cerr << "Failed to create resampler: " << (error ? error : "unknown error") << endl;
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
