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

////////////////////////////////////////////
// Initializing static class members

////////////////////////////////////////////
AudioFstream::AudioFstream( const string filename, 
                            ios_base::openmode openmode ) 
                            : ifstream(filename, openmode)
{
    if ( bad() ) {
        std::cerr << "Unable to find or open file!" << endl;
        CuemsLogger::getLogger()->logError("Couldn't open file : " + filename);
    }
    else {
        CuemsLogger::getLogger()->logOK("File open OK! : " + filename);
    }

    checkHeader();
}

//////////////////////////////////////////////////////////
bool AudioFstream::checkHeader( void ) {

    // We need more integrity and file format checking in here
    // like checking the chunk id's, sizes and so...
    // By now we are just reading the header to take it apart and
    // to start manipulating it...

    if ( good() ) {

        unsigned int bytesRead = 0;
        
        read(headerData.RIFFID, 4);
        bytesRead += gcount();
        read((char*) &headerData.RIFFSize, 4);
        bytesRead += gcount();
        headerData.fileSize = headerData.RIFFSize + 8;
        read(headerData.WAVEID, 4);
        bytesRead += gcount();

        subChunk tmp;
        read(tmp.id, 4);
        bytesRead += gcount();
        bool dataFound = false;

        while ( !dataFound ) {
            if ( (string)(tmp.id) == "fmt " ) {
                read((char*) &tmp.size, 4);
                bytesRead += gcount();
                read((char*) &headerData.AudioFormat, 2);
                bytesRead += gcount();
                read((char*) &headerData.NumChannels, 2);
                bytesRead += gcount();
                read((char*) &headerData.SampleRate, 4);
                bytesRead += gcount();
                read((char*) &headerData.ByteRate, 4);
                bytesRead += gcount();
                read((char*) &headerData.BlockAlign, 2);
                bytesRead += gcount();
                read((char*) &headerData.BitsPerSample, 2);
                bytesRead += gcount();
            }
            else if ( (string)(tmp.id) == "data" ) {
                read((char*) &tmp.size, 4);
                bytesRead += gcount();
                headerData.dataSize = tmp.size;
                dataFound = true;
            }
            else {
                read((char*) &tmp.size, 4);
                bytesRead += gcount();
                seekg(tmp.size, ios_base::cur);
                bytesRead += tmp.size;
            }

            headerData.subHeaders.push_back(tmp);

            if ( !dataFound ) {
                tmp = {0};
                read(tmp.id, 4);
                bytesRead += gcount();
            }
        }

        headerSize = bytesRead;

        /*
        // Check the header (To do: better checking)
        std::cout << endl << "WAV Header reading results:" << endl;
        std::cout << "-> ChunkID \"" << headerData.ChunkID << "\"" << endl;
        std::cout << "-> ChunkSize " << headerData.ChunkSize << endl;
        std::cout << "-> Format \"" << headerData.Format << "\"" << endl;
        std::cout << "-> SubChunk1ID \"" << headerData.SubChunk1ID << "\"" << endl;
        std::cout << "-> SubChunk1Size " << headerData.SubChunk1Size << endl;
        std::cout << "-> AudioFormat " << headerData.AudioFormat << endl;
        std::cout << "-> NumChannels " << headerData.NumChannels << endl;
        std::cout << "-> SampleRate " << headerData.SampleRate << endl;
        std::cout << "-> ByteRate " << headerData.ByteRate << endl;
        std::cout << "-> BlockAlign " << headerData.BlockAlign << endl;
        std::cout << "-> BitsPerSample " << headerData.BitsPerSample << endl;
        std::cout << "-> SubChunk2ID " << headerData.SubChunk2ID << endl;
        std::cout << "-> SubChunk2Size " << headerData.SubChunk2Size << endl << endl;
        */

        CuemsLogger::getLogger()->logOK("WAV header OK! " + std::to_string(bytesRead) + " bytes read");
        CuemsLogger::getLogger()->logOK("File size : " + std::to_string(headerData.fileSize));

        if ( eof() && !dataFound ) {
            std::string str = "Wrong WAV file header!!! " + std::to_string(bytesRead) +
                " bytes read and no data field found!!!";
            CuemsLogger::getLogger()->logError(str);
        }
        else return true;
    }

    return false;
}

//////////////////////////////////////////////////////////
bool AudioFstream::loadFile( const string path ) {
    open( path, ios::binary | ios::in );

    if ( bad() ) {
        std::cerr << "Unable to find or open file!" << endl;
        CuemsLogger::getLogger()->logError("Couldn't open file : " + path);

        return false;
    }
    else {
        CuemsLogger::getLogger()->logOK("File open : " + path);
        return checkHeader();
    }
}

