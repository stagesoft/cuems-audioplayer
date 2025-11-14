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
#include <audiofile.h>
#include <soxr.h>
#include "cuemslogger.h"
#include "cuems_errors.h"

using namespace std;

class AudioFstream
{
    typedef struct {
        char id[5] = "    ";
        unsigned long int size = 0;
    } subChunk;

    public:
        AudioFstream(   const string filename = "", 
                        ios_base::openmode openmode = ios_base::in | ios_base::binary );
        ~AudioFstream();

        struct headerData {
            // "RIFF" & "WAVE" headers
            char RIFFID[5] = "    ";
            unsigned long int RIFFSize = 0;
            unsigned long int fileSize = 0;
            char WAVEID[5] = "    ";

            // "fmt " header data
            unsigned long int SubChunk1Size = 0;
            unsigned int AudioFormat = 0;
            unsigned int NumChannels = 0;
            unsigned long int SampleRate = 0;
            unsigned long int ByteRate = 0;
            unsigned int BlockAlign = 0;
            unsigned int BitsPerSample = 0;
            
            vector<subChunk> subHeaders;

            unsigned long int dataSize = 0;
        } headerData;

        bool checkHeader( void );
        bool loadFile( const string path );
        
        // Stream-like interface for compatibility
        void read(char* buffer, size_t bytes);
        void seekg(long long pos, ios_base::seekdir dir);
        streamsize gcount() const;
        bool eof() const;
        bool good() const;
        bool bad() const;
        void clear();
        void close();
        void open(const string path, ios_base::openmode mode);

        unsigned int headerSize = 0;

        // New methods for resampling
        void setTargetSampleRate(unsigned int rate);
        void setResampleQuality(const string& quality);

    private:
        // Libaudiofile members
        AFfilehandle fileHandle;
        bool fileOpen;
        AFframecount lastReadFrames;
        streamsize lastBytesRead;
        int sampleFormat;
        int sampleWidth;  // bytes per sample
        unsigned int fileSampleRate;
        unsigned int fileChannels;
        bool eofReached;
        bool errorState;
        AFframecount currentFramePos;
        AFframecount totalFrames;

        // Resampling members
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
        soxr_quality_spec_t parseQualityString(const string& quality);
};

#endif // AUDIOFSTREAM_H
