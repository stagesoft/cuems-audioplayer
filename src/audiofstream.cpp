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
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "audiofstream.h"
#include <cstring>
#include <algorithm>

////////////////////////////////////////////
// Initializing static class members

////////////////////////////////////////////
AudioFstream::AudioFstream( const string filename, 
                            ios_base::openmode openmode ) 
{
    fileHandle = AF_NULL_FILEHANDLE;
    fileOpen = false;
    lastReadFrames = 0;
    lastBytesRead = 0;
    sampleFormat = 0;
    sampleWidth = 2;  // Default to 16-bit
    fileSampleRate = 0;
    fileChannels = 0;
    eofReached = false;
    errorState = false;
    currentFramePos = 0;
    totalFrames = 0;
    
    // Initialize resampling members
    targetSampleRate = 0;
    resampler = nullptr;
    resamplingEnabled = false;
    qualitySpec = soxr_quality_spec(SOXR_HQ, 0);  // Default to high quality
    resampleInputBuffer = nullptr;
    resampleOutputBuffer = nullptr;
    resampleBufferSize = 0;

    // Initialize headerData
    headerSize = 0;
    memset(headerData.RIFFID, ' ', 5);
    memset(headerData.WAVEID, ' ', 5);
    headerData.RIFFSize = 0;
    headerData.fileSize = 0;
    headerData.SubChunk1Size = 0;
    headerData.AudioFormat = 0;
    headerData.NumChannels = 0;
    headerData.SampleRate = 0;
    headerData.ByteRate = 0;
    headerData.BlockAlign = 0;
    headerData.BitsPerSample = 0;
    headerData.dataSize = 0;

    if ( !filename.empty() ) {
        open(filename, openmode);
    }
}

//////////////////////////////////////////////////////////
AudioFstream::~AudioFstream (void)
{
    close();
}

//////////////////////////////////////////////////////////
void AudioFstream::open(const string path, ios_base::openmode mode)
{
    close();  // Close any existing file
    
    fileHandle = afOpenFile(path.c_str(), "r", AF_NULL_FILESETUP);
    
    if ( fileHandle == AF_NULL_FILEHANDLE ) {
        std::cerr << "Unable to find or open file: " << path << endl;
        CuemsLogger::getLogger()->logError("Couldn't open file : " + path);
        errorState = true;
        fileOpen = false;
        return;
    }
    
    CuemsLogger::getLogger()->logOK("File open OK! : " + path);
    fileOpen = true;
    errorState = false;
    eofReached = false;
    currentFramePos = 0;
    
    checkHeader();
}

//////////////////////////////////////////////////////////
bool AudioFstream::checkHeader( void ) {
    if ( !fileOpen || fileHandle == AF_NULL_FILEHANDLE ) {
        return false;
    }

    // Get file format information
    int fileFormat = afGetFileFormat(fileHandle, NULL);
    
    // Detect format type (WAV or AIF)
    if (fileFormat == AF_FILE_WAVE) {
        strncpy(headerData.RIFFID, "RIFF", 4);
        strncpy(headerData.WAVEID, "WAVE", 4);
    } else if (fileFormat == AF_FILE_AIFF || fileFormat == AF_FILE_AIFFC) {
        strncpy(headerData.RIFFID, "FORM", 4);
        strncpy(headerData.WAVEID, "AIFF", 4);
    } else {
        // Unknown format, use generic identifiers
        strncpy(headerData.RIFFID, "DATA", 4);
        strncpy(headerData.WAVEID, "FILE", 4);
    }
    
    // Get audio parameters
    fileChannels = afGetChannels(fileHandle, AF_DEFAULT_TRACK);
    fileSampleRate = (unsigned int)afGetRate(fileHandle, AF_DEFAULT_TRACK);
    totalFrames = afGetFrameCount(fileHandle, AF_DEFAULT_TRACK);
    
    // Get sample format
    int sampleFormatGet, sampleWidthGet;
    afGetSampleFormat(fileHandle, AF_DEFAULT_TRACK, &sampleFormatGet, &sampleWidthGet);
    sampleFormat = sampleFormatGet;
    sampleWidth = sampleWidthGet;
    
    // Set output format to signed 16-bit for compatibility
    afSetVirtualSampleFormat(fileHandle, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, 16);
    afSetVirtualChannels(fileHandle, AF_DEFAULT_TRACK, fileChannels);
    
    // Populate headerData
    headerData.NumChannels = fileChannels;
    headerData.SampleRate = fileSampleRate;
    headerData.BitsPerSample = 16;  // We convert everything to 16-bit
    headerData.BlockAlign = fileChannels * 2;  // 2 bytes per sample (16-bit)
    headerData.ByteRate = fileSampleRate * headerData.BlockAlign;
    headerData.AudioFormat = 1;  // PCM
    
    // Calculate file size in bytes (for 16-bit output)
    headerData.dataSize = totalFrames * headerData.BlockAlign;
    headerData.fileSize = headerData.dataSize + 44;  // Approximate header size
    headerData.RIFFSize = headerData.fileSize - 8;
    
    // Estimate header size (this is approximate since libaudiofile handles it)
    headerSize = 44;  // Standard WAV header size
    
    CuemsLogger::getLogger()->logOK("Audio file OK! Sample rate: " + std::to_string(fileSampleRate) + 
                                     " Hz, Channels: " + std::to_string(fileChannels) +
                                     ", Bits: " + std::to_string(sampleWidthGet * 8) +
                                     " (converting to 16-bit)");
    CuemsLogger::getLogger()->logOK("File size : " + std::to_string(headerData.fileSize));
    
    return true;
}

