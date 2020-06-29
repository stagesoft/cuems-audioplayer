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
#include <fstream>
#include <vector>
#include <iomanip>
#include "./cuemslogger/cuemslogger.h"
#include "cuems_errors.h"

using namespace std;

class AudioFstream : public ifstream
{
    typedef struct {
        char id[5] = "    ";
        unsigned long int size = 0;
    } subChunk;

    public:
        AudioFstream(   const string filename = "", 
                        ios_base::openmode openmode = ios_base::in | ios_base::binary );
        inline ~AudioFstream() { };

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

        unsigned int headerSize = 0;

};

#endif // AUDIOFSTREAM_H