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
// Stage Lab Cuems audio player class code file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "audioplayer.h"

////////////////////////////////////////////
// Initializing static class members

std::atomic<long long int> AudioPlayer::playHead(0);
std::atomic <bool> AudioPlayer::endOfStream = false;
std::atomic <bool> AudioPlayer::endOfPlay = false;
std::atomic <bool> AudioPlayer::outOfFile = false;

//////////////////////////////////////////////////////////
AudioPlayer::AudioPlayer(   int port, 
                            long int initOffset,
                            long int finalWait,
                            const string oscRoute,
                            const string filePath, 
                            const string deviceName,
                            const string &client_name,
                            const bool stopOnLostFlag,
                            const bool mtcFollowFlag,
                            unsigned int numberOfChannels, 
                            unsigned int sRate,
                            RtAudio::Api audioApi,
                            const string &resampleQuality )
                            :   // Members initialization
                            OscReceiver(port, oscRoute.c_str()),
                            mtcReceiver(RtMidiIn::LINUX_ALSA, client_name),
                            audioPath(filePath),
                            nChannels(numberOfChannels),
                            sampleRate(sRate),
                            deviceName(deviceName),
                            audio(audioApi),
                            audioFile(filePath.c_str()),  // Open file to check format
                            endWaitTime(finalWait),
                            stopOnMTCLost(stopOnLostFlag),
                            followingMtc(mtcFollowFlag)
 {
    //////////////////////////////////////////////////////////
    // Config tasks to be implemented later maybe
    // loadNodeConfig();
	// loadMediaConfig();

    //////////////////////////////////////////////////////////
    // Set up working class members

    // Set resample quality before opening audio stream
    audioFile.setResampleQuality(resampleQuality);

    // Audio frame size calc (will be updated with actual JACK sample rate later)
    audioFrameSize = nChannels * headStep;
    audioSecondSize = sampleRate * audioFrameSize;
    audioMillisecondSize = ( (float) sampleRate / 1000 ) * audioFrameSize;

    // Adjust initial offset
    //      NOTE: Hardcoded 50 ms offset while testing sync with Xjadeo
    headOffset = (initOffset + XJADEO_ADJUSTMENT) * audioMillisecondSize;
    // Note: With FFmpeg, headers are handled internally - no manual offset needed

    // If we have a positive offset initialli we can already
    // seek the file to its proper initial position
    if ( (playHead + headOffset) >= 0 )
        audioFile.seekg( playHead + headOffset , ios_base::beg );

    // Per channel volume param to process audio
    volumeMaster = new float[nChannels];
    for ( unsigned short int i = 0; i < nChannels; i++ ) {
        volumeMaster[i] = 1.0;
    }

    // Per channel process buffer (32-bit float for JACK)
    intermediate = new float[nChannels];


    //////////////////////////////////////////////////////////
    // Setting our audio stream parameters
    // Check for audio devices
    int audioDeviceId = -1;
    try {
        int deviceCount = audio.getDeviceCount();
        if ( deviceCount == 0 ) {
            std::string str = "No audio devices found on API:" + 
                std::to_string(audio.getCurrentApi());

            std::cerr << str << endl;
            CuemsLogger::getLogger()->logError(str);

            str = "Maybe JACK NOT RUNNING!!!";

            std::cerr << str << endl;
            CuemsLogger::getLogger()->logError(str);

            exit( CUEMS_EXIT_AUDIO_DEVICE_ERR );
        }
        else {
            // Helper function to check if a device is suitable
            auto isDeviceSuitable = [&](int deviceId) -> bool {
                try {
                    RtAudio::DeviceInfo info = audio.getDeviceInfo(deviceId);
                    // Check if device is probed successfully and has enough output channels
                    return info.probed && 
                           info.outputChannels >= nChannels;
                } catch (...) {
                    return false;
                }
            };

            // First, try to find device by name (if specified)
            bool found = false;
            if (!deviceName.empty()) {
                for(int i = 0; i < deviceCount; ++i) {
                    try {
                        RtAudio::DeviceInfo info = audio.getDeviceInfo(i);
                        if (info.name == deviceName && isDeviceSuitable(i)) {
                            audioDeviceId = i;
                            found = true;
                            CuemsLogger::getLogger()->logInfo("Found specified device: " + deviceName);
                            break;
                        }
                    } catch (...) {
                        // Skip devices that fail to probe
                        continue;
                    }
                }
            }

            // If device name not found or not suitable, find any suitable output device
            if (!found) {
                // Try default output device first
                int defaultDevice = audio.getDefaultOutputDevice();
                if (defaultDevice >= 0 && defaultDevice < deviceCount && isDeviceSuitable(defaultDevice)) {
                    audioDeviceId = defaultDevice;
                    found = true;
                    CuemsLogger::getLogger()->logInfo("Using default output device");
                } else {
                    // Iterate through all devices to find a suitable one
                    for(int i = 0; i < deviceCount; ++i) {
                        if (isDeviceSuitable(i)) {
                            audioDeviceId = i;
                            found = true;
                            try {
                                RtAudio::DeviceInfo info = audio.getDeviceInfo(i);
                                CuemsLogger::getLogger()->logInfo("Using device: " + info.name + 
                                    " (" + std::to_string(info.outputChannels) + " output channels)");
                            } catch (...) {
                                CuemsLogger::getLogger()->logInfo("Using device ID: " + std::to_string(i));
                            }
                            break;
                        }
                    }
                }
            }

            // If still no suitable device found, provide detailed error
            if (!found || audioDeviceId < 0) {
                std::string str = "No suitable audio output device found. ";
                str += "Required: " + std::to_string(nChannels) + " output channels. ";
                str += "Available devices:";
                
                std::cerr << str << endl;
                CuemsLogger::getLogger()->logError(str);
                
                // List available devices for debugging
                for(int i = 0; i < deviceCount; ++i) {
                    try {
                        RtAudio::DeviceInfo info = audio.getDeviceInfo(i);
                        std::string deviceStr = "  Device " + std::to_string(i) + ": " + info.name;
                        deviceStr += " (probed: " + std::string(info.probed ? "yes" : "no");
                        deviceStr += ", output channels: " + std::to_string(info.outputChannels) + ")";
                        std::cerr << deviceStr << endl;
                        CuemsLogger::getLogger()->logError(deviceStr);
                    } catch (...) {
                        std::string deviceStr = "  Device " + std::to_string(i) + ": (failed to probe)";
                        std::cerr << deviceStr << endl;
                        CuemsLogger::getLogger()->logError(deviceStr);
                    }
                }

                str = "Maybe JACK NOT RUNNING or no suitable output device available!";
                std::cerr << str << endl;
                CuemsLogger::getLogger()->logError(str);

                exit( CUEMS_EXIT_AUDIO_DEVICE_ERR );
            }
        }
    }
    catch ( RtAudioError &error ) {
        std::cerr << error.getMessage();
        CuemsLogger::getLogger()->logError( error.getMessage() );

        CuemsLogger::getLogger()->logInfo( "Exiting with result code: " + std::to_string(CUEMS_EXIT_AUDIO_DEVICE_ERR) );
        exit( CUEMS_EXIT_AUDIO_DEVICE_ERR );
    }

    // Get the default audio device and set stream parameters
    RtAudio::StreamParameters streamParams;
    streamParams.deviceId = audioDeviceId;
    streamParams.nChannels = nChannels;
    streamParams.firstChannel = 0;

    RtAudio::StreamOptions streamOps;

    // proto fruta, if we got uuid use only that


    streamOps.streamName = client_name;
    

    try {
        // For JACK/pw-jack: Query the device's native sample rate to avoid mismatch
        // This is crucial for pw-jack compatibility where the server rate may differ
        unsigned int requestedRate = sampleRate;  // Start with constructor's rate
        
        try {
            RtAudio::DeviceInfo deviceInfo = audio.getDeviceInfo(audioDeviceId);
            if (deviceInfo.probed && deviceInfo.sampleRates.size() > 0) {
                // Use the device's preferred (first) sample rate
                requestedRate = deviceInfo.sampleRates[0];
                CuemsLogger::getLogger()->logInfo("Using device native sample rate: " + 
                    std::to_string(requestedRate) + " Hz");
            }
        } catch (...) {
            // If we can't query, stick with requested rate
            CuemsLogger::getLogger()->logInfo("Could not query device sample rate, using requested: " + 
                std::to_string(requestedRate) + " Hz");
        }
        
        audio.openStream(  &streamParams, 
                            NULL, 
                            RTAUDIO_FLOAT32,  // JACK's native format
                            requestedRate,     // Use device's native rate
                            &bufferFrames, 
                            &audioCallback,
                            (void *) this,
                            &streamOps );

        audio.startStream();
        
        // Get actual JACK sample rate (JACK is the master)
        unsigned int jackSampleRate = audio.getStreamSampleRate();
        if (jackSampleRate != 0) {
            if (jackSampleRate != sampleRate) {
                CuemsLogger::getLogger()->logInfo("JACK server sample rate: " + std::to_string(jackSampleRate) + 
                                                   " Hz (application default was " + std::to_string(sampleRate) + " Hz, will resample)");
            } else {
                CuemsLogger::getLogger()->logInfo("JACK server sample rate: " + std::to_string(jackSampleRate) + " Hz");
            }
            
            // Use JACK's sample rate
            sampleRate = jackSampleRate;
            
            // Recalculate timing with actual JACK sample rate
            audioSecondSize = sampleRate * audioFrameSize;
            audioMillisecondSize = ( (float) sampleRate / 1000 ) * audioFrameSize;
            
            // Recalculate offset with correct sample rate
            headOffset = (initOffset + XJADEO_ADJUSTMENT) * audioMillisecondSize;
            // Note: With FFmpeg, headers are handled internally - no manual offset needed
        }
        
        // Check if we already have a valid audio file
        if (!audioFile.good()) {
            std::string str = "Error opening audio file: " + audioPath;
            std::cerr << str << endl;
            CuemsLogger::getLogger()->logError(str);
            exit(CUEMS_EXIT_AUDIO_DEVICE_ERR);
        }
        
        // Get the file's actual channel count
        unsigned int fileChannels = audioFile.getChannels();
        
        // Get device capabilities
        RtAudio::DeviceInfo deviceInfo = audio.getDeviceInfo(audioDeviceId);
        unsigned int deviceChannels = deviceInfo.outputChannels;
        
        // Determine target channels: use file's channels if device supports it, otherwise downmix
        // This is how mpv does it - only downmix when necessary
        if (fileChannels > deviceChannels) {
            // File has more channels than device supports, need to downmix
            std::cerr << "Device supports " << deviceChannels << " channels, file has " 
                      << fileChannels << " channels - will downmix" << endl;
            CuemsLogger::getLogger()->logInfo("Downmixing " + std::to_string(fileChannels) + 
                                              " channels to " + std::to_string(deviceChannels) + 
                                              " channels to match device capabilities");
            
            // Reopen the file with downmixing
            audioFile.close();
            audioFile.setTargetChannels(deviceChannels);
            audioFile.open(audioPath, ios::binary | ios::in);
            
            if (!audioFile.good()) {
                std::string str = "Error reopening audio file with downmixing: " + audioPath;
                std::cerr << str << endl;
                CuemsLogger::getLogger()->logError(str);
                exit(CUEMS_EXIT_AUDIO_DEVICE_ERR);
            }
            
            // Update nChannels to match downmixed output
            nChannels = deviceChannels;
        } else if (fileChannels < nChannels) {
            // File has fewer channels than requested, use file's channel count
            std::cerr << "File has " << fileChannels << " channels, using that instead of requested " 
                      << nChannels << " channels" << endl;
            CuemsLogger::getLogger()->logInfo("Using file's " + std::to_string(fileChannels) + 
                                              " channels (device supports " + std::to_string(deviceChannels) + ")");
            nChannels = fileChannels;
        } else {
            // Device has enough channels, use file's native channel count
            std::cerr << "Playing " << fileChannels << " channels (device supports " 
                      << deviceChannels << " channels)" << endl;
            CuemsLogger::getLogger()->logInfo("Playing " + std::to_string(fileChannels) + 
                                              " channels (device supports " + std::to_string(deviceChannels) + ")");
            nChannels = fileChannels;
        }
        
        // Configure resampling in audio file if needed (libsoxr will handle rate conversion)
        audioFile.setTargetSampleRate(sampleRate);
    }
    catch (RtAudioError &error) {
        std::cerr << error.getMessage();
        CuemsLogger::getLogger()->logError( error.getMessage() );

        CuemsLogger::getLogger()->logInfo( "Exiting with result code: " + std::to_string(CUEMS_EXIT_AUDIO_DEVICE_ERR) );
        exit( CUEMS_EXIT_AUDIO_DEVICE_ERR );
    }


}