//////////////////////////////////////////////////////////
bool AudioFstream::loadFile( const string path ) {
    open( path, ios_base::binary | ios_base::in );
    
    if ( !fileOpen ) {
        return false;
    }
    
    return checkHeader();
}

//////////////////////////////////////////////////////////
void AudioFstream::read(char* buffer, size_t bytes)
{
    lastBytesRead = 0;
    lastReadFrames = 0;
    
    if (!fileOpen || fileHandle == AF_NULL_FILEHANDLE || eofReached) {
        return;
    }
    
    // Calculate how many frames we need to read
    size_t bytesPerFrame = fileChannels * 2;  // 2 bytes per sample (16-bit)
    size_t framesToRead = bytes / bytesPerFrame;
    
    if (framesToRead == 0) {
        return;
    }
    
    if (!resamplingEnabled || targetSampleRate == 0 || targetSampleRate == fileSampleRate) {
        // Direct reading without resampling
        AFframecount framesRead = afReadFrames(fileHandle, AF_DEFAULT_TRACK, buffer, framesToRead);
        lastReadFrames = framesRead;
        lastBytesRead = framesRead * bytesPerFrame;
        currentFramePos += framesRead;
        
        if (framesRead < (AFframecount)framesToRead) {
            eofReached = true;
        }
    } else {
        // Reading with resampling
        // Calculate input frames needed based on sample rate ratio
        double ratio = (double)targetSampleRate / (double)fileSampleRate;
        size_t inputFramesNeeded = (size_t)(framesToRead / ratio) + 1;
        
        // Allocate or resize buffers if needed
        size_t neededBufferSize = std::max(inputFramesNeeded, framesToRead) * fileChannels;
        if (resampleBufferSize < neededBufferSize) {
            delete[] resampleInputBuffer;
            delete[] resampleOutputBuffer;
            resampleBufferSize = neededBufferSize * 2;  // Allocate extra
            resampleInputBuffer = new float[resampleBufferSize];
            resampleOutputBuffer = new float[resampleBufferSize];
        }
        
        // Read frames from file and convert to float
        int16_t* tempBuffer = new int16_t[inputFramesNeeded * fileChannels];
        AFframecount framesRead = afReadFrames(fileHandle, AF_DEFAULT_TRACK, tempBuffer, inputFramesNeeded);
        currentFramePos += framesRead;
        
        if (framesRead == 0) {
            delete[] tempBuffer;
            eofReached = true;
            return;
        }
        
        // Convert int16 to float [-1.0, 1.0]
        for (size_t i = 0; i < framesRead * fileChannels; i++) {
            resampleInputBuffer[i] = tempBuffer[i] / 32768.0f;
        }
        delete[] tempBuffer;
        
        // Perform resampling
        size_t inputFramesDone = 0;
        size_t outputFramesDone = 0;
        
        soxr_error_t error = soxr_process(resampler, 
                                          resampleInputBuffer, framesRead, &inputFramesDone,
                                          resampleOutputBuffer, framesToRead, &outputFramesDone);
        
        if (error) {
            CuemsLogger::getLogger()->logError("Resampling error: " + string(error));
            errorState = true;
            delete[] tempBuffer;
            return;
        }
        
        // Convert float back to int16
        int16_t* outputBuffer = (int16_t*)buffer;
        for (size_t i = 0; i < outputFramesDone * fileChannels; i++) {
            float sample = resampleOutputBuffer[i] * 32768.0f;
            sample = std::max(-32768.0f, std::min(32767.0f, sample));  // Clamp
            outputBuffer[i] = (int16_t)sample;
        }
        
        lastReadFrames = outputFramesDone;
        lastBytesRead = outputFramesDone * bytesPerFrame;
        
        if (framesRead < (AFframecount)inputFramesNeeded) {
            eofReached = true;
        }
    }
}

