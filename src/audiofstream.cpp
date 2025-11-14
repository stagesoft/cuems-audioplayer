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

////////////////////////////////////////////
// Constructor
////////////////////////////////////////////
AudioFstream::AudioFstream( const string filename, 
                            ios_base::openmode openmode ) 
{
    // Initialize FFmpeg members
    formatContext = nullptr;
    codecContext = nullptr;
    packet = nullptr;
    frame = nullptr;
    swrContext = nullptr;
    audioStreamIndex = -1;
    
    // Initialize file state
    fileOpen = false;
    eofReached = false;
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
    resampler = nullptr;
    resamplingEnabled = false;
    qualitySpec = soxr_quality_spec(SOXR_HQ, 0);  // Default to high quality
    resampleInputBuffer = nullptr;
    resampleOutputBuffer = nullptr;
    resampleBufferSize = 0;

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
    
    // Open input file
    formatContext = nullptr;
    int ret = avformat_open_input(&formatContext, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "Unable to find or open file: " << path << endl;
        std::cerr << "FFmpeg error: " << getFFmpegError(ret) << endl;
        CuemsLogger::getLogger()->logError("Couldn't open file: " + path + " - " + getFFmpegError(ret));
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Retrieve stream information
    ret = avformat_find_stream_info(formatContext, nullptr);
    if (ret < 0) {
        std::cerr << "Could not find stream information: " << getFFmpegError(ret) << endl;
        CuemsLogger::getLogger()->logError("Could not find stream information: " + getFFmpegError(ret));
        avformat_close_input(&formatContext);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Find the first audio stream
    audioStreamIndex = -1;
    for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            break;
        }
    }
    
    if (audioStreamIndex == -1) {
        std::cerr << "Could not find audio stream in file" << endl;
        CuemsLogger::getLogger()->logError("No audio stream found in file");
        avformat_close_input(&formatContext);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Get codec parameters
    AVCodecParameters* codecParams = formatContext->streams[audioStreamIndex]->codecpar;
    
    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec!" << endl;
        CuemsLogger::getLogger()->logError("Unsupported codec ID: " + std::to_string(codecParams->codec_id));
        avformat_close_input(&formatContext);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Allocate codec context
    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        std::cerr << "Failed to allocate codec context" << endl;
        CuemsLogger::getLogger()->logError("Failed to allocate codec context");
        avformat_close_input(&formatContext);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Copy codec parameters to codec context
    ret = avcodec_parameters_to_context(codecContext, codecParams);
    if (ret < 0) {
        std::cerr << "Failed to copy codec parameters: " << getFFmpegError(ret) << endl;
        CuemsLogger::getLogger()->logError("Failed to copy codec parameters: " + getFFmpegError(ret));
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Open codec
    ret = avcodec_open2(codecContext, codec, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to open codec: " << getFFmpegError(ret) << endl;
        CuemsLogger::getLogger()->logError("Failed to open codec: " + getFFmpegError(ret));
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
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
    
    // Extract audio properties
    fileChannels = codecContext->ch_layout.nb_channels;
    fileSampleRate = codecContext->sample_rate;
    
    // Calculate total samples from duration
    AVStream* audioStream = formatContext->streams[audioStreamIndex];
    if (audioStream->duration != AV_NOPTS_VALUE) {
        totalSamples = av_rescale_q(audioStream->duration, audioStream->time_base, 
                                    (AVRational){1, (int)fileSampleRate});
    } else if (formatContext->duration != AV_NOPTS_VALUE) {
        totalSamples = (int64_t)((double)formatContext->duration / AV_TIME_BASE * fileSampleRate);
    } else {
        // Unknown duration, estimate from file size (will be inaccurate for compressed formats)
        totalSamples = 0;  // Will be updated as we decode
    }
    
    // File size in bytes (32-bit float output for JACK)
    fileSize = totalSamples * fileChannels * 4;  // 4 bytes per sample (32-bit float)
    
    // Setup libswresample for format conversion to float
    // (FFmpeg decodes to various formats, we need float for libsoxr)
    AVChannelLayout out_ch_layout = codecContext->ch_layout;
    
    ret = swr_alloc_set_opts2(&swrContext,
                              &out_ch_layout, AV_SAMPLE_FMT_FLT, fileSampleRate,
                              &codecContext->ch_layout, codecContext->sample_fmt, codecContext->sample_rate,
                              0, nullptr);
    
    if (ret < 0 || !swrContext) {
        std::cerr << "Failed to create resampler context: " << getFFmpegError(ret) << endl;
        CuemsLogger::getLogger()->logError("Failed to create resampler context: " + getFFmpegError(ret));
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    ret = swr_init(swrContext);
    if (ret < 0) {
        std::cerr << "Failed to initialize resampler: " << getFFmpegError(ret) << endl;
        CuemsLogger::getLogger()->logError("Failed to initialize resampler: " + getFFmpegError(ret));
        cleanupFFmpeg();
        errorState = true;
        fileOpen = false;
        return;
    }
    
    // Allocate conversion buffer (decode → float)
    conversionBufferSize = 8192 * fileChannels;  // 8192 frames worth
    conversionBuffer = new float[conversionBufferSize];
    conversionBufferUsed = 0;
    conversionBufferPos = 0;
    
    fileOpen = true;
    errorState = false;
    eofReached = false;
    currentSamplePos = 0;
    
    CuemsLogger::getLogger()->logOK("File open OK! : " + path);
    CuemsLogger::getLogger()->logOK("Audio codec: " + string(codec->long_name));
    CuemsLogger::getLogger()->logOK("Sample rate: " + std::to_string(fileSampleRate) + " Hz");
    CuemsLogger::getLogger()->logOK("Channels: " + std::to_string(fileChannels));
    CuemsLogger::getLogger()->logOK("Format: " + string(av_get_sample_fmt_name(codecContext->sample_fmt)));
    if (totalSamples > 0) {
        CuemsLogger::getLogger()->logOK("Duration: " + std::to_string(totalSamples / (double)fileSampleRate) + " seconds");
    }
    
    // Initialize resampler if target sample rate is already set
    if (targetSampleRate > 0 && targetSampleRate != fileSampleRate) {
        initializeResampler();
    }
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
bool AudioFstream::decodeNextFrame()
{
    if (!fileOpen || eofReached) {
        return false;
    }
    
    while (true) {
        // Read packet
        int ret = av_read_frame(formatContext, packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                eofReached = true;
                // Flush decoder
                avcodec_send_packet(codecContext, nullptr);
            } else {
                std::cerr << "Error reading frame: " << getFFmpegError(ret) << endl;
                errorState = true;
                return false;
            }
        }
        
        // Skip non-audio packets
        if (ret >= 0 && packet->stream_index != audioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        
        // Send packet to decoder
        if (ret >= 0) {
            ret = avcodec_send_packet(codecContext, packet);
            av_packet_unref(packet);
            
            if (ret < 0) {
                std::cerr << "Error sending packet to decoder: " << getFFmpegError(ret) << endl;
                errorState = true;
                return false;
            }
        }
        
        // Receive decoded frame
        ret = avcodec_receive_frame(codecContext, frame);
        
        if (ret == AVERROR(EAGAIN)) {
            // Need more packets
            if (eofReached) {
                return false;  // No more data
            }
            continue;
        } else if (ret == AVERROR_EOF) {
            eofReached = true;
            return false;
        } else if (ret < 0) {
            std::cerr << "Error receiving frame from decoder: " << getFFmpegError(ret) << endl;
            errorState = true;
            return false;
        }
        
        // Successfully decoded a frame
        return true;
    }
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
        
        while (floatsResampled < resampledFloatsNeeded && !eofReached) {
            // First, ensure we have decoded float data in conversionBuffer
            while (conversionBufferPos >= conversionBufferUsed && !eofReached) {
                // Need to decode more data
                if (decodeNextFrame()) {
                    // Convert decoded frame to float
                    uint8_t* out = (uint8_t*)conversionBuffer;
                    int out_samples = swr_convert(swrContext, &out, frame->nb_samples,
                                                 (const uint8_t**)frame->data, frame->nb_samples);
                    
                    if (out_samples < 0) {
                        std::cerr << "Error converting audio samples" << endl;
                        errorState = true;
                        av_frame_unref(frame);
                        break;
                    }
                    
                    conversionBufferUsed = out_samples * fileChannels;
                    conversionBufferPos = 0;
                    av_frame_unref(frame);
                } else {
                    break;  // EOF or error
                }
            }
            
            if (conversionBufferPos >= conversionBufferUsed) {
                break;  // No more data available
            }
            
            // Feed available float data to libsoxr
            size_t floatsAvailable = conversionBufferUsed - conversionBufferPos;
            size_t floatsToResample = std::min(floatsAvailable, resampledFloatsNeeded - floatsResampled);
            size_t framesAvailable = floatsAvailable / fileChannels;
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
        
        // Copy resampled floats directly to output (JACK native format)
        size_t floatsToCopy = std::min(floatsResampled, samplesNeeded);
        for (size_t i = 0; i < floatsToCopy; i++) {
            outputPtr[i] = tempFloatBuffer[i];
            lastBytesRead += 4;  // 4 bytes per float
        }
        
        // Update current position based on how many samples we output
        currentSamplePos += floatsToCopy;
        
        delete[] tempFloatBuffer;
        
    } else {
        // No resampling - direct decode and output as float
        while (bytesRemaining > 0 && !eofReached) {
            // Ensure we have decoded float data
            while (conversionBufferPos >= conversionBufferUsed && !eofReached) {
                if (decodeNextFrame()) {
                    // Convert decoded frame to float
                    uint8_t* out = (uint8_t*)conversionBuffer;
                    int out_samples = swr_convert(swrContext, &out, frame->nb_samples,
                                                 (const uint8_t**)frame->data, frame->nb_samples);
                    
                    if (out_samples < 0) {
                        std::cerr << "Error converting audio samples" << endl;
                        errorState = true;
                        av_frame_unref(frame);
                        break;
                    }
                    
                    conversionBufferUsed = out_samples * fileChannels;
                    conversionBufferPos = 0;
                    av_frame_unref(frame);
                } else {
                    break;
                }
            }
            
            if (conversionBufferPos >= conversionBufferUsed) {
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
    if (!fileOpen || !formatContext) {
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
    
    // Convert to timestamp
    int64_t targetTimestamp = av_rescale_q(fileSamplePos / fileChannels, 
                                          (AVRational){1, (int)fileSampleRate},
                                          formatContext->streams[audioStreamIndex]->time_base);
    
    // Seek
    int ret = av_seek_frame(formatContext, audioStreamIndex, targetTimestamp, AVSEEK_FLAG_BACKWARD);
    
    if (ret < 0) {
        std::cerr << "Seek error: " << getFFmpegError(ret) << endl;
        CuemsLogger::getLogger()->logError("Seek error: " + getFFmpegError(ret));
        errorState = true;
        return;
    }
    
    // Flush codec buffers
    avcodec_flush_buffers(codecContext);
    
    // Reset conversion buffer
    conversionBufferUsed = 0;
    conversionBufferPos = 0;
    
    // Clear EOF flag
    eofReached = false;
    
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
    if (codecContext) {
        avcodec_free_context(&codecContext);
    }
    if (formatContext) {
        avformat_close_input(&formatContext);
    }
    if (swrContext) {
        swr_free(&swrContext);
    }
    if (conversionBuffer) {
        delete[] conversionBuffer;
        conversionBuffer = nullptr;
    }
    
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
    
    // No resampling - return original file size
    return fileSize;
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
    resampleBufferSize = 8192 * fileChannels;  // 8192 frames worth
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
