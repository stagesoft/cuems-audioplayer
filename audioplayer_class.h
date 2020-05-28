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
#ifndef XJADEO_ADJUSTMENT
#define XJADEO_ADJUSTMENT 65
#endif
#ifndef MTC_FRAMES_TOLLERANCE
#define MTC_FRAMES_TOLLERANCE 2
#endif

#include <atomic>
#include <math.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <csignal>
#include <rtaudio/RtAudio.h>
#include <rtmidi/RtMidi.h>
#include "audiofstream_class.h"
#include "./sysqlogger_class/sysqlogger_class.h"
#include "sysq_errors.h"
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
                        long int initOffset = 0,
                        long int finalWait = 0,
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
        unsigned int audioMillisecondSize;              // Audio millisecond size in bytes

        short int* intermediate;
        float* volumeMaster;             // Volumen master multiplier TODO: per channel

        // Our midi and audio objects
        RtAudio audio;
        MtcReceiver mtcReceiver;
        AudioFstream audioFile;

        // Stream and playing control vars
        static bool endOfStream;                // Is the end of the stream reached already?
        static bool endOfPlay;                  // Are we done playing and waiting?
        long int endTimeStamp = 0;              // Our finish timestamp to calculate end wait
        static bool followingMtc;               // Is player following MTC?

        static long long int playHead;          // Current reading head position in bytes

        // float headSpeed;                       // Head speed (TO DO)
        // float headAccel;                       // Head acceleration (TO DO)
        // std::atomic<double> playheadControl;   // Head reading direction, (TO DO)

        unsigned int headStep = 2;              // Head step per channel, by now SINT16 format, 2 bytes
        long int headOffset = 0;                // Head offset
        bool offsetChanged = false;             // Flag to recognise when the offset is OSC changed
        long int headNewOffset = 0;             // Head offset to update through OSC

        long int endWaitTime = 0;               // End time to wait before quitting

        string playerUuid = "";                 // Player UUID for identification porpouses

        bool stopOnMTCLost = true;              // Stop on MTC signal lost?
        bool mtcSignalLost = false;             // Flag to check MTC signal lost?
        bool mtcSignalStarted = false;          // Flag to check MTC signal started?

    //////////////////////////////////////////////////////////
    // Private members
    private:

        // Config functions, maybe to be implemented
        // bool loadNodeConfig( void );
        // bool loadMediaConfig( void );

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