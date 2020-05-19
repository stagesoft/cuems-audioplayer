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
// Stage Lab SysQ audio file stream class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
#ifndef AUDIOFSTREAM_CLASS_H
#define AUDIOFSTREAM_CLASS_H

#include <iostream>
#include <fstream>
#include <iomanip>

using namespace std;

class AudioFstream : public ifstream
{
    public:
        AudioFstream(   const string filename = "", 
                        ios_base::openmode openmode = ios_base::in | ios_base::binary );
        inline ~AudioFstream() { };

        struct headerData {
            char ChunkID[5] = "    ";
            unsigned long int ChunkSize = 0;
            char Format[5] = "    ";
            char SubChunk1ID[5] = "    ";
            unsigned long int SubChunk1Size = 0;
            unsigned int AudioFormat = 0;
            unsigned int NumChannels = 0;
            unsigned long int SampleRate = 0;
            unsigned long int ByteRate = 0;
            unsigned int BlockAlign = 0;
            unsigned int BitsPerSample = 0;
            
            char SubChunk2ID[5] = "    ";
            unsigned long int SubChunk2Size = 0;
        } headerData;

        bool checkHeader( void );
        bool loadFile( const string path );

        unsigned int headerSize = 0;

};

#endif // AUDIOFSTREAM_CLASS_H