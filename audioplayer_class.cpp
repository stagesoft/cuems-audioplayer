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
// Stage Lab SysQ audio player class code file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "audioplayer_class.h"

////////////////////////////////////////////
// Initializing static class members

double AudioPlayer::playHead = 0;
bool AudioPlayer::followingMtc = true;
bool AudioPlayer::isStreamRunning = false;
bool AudioPlayer::endOfStream = false;

//////////////////////////////////////////////////////////
AudioPlayer::AudioPlayer(   int port, 
                            double initOffset,
                            double finalWait,
                            const string oscRoute ,
                            const string filePath , 
                            const string uuid,
                            const bool stopOnLostFlag,
                            unsigned int numberOfChannels, 
                            unsigned int sRate, 
                            unsigned int device,
                            RtAudio::Api audioApi )
                            :   // Members initialization
                            OscReceiver(port, oscRoute.c_str()),
                            audioPath(filePath),
                            nChannels(numberOfChannels),
                            sampleRate(sRate),
                            deviceID(device),
                            audio(audioApi),
                            audioFile(filePath.c_str()),
                            headOffset(initOffset),
                            endWaitTime(finalWait),
                            playerUuid(uuid),
                            stopOnMTCLost(stopOnLostFlag)
 {
    //////////////////////////////////////////////////////////
    // Config tasks to be implemented later maybe
    // loadNodeConfig();
	// loadMediaConfig();

    //////////////////////////////////////////////////////////
    // Set up working class members



    // Per channel volume param to process audio
    volumeMaster = new float[nChannels];
    for ( unsigned short int i = 0; i < nChannels; i++ ) {
        volumeMaster[i] = 1.0;
    }

    // File header read so initial offset
    headOffset = audioFile.headerSize;

    // Audio frame size calc
    audioFrameSize = nChannels * headStep;
    audioSecondSize = sampleRate * audioFrameSize;

    intermediate = new short int[nChannels];


    //////////////////////////////////////////////////////////
    // Setting our audio stream parameters
    // Check for audio devices
    try {
        if ( audio.getDeviceCount() == 0 ) {
            std::cout << "No audio devices found on API:" << 
                std::to_string(audio.getCurrentApi()) << endl << endl <<
                "Maybe Jack Audio Kit is not running." << endl << endl;

            exit(EXIT_FAILURE);
        }
    }
    catch ( RtAudioError &error ) {
        std::cout << error.getMessage();

        exit(EXIT_FAILURE);
    }

    // Get the default audio device and set stream parameters
    RtAudio::StreamParameters streamParams;
    streamParams.deviceId = audio.getDefaultOutputDevice();
    streamParams.nChannels = nChannels;
    streamParams.firstChannel = 0;

    RtAudio::StreamOptions streamOps;
    streamOps.streamName = "a" + to_string(oscPort) + playerUuid;

    std::cout << "audioplayer UUID: " << streamOps.streamName << endl << endl;

    try {
        audio.openStream(  &streamParams, 
                            NULL, 
                            RTAUDIO_SINT16, 
                            sampleRate, 
                            &bufferFrames, 
                            &audioCallback,
                            (void *) this,
                            &streamOps );

        audio.startStream();
    }
    catch (RtAudioError &error) {
        std::cout << error.getMessage();

        exit( EXIT_FAILURE );
    }


}

//////////////////////////////////////////////////////////
AudioPlayer::~AudioPlayer( void ) {
    std::string message;

    try {
        // Stop the stream
        audio.abortStream();
    }
    catch (RtAudioError& e) {
        std::cout << e.getMessage();

    }

    // Clean up
    // fclose(data.fd);
    audio.closeStream();

    // Delete dinamically reserved members
    delete []volumeMaster;
    delete []intermediate;

}

