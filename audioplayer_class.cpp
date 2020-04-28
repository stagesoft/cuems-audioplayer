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
// bool AudioPlayer::stopOnLost = false;

double AudioPlayer::playHead = 0;
bool AudioPlayer::followingMtc = true;
bool AudioPlayer::isStreamRunning = false;
bool AudioPlayer::endOfStream = false;

//////////////////////////////////////////////////////////
AudioPlayer::AudioPlayer(   int port, 
                            const char *oscRoute ,
                            const char *filePath , 
                            unsigned int numberOfChannels, 
                            unsigned int sRate, 
                            unsigned int device,
                            RtAudio::Api audioApi )
                            :   // Members initialization
                            OscReceiver(port, oscRoute),
                            audioPath(filePath),
                            nChannels(numberOfChannels),
                            sampleRate(sRate),
                            deviceID(device),
                            audio(audioApi),
                            audioFile(filePath)
 {
    //////////////////////////////////////////////////////////
    // Config tasks to be implemented later maybe
    // loadNodeConfig();
	// loadMediaConfig();

    //////////////////////////////////////////////////////////
    // Set up working class members
    // MTC follow tolerance
    followTollerance = TOLLERANCE * (sampleRate / 1000) * nChannels * headStep;
    // Per channel volume param to process audio
    volumeMaster = new float[nChannels];
    for ( unsigned short int i = 0; i < nChannels; i++ ) {
        volumeMaster[i] = 1.0;
    }
    // File header read so initial offset
    headOffset = audioFile.tellg();
    playHead = headOffset;



    //////////////////////////////////////////////////////////
    // Setting our audio stream parameters
    // Check for audio devices
    if ( audio.getDeviceCount() == 0 ) {
        std::cout << "No audio devices found on API:" << std::to_string(audio.getCurrentApi()) << endl;

        exit(EXIT_FAILURE);
    }

    // Get the default audio device and set stream parameters
    streamParams.deviceId = audio.getDefaultOutputDevice();
    streamParams.nChannels = nChannels;
    streamParams.firstChannel = 0;

    try {
        audio.openStream(  &streamParams, 
                            NULL, 
                            RTAUDIO_SINT16, 
                            sampleRate, 
                            &bufferFrames, 
                            &audioCallback,
                            (void *) this );

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

}

//////////////////////////////////////////////////////////
// Midi callback - Static member function
int AudioPlayer::audioCallback( void *outputBuffer, void * /*inputBuffer*/, unsigned int nBufferFrames,
            double /*streamTime*/, RtAudioStreamStatus /*status*/, void *data ) {

    AudioPlayer *ap = (AudioPlayer*) data;

    unsigned int count = 0;
    short int* intermediate;
    unsigned int read = 0;
    double mtcHead = ap->mtcReceiver.mtcHead;

    intermediate = new short int[ap->nChannels];

    if ( ap->audioFile.good() && ap->mtcReceiver.isTimecodeRunning && ap->followingMtc ) {

        unsigned int audioFrameSize = ap->nChannels * ap->headStep;
        double playHeadInMs = (ap->playHead / (ap->sampleRate * audioFrameSize)) * 1000;
        
        // If our audio play head is out of the boundaries of our mtc frame
        // tollerance... We correct it.
        if ( fabs(playHeadInMs - mtcHead) > (2 * 1000 / ap->mtcReceiver.curFrameRate) ) {
            ap->playHead = ap->headOffset + (( mtcHead / 1000 ) * ap->sampleRate * audioFrameSize);

            ap->audioFile.seekg((streamoff)(ap->playHead), ios_base::beg); 

        }

        for ( unsigned int i = 0; i < nBufferFrames; i++ ) {
            // Per channel reading
            read = 0;
            for ( unsigned int i = 0; i < ap->nChannels; i++) {
                // Read operation to the intermediate buffer, we read the size of our 
                // sample by the number of channels times, from file
                // read += fread( &intermediate[i], 1, ap->headStep, file );
                ap->audioFile.read((char*) &intermediate[i], ap->headStep);
                read += ap->audioFile.gcount();

                intermediate[i] *= ap->volumeMaster[i];

            }

            memcpy((char *)outputBuffer + count, intermediate, audioFrameSize);

            if ( read > 0 ) count += read; else break;
        }

        ap->playHead += count;

        if ( count < nBufferFrames ) {
            unsigned long int bytes = (nBufferFrames - count) * ap->nChannels * ap->headStep;
            unsigned long int startByte = count * ap->nChannels * ap->headStep;

            memset( (char *)(outputBuffer) + startByte, 0, bytes );

            ap->endOfStream = true;
            delete[] intermediate;
            return 1;
        }

    }
    else {
        memset( (char *)(outputBuffer), 0, nBufferFrames * ap->nChannels * ap->headStep );

    }

    delete[] intermediate;
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
            fileLoaded = !fileLoaded;
        } else if ( std::strcmp( m.AddressPattern(), (OscReceiver::oscAddress + "/stop").c_str() ) == 0 ) {
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

