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
#include <soxr.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
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
        
        // File information accessors (for compatibility with audioplayer.cpp)
        unsigned long long getFileSize() const;
        unsigned int getChannels() const;
        unsigned int getSampleRate() const;
        unsigned int getBitsPerSample() const;

    private:
        // FFmpeg members
        AVFormatContext* formatContext;
        AVCodecContext* codecContext;
        AVPacket* packet;
        AVFrame* frame;
        SwrContext* swrContext;
        int audioStreamIndex;
        
        // File state
        bool fileOpen;
        bool eofReached;
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
        string getFFmpegError(int errnum);  // Translate FFmpeg error codes
};

#endif // AUDIOFSTREAM_H