//////////////////////////////////////////////////////////
// Midi callback - Static member function
int AudioPlayer::audioCallback( void *outputBuffer, void * /*inputBuffer*/, unsigned int nBufferFrames,
            double /*streamTime*/, RtAudioStreamStatus /*status*/, void *data ) {

    AudioPlayer *ap = (AudioPlayer*) data;

    // If we are playing audio...
    if (    ap->audioFile.good() && ! ap->audioFile.eof() &&
            ap->mtcReceiver.isTimecodeRunning && 
            ap->followingMtc ) {

        unsigned int count = 0;
        unsigned int read = 0;

        ap->headMutex.lock();

        double mtcHeadInBytes = ( ap->mtcReceiver.mtcHead / 1000 ) * (double) ap->audioSecondSize;

        double difference = fabs( ap->playHead - mtcHeadInBytes );
        double tollerance = 2 * (1 / (double) ap->mtcReceiver.curFrameRate ) * (double) ap->audioSecondSize;
        
        // If our audio play head is out of the boundaries of our mtc frame
        // tollerance... We correct it.
        //      - 2 MTC frames of tollerance
        if ( difference > tollerance || ap->offsetChanged ) {
            // Set new head
            ap->playHead = mtcHeadInBytes;

            // Set new offset if any
            if ( ap->offsetChanged ) {
                ap->headOffset = ap->headNewOffset;
                ap->offsetChanged = false;
            }

            // Seek the file with the new coordinates
            ap->audioFile.seekg((streamoff)(ap->playHead + ap->headOffset), ios_base::beg);
        }

        for ( unsigned int i = 0; i < nBufferFrames; i++ ) {
            // Per channel reading
            read = 0;
            for ( unsigned int i = 0; i < ap->nChannels; i++) {
                // Read operation to the intermediate buffer, we read the size of our 
                // sample by the number of channels times, from file
                // read += fread( &intermediate[i], 1, ap->headStep, file );
                ap->audioFile.read((char*) &ap->intermediate[i], ap->headStep);
                read += ap->audioFile.gcount();

                ap->intermediate[i] *= ap->volumeMaster[i];

            }

            memcpy((char *)outputBuffer + count, ap->intermediate, ap->audioFrameSize);

            if ( read > 0 ) count += read; else break;
        }

        ap->playHead += count;

        ap->headMutex.unlock();

        // If we didn't read enough butes to fill the buffer, let's put some
        // silence aferwards copying zeros to the rest of the buffer
        /*
        if ( count < nBufferFrames ) {
            unsigned long int bytes = (nBufferFrames - count) * ap->audioFrameSize;
            unsigned long int startByte = count * ap->audioFrameSize;

            memset( (char *)(outputBuffer) + startByte, 0, bytes );

            ap->endOfStream = true;
            return 1;
        }
        */

        if ( ap->audioFile.eof() ) {
            ap->endOfStream = true;
        }
    }
    // If we are not playing audio... Just copy silence...
    else {
        memset( (char *)(outputBuffer), 0, nBufferFrames * ap->audioFrameSize );

    }

    return 0;

}

////////////////////////////////////////////
void AudioPlayer::ProcessMessage( const osc::ReceivedMessage& m, 
            const IpEndpointName& /*remoteEndpoint*/ )
{
    // std::cout << m.AddressPattern();
    try{
        // Parsing OSC audioplayer messages
        // Volume channel 0
        if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/vol0").c_str() ) == 0 ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[0] >> osc::EndMessage;
            m.ArgumentStream() >> volumeMaster[0] >> osc::EndMessage;
            
        // Volume channel 1
        } else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/vol1").c_str() ) == 0 ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[1] >> osc::EndMessage;
            m.ArgumentStream() >> volumeMaster[1] >> osc::EndMessage;
            
        // Volume master
        } else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/volmaster").c_str() ) == 0 ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[0] >> osc::EndMessage;
            m.ArgumentStream() >> volumeMaster[0] >> osc::EndMessage;
            volumeMaster[1] = volumeMaster[0];

        // Offset
        } else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/offset").c_str() ) == 0 ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[0] >> osc::EndMessage;
            float offsetOSC;
            m.ArgumentStream() >> offsetOSC >> osc::EndMessage;

            // Offset argument in OSC command is in milliseconds
            // so we need to calculate in bytes in our file

            headNewOffset = offsetOSC / 1000;                     // To seconds
            headNewOffset *= (double) audioSecondSize;  // To bytes

            // Plus the standard header size
            headNewOffset += audioFile.headerSize;

            offsetChanged = true;

        } else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/load").c_str() ) == 0 ) {
            const char* newPath;
            m.ArgumentStream() >> newPath >> osc::EndMessage;
            audioPath = newPath;
            std::cout << "audioPath : " << audioPath << endl;
            audioFile.close();
            std::cout << "File closed" << endl;
            audioFile.loadFile(audioPath.c_str());
            std::cout << "loadFile Called" << endl;
        } else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/play").c_str() ) == 0 ) {
            // TO DO
        } else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/stop").c_str() ) == 0 ) {
            // TO DO
        /*} else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/quit").c_str() ) == 0 ) {
            raise(SIGTERM);
        } */
        } else if ( (string)m.AddressPattern() == (OscReceiver::oscAddress + "/quit") ) {
            raise(SIGTERM);
        }
        
    } catch ( osc::Exception& e ) {
        // any parsing errors such as unexpected argument types, or 
        // missing arguments get thrown as exceptions.
        std::cout << "error while parsing message: "
            << m.AddressPattern() << ": " << e.what() << "\n";
    }
}

//////////////////////////////////////////////////////////
/*
bool AudioPlayer::loadNodeConfig( void ) {
    cout << "Node config read!" << endl;
    return true;
}
*/

//////////////////////////////////////////////////////////
/*
bool AudioPlayer::loadMediaConfig( void ) {
    cout << "Media config read!" << endl;
    return true;
}
*/

//////////////////////////////////////////////////////////
// Logging to be implemented later
/*
void AudioPlayer::log( std::string* message ) {
    std::cout << *message << endl;
}
*/

