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
    targetChannels = 0;  // 0 means use file's channel count (no downmixing)
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
    
    // Setup libswresample for format conversion to float (and optional channel downmixing)
    // Use AudioDecoder's createSwrContext which handles unknown channel layouts properly
    unsigned int outputChannels = (targetChannels > 0) ? targetChannels : fileChannels;
    
    if (targetChannels > 0 && targetChannels != fileChannels) {
        std::cerr << "Downmixing from " << fileChannels << " to " << targetChannels << " channels" << endl;
        CuemsLogger::getLogger()->logInfo("Downmixing audio: " + std::to_string(fileChannels) + 
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
    currentSamplePos = 0;
    
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
bool AudioFstream::decodeNextFrame()
{
    if (!fileOpen || eofReached) {
        return false;
    }
    
    while (true) {
        // Read packet from MediaFileReader
        int ret = fileReader.readPacket(packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                eofReached = true;
                // Flush decoder
                audioDecoder.sendPacket(nullptr);
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
            ret = audioDecoder.sendPacket(packet);
            av_packet_unref(packet);
            
            if (ret < 0) {
                std::cerr << "Error sending packet to decoder: " << getFFmpegError(ret) << endl;
                errorState = true;
                return false;
            }
        }
        
        // Receive decoded frame
        ret = audioDecoder.receiveFrame(frame);
        
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
        // Debug: log frame info for first few frames
        static int frame_count = 0;
        if (frame_count < 3) {
            std::cerr << "Decoded frame " << frame_count << ": " << frame->nb_samples 
                      << " samples, format: " << av_get_sample_fmt_name((AVSampleFormat)frame->format) << endl;
            frame_count++;
        }
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
                    // Account for libswresample's internal buffering delay
                    int64_t delay = swr_get_delay(swrContext, fileSampleRate);
                    
                    // Calculate max output samples: input samples + buffered samples
                    int64_t estimated_out_samples = av_rescale_rnd(delay + frame->nb_samples,
                                                                    fileSampleRate, fileSampleRate,
                                                                    AV_ROUND_UP);
                    int max_out_samples = std::min((int64_t)(conversionBufferSize / fileChannels),
                                                    estimated_out_samples);
                    
                    uint8_t* out = (uint8_t*)conversionBuffer;
                    int out_samples = swr_convert(swrContext, &out, max_out_samples,
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
                    // EOF - drain remaining samples from swresample
                    int64_t delay = swr_get_delay(swrContext, fileSampleRate);
                    if (delay > 0) {
                        int max_out_samples = conversionBufferSize / fileChannels;
                        uint8_t* out = (uint8_t*)conversionBuffer;
                        int out_samples = swr_convert(swrContext, &out, max_out_samples,
                                                     nullptr, 0);  // NULL input to drain
                        if (out_samples > 0) {
                            conversionBufferUsed = out_samples * fileChannels;
                            conversionBufferPos = 0;
                            continue;  // Process drained samples
                        }
                    }
                    break;  // EOF or error
                }
            }
            
            if (conversionBufferPos >= conversionBufferUsed) {
                // No more input data - if we're at EOF, drain the resampler
                if (eofReached && floatsResampled < resampledFloatsNeeded) {
                    // Drain libsoxr's internal buffer
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
                        break;  // Fully drained
                    }
                } else {
                    break;  // No more data available
                }
            } else {
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
                    // Account for libswresample's internal buffering delay
                    int64_t delay = swr_get_delay(swrContext, fileSampleRate);
                    
                    // Calculate max output samples: input samples + buffered samples
                    int64_t estimated_out_samples = av_rescale_rnd(delay + frame->nb_samples,
                                                                    fileSampleRate, fileSampleRate,
                                                                    AV_ROUND_UP);
                    int max_out_samples = std::min((int64_t)(conversionBufferSize / fileChannels),
                                                    estimated_out_samples);
                    
                    uint8_t* out = (uint8_t*)conversionBuffer;
                    int out_samples = swr_convert(swrContext, &out, max_out_samples,
                                                 (const uint8_t**)frame->data, frame->nb_samples);
                    
                    if (out_samples < 0) {
                        std::cerr << "Error converting audio samples: " << getFFmpegError(out_samples) << endl;
                        errorState = true;
                        av_frame_unref(frame);
                        break;
                    }
                    
                    // Debug: log conversion info for first few frames
                    static int convert_count_no_resample = 0;
                    if (convert_count_no_resample < 3) {
                        std::cerr << "Converted " << out_samples << " samples to float (no resample, frame " << convert_count_no_resample << ")" << endl;
                        convert_count_no_resample++;
                    }
                    
                    conversionBufferUsed = out_samples * fileChannels;
                    conversionBufferPos = 0;
                    av_frame_unref(frame);
                } else {
                    // EOF - drain remaining samples from swresample
                    int64_t delay = swr_get_delay(swrContext, fileSampleRate);
                    if (delay > 0) {
                        int max_out_samples = conversionBufferSize / fileChannels;
                        uint8_t* out = (uint8_t*)conversionBuffer;
                        int out_samples = swr_convert(swrContext, &out, max_out_samples,
                                                     nullptr, 0);  // NULL input to drain
                        if (out_samples > 0) {
                            conversionBufferUsed = out_samples * fileChannels;
                            conversionBufferPos = 0;
                            continue;  // Process drained samples
                        }
                    }
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