//////////////////////////////////////////////////////////
void AudioFstream::seekg(long long pos, ios_base::seekdir dir)
{
    if (!fileOpen || fileHandle == AF_NULL_FILEHANDLE) {
        return;
    }
    
    AFframecount targetFrame = 0;
    size_t bytesPerFrame = fileChannels * 2;  // 2 bytes per sample (16-bit)
    
    if (dir == ios_base::beg) {
        // Convert byte position to frame position
        targetFrame = pos / bytesPerFrame;
    } else if (dir == ios_base::cur) {
        targetFrame = currentFramePos + (pos / bytesPerFrame);
    } else if (dir == ios_base::end) {
        targetFrame = totalFrames + (pos / bytesPerFrame);
    }
    
    // Clamp to valid range
    if (targetFrame < 0) targetFrame = 0;
    if (targetFrame > totalFrames) targetFrame = totalFrames;
    
    AFframecount result = afSeekFrame(fileHandle, AF_DEFAULT_TRACK, targetFrame);
    if (result >= 0) {
        currentFramePos = result;
        eofReached = false;
        
        // Reset resampler state if resampling is enabled
        if (resamplingEnabled && resampler) {
            // Flush resampler
            soxr_process(resampler, NULL, 0, NULL, NULL, 0, NULL);
        }
    } else {
        CuemsLogger::getLogger()->logError("Seek failed");
        errorState = true;
    }
}

//////////////////////////////////////////////////////////
streamsize AudioFstream::gcount() const
{
    return lastBytesRead;
}

//////////////////////////////////////////////////////////
bool AudioFstream::eof() const
{
    return eofReached;
}

//////////////////////////////////////////////////////////
bool AudioFstream::good() const
{
    return fileOpen && !errorState && !eofReached;
}

//////////////////////////////////////////////////////////
bool AudioFstream::bad() const
{
    return errorState;
}

//////////////////////////////////////////////////////////
void AudioFstream::clear()
{
    eofReached = false;
    errorState = false;
}

//////////////////////////////////////////////////////////
void AudioFstream::close()
{
    if (fileOpen && fileHandle != AF_NULL_FILEHANDLE) {
        afCloseFile(fileHandle);
        fileHandle = AF_NULL_FILEHANDLE;
        fileOpen = false;
    }
    
    cleanupResampler();
    
    eofReached = false;
    errorState = false;
    currentFramePos = 0;
}

//////////////////////////////////////////////////////////
void AudioFstream::setTargetSampleRate(unsigned int rate)
{
    targetSampleRate = rate;
    
    if (fileOpen && fileSampleRate != 0) {
        if (targetSampleRate != fileSampleRate) {
            CuemsLogger::getLogger()->logInfo("Sample rate conversion needed: " + 
                                               std::to_string(fileSampleRate) + " Hz -> " + 
                                               std::to_string(targetSampleRate) + " Hz");
            initializeResampler();
        } else {
            CuemsLogger::getLogger()->logInfo("Sample rates match: " + 
                                               std::to_string(fileSampleRate) + " Hz, no resampling needed");
            resamplingEnabled = false;
            cleanupResampler();
        }
    }
}

//////////////////////////////////////////////////////////
void AudioFstream::setResampleQuality(const string& quality)
{
    qualitySpec = parseQualityString(quality);
    
    // If resampler already exists, recreate it with new quality
    if (resamplingEnabled && resampler) {
        cleanupResampler();
        initializeResampler();
    }
}

//////////////////////////////////////////////////////////
soxr_quality_spec_t AudioFstream::parseQualityString(const string& quality)
{
    if (quality == "vhq") {
        CuemsLogger::getLogger()->logInfo("Resampling quality: Very High (VHQ)");
        return soxr_quality_spec(SOXR_VHQ, 0);
    } else if (quality == "hq") {
        CuemsLogger::getLogger()->logInfo("Resampling quality: High (HQ)");
        return soxr_quality_spec(SOXR_HQ, 0);
    } else if (quality == "mq") {
        CuemsLogger::getLogger()->logInfo("Resampling quality: Medium (MQ)");
        return soxr_quality_spec(SOXR_MQ, 0);
    } else if (quality == "lq") {
        CuemsLogger::getLogger()->logInfo("Resampling quality: Low (LQ)");
        return soxr_quality_spec(SOXR_LQ, 0);
    } else {
        CuemsLogger::getLogger()->logWarning("Unknown quality '" + quality + "', defaulting to High (HQ)");
        return soxr_quality_spec(SOXR_HQ, 0);
    }
}

//////////////////////////////////////////////////////////
void AudioFstream::initializeResampler()
{
    if (!fileOpen || targetSampleRate == 0 || fileSampleRate == 0) {
        return;
    }
    
    if (targetSampleRate == fileSampleRate) {
        resamplingEnabled = false;
        return;
    }
    
    cleanupResampler();
    
    soxr_error_t error;
    soxr_io_spec_t ioSpec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    
    resampler = soxr_create((double)fileSampleRate, (double)targetSampleRate, 
                           fileChannels, &error, &ioSpec, &qualitySpec, NULL);
    
    if (error || !resampler) {
        CuemsLogger::getLogger()->logError("Failed to create resampler: " + string(error ? error : "unknown error"));
        errorState = true;
        resamplingEnabled = false;
        return;
    }
    
    resamplingEnabled = true;
    CuemsLogger::getLogger()->logOK("Resampler initialized successfully");
}

//////////////////////////////////////////////////////////
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