//////////////////////////////////////////////////////////
AudioPlayer::~AudioPlayer( void ) {
    try {
        // Stop the stream
        audio.abortStream();
    }
    catch (RtAudioError& error) {
        std::cerr << error.getMessage();
        CuemsLogger::getLogger()->logError( error.getMessage() );
    }

    // Clean up
    audio.closeStream();

    // Delete dinamically reserved members
    delete []volumeMaster;
    delete []intermediate;
}

//////////////////////////////////////////////////////////
// RtAudio callback - Static member function
int AudioPlayer::audioCallback( void *outputBuffer, void * /*inputBuffer*/, unsigned int nBufferFrames,
            double /*streamTime*/, RtAudioStreamStatus /*status*/, void *data ) {

    AudioPlayer *ap = (AudioPlayer*) data;

    // If we are receiving MTC and following it...
    // Or we are not receiving it and we do not stop on its lost
    // And we haven't reached the end of the file...
    if (    ( (ap->mtcReceiver.isTimecodeRunning && ap->followingMtc) || 
            (ap->mtcSignalLost && !ap->stopOnMTCLost) ) &&
            ap->playheadControl == 1 ) {
        unsigned int count = 0;
        unsigned int read = 0;

        // Check play control flags
        // If there is MTC signal and we haven't started, check it
        if ( ap->mtcReceiver.isTimecodeRunning ) {
            if ( !ap->mtcSignalStarted ) {
                CuemsLogger::getLogger()->logInfo("MTC -> Play started");
                ap->mtcSignalStarted = true;
            }
            else {
                if ( ap->mtcSignalLost ) {
                    CuemsLogger::getLogger()->logInfo("MTC -> Play resumed");
                }
            }

            // Receiving MTC, means that signal is not lost anymore
            ap->mtcSignalLost = false;
        }
        // Either, if there is no MTC signal and we already started, it is lost
        else {
            if ( ap->mtcSignalStarted && !ap->mtcSignalLost ) {
                CuemsLogger::getLogger()->logInfo("MTC signal lost");
                ap->mtcSignalLost = true;
            }
        }

        // Now we start playing in two different cases:
        // 1) after MTC: if there is MTC signal then we treat it
        if ( ap->mtcReceiver.isTimecodeRunning && ap->followingMtc && !ap->mtcSignalLost )
        {
            // Tollerance 2 frames, due to the MTC 8 packages -> 2 frames relay
            long int tollerance = MTC_FRAMES_TOLLERANCE * ( 1000 / (float) ap->mtcReceiver.curFrameRate ) * ap->audioMillisecondSize;
            
            long int mtcHeadInBytes = ap->mtcReceiver.mtcHead * ap->audioMillisecondSize ;

            long int difference = ap->playHead - mtcHeadInBytes;

            // If our audio play head is too late or out of the boundaries of our mtc frame
            // tollerance... We correct it. Also if the offset changed dinamically via OSC
            if ( abs(difference) > tollerance || ap->offsetChanged ) {
                // Set new OSC offset if any
                if ( ap->offsetChanged ) {
                    ap->headOffset = ap->headNewOffset;
                    // And reset flag
                    ap->offsetChanged = false;
                    // Note: Don't set outOfFile here - let the boundary check below determine that
                }

                // Calculate the actual seek position in the file (accounting for offset)
                long long seekPosition = mtcHeadInBytes + ap->headOffset;
                unsigned long long fileSize = ap->audioFile.getFileSize();
                
                if ( (seekPosition >= 0) && (seekPosition <= (long long)fileSize) ){

                    ap->endOfStream = false;
                    ap->outOfFile = false;
                    if ( ap->audioFile.eof() ) {
                        ap->audioFile.clear();
                    } 
                    // Seek to the calculated position
                    ap->audioFile.seekg( seekPosition , ios_base::beg );
                    // Update playHead to match where we actually are (without offset, as offset is separate)
                    ap->playHead = seekPosition - ap->headOffset;
                }
                else {
                    CuemsLogger::getLogger()->logInfo("Out of file boundaries!");
                    // Clear error flags to allow responding to future offset changes
                    // (e.g., when OSC offset command moves position back into bounds)
                    ap->audioFile.clear();
                    ap->endOfStream = true;
                    ap->outOfFile = true;
                }

            }
        }

        // 2) without MTC: we do not treat it but, in any case, we continue playing
        //      while we are not out of the file boundaries
        if ( !ap->outOfFile ) {
            // Calculate total bytes to read for this buffer
            unsigned long int bytesToRead = nBufferFrames * ap->audioFrameSize;
            
            if ( (ap->playHead + ap->headOffset) >= 0 ) {
                // Read entire buffer in ONE call - much more efficient for resampling!
                ap->audioFile.read((char*) outputBuffer, bytesToRead);
                count = ap->audioFile.gcount();
                
                // Apply volume to each sample
                float* floatBuffer = (float*)outputBuffer;
                for ( unsigned int i = 0; i < count / 4; i++ ) {
                    floatBuffer[i] *= ap->volumeMaster[i % ap->nChannels];
                }
            }
            else {
                // Before file start - fill with silence
                memset(outputBuffer, 0, bytesToRead);
                count = bytesToRead;
            }

            ap->playHead += count;
        }

        // If we didn't read enough bytes to fill the buffer, let's put some
        // silence aferwards copying zeros to the rest of the buffer
        if ( count < nBufferFrames ) {
            unsigned long int bytes = (nBufferFrames - count) * ap->audioFrameSize;
            unsigned long int startByte = count * ap->audioFrameSize;

            memset( (char *)(outputBuffer) + startByte, 0, bytes );

           // ap->outOfFile = true;
        }

        // If we did not read anything, we are out of boundaries, maybe...
        if ( count == 0 ) {
            // Maybe it is the end of the stream
            if ( ap->endWaitTime == 0 ) {
                // If there is not waiting time, we just finish
                // and we end the stream by returning a positive value
                ap->endOfPlay = true;
                
                return 1;
            }
            else {
                // If we have waiting time set...
                if ( ap->endTimeStamp == 0 ) {
                    // We note down our timestamp
                    ap->endTimeStamp = chrono::duration_cast<chrono::milliseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();

                    std::string str;
                    if ( ap->endWaitTime == __LONG_MAX__ ) 
                        str = "for quit command";
                    else
                        str = std::to_string( ap->endWaitTime ) + " ms";

                    CuemsLogger::getLogger()->logInfo("Out of file boundaries, waiting " + str);
                }
                
                ap->endOfStream = true;

                long int timecodeNow = chrono::duration_cast<chrono::milliseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();
                
                if ( ( timecodeNow - ap->endTimeStamp ) > ap->endWaitTime ) {
                    CuemsLogger::getLogger()->logInfo("Waiting time exceded, ending audioplayer");
                    ap->endOfPlay = true;
                    return 1;
                }
                
                
            }
        }
        else {
            ap->endOfStream = false;
            ap->endOfPlay = false;
            ap->endTimeStamp = 0;
            ap->outOfFile = false;
        }
    }
    // If we are not playing audio... Just copy silence...
    else
    {
        // Check play control flags
        // if we already started and we have no MTC signal, and it was 
        // already lost, it is lost now
        if (    !ap->mtcReceiver.isTimecodeRunning && 
                ap->mtcSignalStarted && 
                !ap->mtcSignalLost ) {
            CuemsLogger::getLogger()->logInfo("MTC signal lost");
            ap->mtcSignalLost = true;
        }

        memset( (char *)(outputBuffer), 0, nBufferFrames * ap->audioFrameSize );
    }

    // At the end, if we are following MTC but it is not running right now
    // let's fix our head to MTC head now it's stopped
    /*
    if ( ! ap->mtcReceiver.isTimecodeRunning && ap->followingMtc ) {
        ap->playHead = ap->mtcReceiver.mtcHead * ap->audioMillisecondSize;
    }
    */

    return 0;

}

