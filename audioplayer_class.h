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
// Stage Lab SysQ audio player class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
#ifndef AUDIOPLAYER_CLASS_H
#define AUDIOPLAYER_CLASS_H

//////////////////////////////////////////////////////////
// Preprocessor definitions
// #define TIXML_USE_STL
// #define __LINUX_ALSA__
// #define __UNIX_JACK__
#define TOLLERANCE 2000

#include <atomic>
#include <math.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <csignal>
#include <rtaudio/RtAudio.h>
#include <rtmidi/RtMidi.h>
#include "audiofstream_class.h"
#include "./mtcreceiver_class/mtcreceiver_class.h"
#include "./oscreceiver_class/oscreceiver_class.h"

using namespace std;

class AudioPlayer : public OscReceiver
{
    //////////////////////////////////////////////////////////
    // Public members
    public:
        //////////////////////////////////////////
        // Constructors and destructors
        AudioPlayer(    int port = 7000,
                        double initOffset = 0,
                        double finalWait = 0,
                        const string oscRoute = "/",
                        const string filePath = "", 
                        const string uuid = "",
                        const bool stopOnLostFlag = true,
                        unsigned int numberOfChannels = 2, 
                        unsigned int sRate = 44100, 
                        unsigned int device = 0,
                        RtAudio::Api audioApi = RtAudio::Api::UNIX_JACK );
        ~AudioPlayer( void );
        //////////////////////////////////////////

        // Audio sample data
        string audioPath;

        // Audio stream settings
        // RtAudio::StreamParameters streamParams;         // Stream parameters
        // unsigned int audioApi;                       // Our selected Audio API
        unsigned int nChannels;                         // Our default number of audio channels
        unsigned int sampleRate;                        // Our sample rate
        unsigned int bufferFrames;                      // 2048 sample frames
        unsigned int deviceID;                          // 0 -> default device

        unsigned int audioFrameSize;                    // Audio frame size in bytes
        unsigned int audioSecondSize;                   // Audio second size in bytes

        short int* intermediate;
        float* volumeMaster;             // Volumen master multiplier TODO: per channel

        // Our midi and audio objects
        RtAudio audio;
        MtcReceiver mtcReceiver;
        AudioFstream audioFile;

        // Stream control vars
        static bool isStreamRunning;            // Is the timecode sync running?
        static bool endOfStream;                // Is the end of the stream reached already?
        static bool followingMtc;               // Is head following MTC?

        static double playHead;                 // Current reading head position in bytes
        std::mutex headMutex;

        float headSpeed;                       // Head speed
        float headAccel;                       // Head acceleration
        std::atomic<double> playheadControl;
        unsigned int headStep = 2;              // Head step per channel, by now SINT16 format, 2 bytes
        double headOffset = 0;                  // Head offset
        bool offsetChanged = false;
        double headNewOffset = 0;               // Head offset to update through OSC

        double endWaitTime = 0;                 // End time to wait before quitting

        string playerUuid = "";                 // Player UUID for identification porpouses

        bool stopOnMTCLost = true;              // Stop on MTC signal lost?

    //////////////////////////////////////////////////////////
    // Private members
    private:

        // Config functions, maybe to be implemented
        // bool loadNodeConfig( void );
        // bool loadMediaConfig( void );

        // Logging functions
        void log( std::string* message );

        //////////////////////////////////////////////////////////
        // Callbacks for audio an dmidi
        // RtAudio callback function
        static int audioCallback(   void *outputBuffer, void * inputBuffer, unsigned int nBufferFrames,
                                    double streamTime, RtAudioStreamStatus status, void *data );

    //////////////////////////////////////////////////////////
    // Protected members
    protected:
        virtual void ProcessMessage(    const osc::ReceivedMessage& m, 
                                    const IpEndpointName& /*remoteEndpoint*/ );


};

#endif // AUDIOPLAYER_CLASS_H