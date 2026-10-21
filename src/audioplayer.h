/* LICENSE TEXT

    audioplayer for linux based using RtAudio and RtMidi libraries to
    process audio and receive MTC sync. It also uses oscpack to receive
    some configurations through osc commands.
    Copyright (C) 2020  Stage Lab Coop.

    Authors:
        Alex Ramos <alex@stagelab.coop>
        Ion Reguera <ion@stagelab.coop>

    Portions of the file reading and audio decoding code are based on mpv
    (https://mpv.io/), which is licensed under GPLv2+ and LGPLv2.1+.

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
// SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>

//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
// Stage Lab Cuems audio player class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

#include <atomic>
#include <math.h>
#include <chrono>
#include <memory>
#include <vector>
#include <iostream>
#include <iomanip>
#include <csignal>
#include <rtaudio/RtAudio.h>
#include <rtmidi/RtMidi.h>
#include "audiofstream.h"
#include "cuemslogger.h"
#include "cuems_errors.h"
#include "mtcreceiver.h"
#include "oscreceiver.h"
#include "playercore.h"
#include "qfseqlock.h"

using namespace std;

// The player follows MTC with the feed engine (AudioFstream, plan 869ejxng3 +
// 1amfwm7e6cw rev 3 step 1) driven by PlayerCore (step 2): content-sample
// timebase in ns, the trusted-tick seqlock, MTC liveness, the PI lock loop on
// the resampler's ratio, the latched OSC events, the LOOP acks and a
// lock-free journal ring drained by the main loop.
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
                        const string deviceName = "",
                        const string &client_name = "Audio_Player",
                        const bool stopOnLostFlag = true,
                        const bool mtcFollowFlag = false,
                        unsigned int numberOfChannels = 2,
                        unsigned int sRate = 44100,
                        RtAudio::Api audioApi = RtAudio::Api::UNIX_JACK,
                        const string &resampleQuality = "hq",
                        long explicitLatencyMs = -1 );

        // Set the audio output-pipeline latency compensation in ms.
        // Values outside [0, 500] are clamped.
        void setOutputLatencyMs(long ms);
        ~AudioPlayer( void );
        //////////////////////////////////////////

        // Audio sample data
        string audioPath;

        unsigned int nChannels;                         // Our default number of audio channels
        unsigned int sampleRate;                        // Our sample rate (JACK's)
        unsigned int bufferFrames;                      // JACK period
        string deviceName;

        float* volumeMaster;                            // Volumen master multiplier

        // Our midi, osc and audio objects
        RtAudio audio;
        MtcReceiver mtcReceiver;
        AudioFstream audioFile;
        std::unique_ptr<PlayerCore> core;
        TickSeqlock ticks;                              // written by the MTC tick callback

        // Stream and playing control flags and vars
        static std::atomic <bool> endOfPlay;            // Are we done playing and waiting?
        static std::atomic <bool> quitRequested;        // /quit, SIGTERM, SIGINT: the main loop acts (I16e, J16)

        long int endWaitTime = 0;               // End time to wait before quitting
        string playerUuid = "";                 // Player UUID for identification porpouses
        string slug = "";                       // the logger slug, for the stdout ack lines
        bool stopOnMTCLost = true;              // --ciml: kept for the CLI, no effect in this release (G2)

        // Main loop: write the journal and the mode-change ack lines
        void drainJournal( void );

    //////////////////////////////////////////////////////////
    // Private members
    private:
        std::atomic<long> outputLatencyFrames_{0};
        long m_explicitLatencyMs;               // -1 = no override, use JACK's value
        TickSample lastTick_;                   // the audio thread's last seqlock read
        int64_t latencyNs( void ) const;

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

#endif // AUDIOPLAYER_H