////////////////////////////////////////////
// OSC process message callback
void AudioPlayer::ProcessMessage( const osc::ReceivedMessage& m, 
            const IpEndpointName& /*remoteEndpoint*/ )
{
    try{
        // Parsing OSC audioplayer messages
        // Volume channel 0
        if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/vol0") ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[0] >> osc::EndMessage;
            m.ArgumentStream() >> volumeMaster[0] >> osc::EndMessage;
            CuemsLogger::getLogger()->logInfo("OSC: new volume channel 0 " + std::to_string(volumeMaster[0]));
            
        // Volume channel 1
        } else if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/vol1") ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[1] >> osc::EndMessage;
            m.ArgumentStream() >> volumeMaster[1] >> osc::EndMessage;
            CuemsLogger::getLogger()->logInfo("OSC: new volume channel 1 " + std::to_string(volumeMaster[1]));
            
        // Volume master
        } else if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/volmaster") ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[0] >> osc::EndMessage;
            m.ArgumentStream() >> volumeMaster[0] >> osc::EndMessage;
            volumeMaster[1] = volumeMaster[0];
            CuemsLogger::getLogger()->logInfo("OSC: new volume master " + std::to_string(volumeMaster[0]));

        // Offset
        } else if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/offset") ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[0] >> osc::EndMessage;
            float offsetOSC;
            m.ArgumentStream() >> offsetOSC >> osc::EndMessage;
            offsetOSC = floor(offsetOSC);

            CuemsLogger::getLogger()->logInfo("OSC: new offset value " + std::to_string((long int)offsetOSC));

            // Offset argument in OSC command is in milliseconds
            // so we need to calculate in bytes in our file

            headNewOffset = ( offsetOSC + XJADEO_ADJUSTMENT ) * audioMillisecondSize;             // To bytes

            // Note: With FFmpeg, headers are handled internally - no manual offset needed

            offsetChanged = true;

        // Wait
        } else if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/wait") ) {
            // osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
            // args >> volumeMaster[0] >> osc::EndMessage;
            float waitOSC;
            m.ArgumentStream() >> waitOSC >> osc::EndMessage;
            waitOSC = floor(waitOSC);

            CuemsLogger::getLogger()->logInfo("OSC: new end wait value " + std::to_string((long int)waitOSC));

            endWaitTime = waitOSC;             // In milliseconds
        // Load
        } else if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/load") ) {
            const char* newPath;
            m.ArgumentStream() >> newPath >> osc::EndMessage;
            audioPath = newPath;
            CuemsLogger::getLogger()->logInfo("OSC: /load command");
            audioFile.close();
            CuemsLogger::getLogger()->logInfo("OSC: previous file closed");
            audioFile.loadFile(audioPath);
            CuemsLogger::getLogger()->logInfo("OSC: loaded new path -> " + audioPath);
        // Play/pause
        } else if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/play") ) {
            CuemsLogger::getLogger()->logInfo("OSC: /play command");
            if ( playheadControl != 0 )
                playheadControl = 0;
            else 
                playheadControl = 1;
        // Stop
        } else if ( (string) m.AddressPattern() == (OscReceiver::oscAddress + "/stop") ) {
            // TO DO : right now is the same as play/pause... Don't know if there 
            //          will be other implementations of the command...
            CuemsLogger::getLogger()->logInfo("OSC: /stop command");
            if ( playheadControl != 0 )
                playheadControl = 0;
            else 
                playheadControl = 1;
        // Quit
        } else if ( (string)m.AddressPattern() == (OscReceiver::oscAddress + "/quit") ) {
            CuemsLogger::getLogger()->logInfo("OSC: /quit command");
            raise(SIGTERM);
        // Check
        } else if ( (string)m.AddressPattern() == (OscReceiver::oscAddress + "/check") ) {
            CuemsLogger::getLogger()->logInfo("OSC: /check command");
            raise(SIGUSR1);
        // Stop on lost
        } else if ( (string)m.AddressPattern() == (OscReceiver::oscAddress + "/stoponlost") ) {
            CuemsLogger::getLogger()->logInfo("OSC: /stoponlost command");
            stopOnMTCLost = !stopOnMTCLost;
        // MTC Follow
        } else if ( (string)m.AddressPattern() == (OscReceiver::oscAddress + "/mtcfollow") ) {
            CuemsLogger::getLogger()->logInfo("OSC: /mtcfollow command");
            followingMtc = !followingMtc;
        }
        
    } catch ( osc::Exception& error ) {
        // any parsing errors such as unexpected argument types, or 
        // missing arguments get thrown as exceptions.
        // std::cerr << "Error while parsing OSC message: "
        //     << m.AddressPattern() << ": " << error.what() << "\n";
        CuemsLogger::getLogger()->logError(  "OSC ERR : " + 
                                        (std::string) m.AddressPattern() + 
                                        ": " + (std::string) error.what() );
    }
}

//////////////////////////////////////////////////////////
/*
bool AudioPlayer::loadNodeConfig( void ) {
    cerr << "Node config read!" << endl;
    return true;
}
*/

//////////////////////////////////////////////////////////
/*
bool AudioPlayer::loadMediaConfig( void ) {
    cerr << "Media config read!" << endl;
    return true;
}
*/

