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
// Stage Lab Cuems audio player class code file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "audioplayer.h"
#include <time.h>
#include <unistd.h>
#include "testknobs.h"

////////////////////////////////////////////
// Initializing static class members

std::atomic <bool> AudioPlayer::endOfPlay = false;
std::atomic <bool> AudioPlayer::quitRequested = false;

namespace {
int64_t monoNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
void writeLine(const std::string& s)
{
    // one write(2), so a line can never interleave with another thread's output
    ssize_t r = ::write(STDOUT_FILENO, s.data(), s.size());
    (void)r;
}
}  // namespace

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
                            const string &resampleQuality,
                            long explicitLatencyMs )
                            :   // Members initialization
                            OscReceiver(port, oscRoute.c_str()),
                            audioPath(filePath),
                            nChannels(numberOfChannels),
                            sampleRate(sRate),
                            deviceName(deviceName),
                            audio(audioApi),
                            mtcReceiver(MTCRECV_DEFAULT_API, client_name),
                            audioFile(filePath.c_str()),  // Open file to check format
                            endWaitTime(finalWait),
                            stopOnMTCLost(stopOnLostFlag),
                            m_explicitLatencyMs(explicitLatencyMs)
 {
    // Enable network-tolerant MTC timeouts (for rtpmidid / MTC over network)
    mtcReceiver.setNetworkMode(true);

    // --resample-quality is kept for the CLI: libsoxr 0.1.3 always uses vr32
    // in variable-ratio mode (section 4.3b)
    audioFile.setResampleQuality(resampleQuality);
    if (resampleQuality != "hq") {
        CuemsLogger::getLogger()->logInfo("--resample-quality " + resampleQuality +
                                          " has no effect: the variable-ratio resampler is always vr32");
    }

    // Per channel volume param to process audio
    volumeMaster = new float[nChannels > 8 ? nChannels : 8];
    for ( unsigned short int i = 0; i < (nChannels > 8 ? nChannels : 8); i++ ) {
        volumeMaster[i] = 1.0;
    }

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

    // Never let RtAudio auto-connect our outports to system:playback_* when the
    // stream starts. The engine always wires this player to the mixer
    // (0_mixer); an auto-connect that lands after the engine's wiring stays
    // and the cue sounds through the mixer AND straight to the outputs, doubled
    // and outside the mixer's volume and mute (ClickUp 869fcvz85). The engine
    // also removes such edges now, but an old engine would not.
    // Needs RtAudio >= 5.1 (5.2.0 on casas and the deployed nodes). A player
    // started by hand, outside the engine, is therefore not audible until
    // something connects it (jack_connect).
    streamOps.flags = RTAUDIO_JACK_DONT_CONNECT;


    // Everything that allocates or fixes a size happens BEFORE startStream
    // (G19): the stream's period and rate, the latency, the final open() of
    // the file with its output channel count, the head cache, the VR soxr and
    // every buffer, the core.
    try {
        // For JACK/pw-jack: Query the device's native sample rate to avoid mismatch
        unsigned int requestedRate = sampleRate;
        try {
            RtAudio::DeviceInfo deviceInfo = audio.getDeviceInfo(audioDeviceId);
            if (deviceInfo.probed && deviceInfo.sampleRates.size() > 0) {
                requestedRate = deviceInfo.sampleRates[0];
                CuemsLogger::getLogger()->logInfo("Using device native sample rate: " +
                    std::to_string(requestedRate) + " Hz");
            }
        } catch (...) {
            CuemsLogger::getLogger()->logInfo("Could not query device sample rate, using requested: " +
                std::to_string(requestedRate) + " Hz");
        }

        audio.openStream(  &streamParams,
                            NULL,
                            RTAUDIO_FLOAT32,  // JACK's native format
                            requestedRate,
                            &bufferFrames,
                            &audioCallback,
                            (void *) this,
                            &streamOps );

        // JACK is the master: its rate and its period
        unsigned int jackSampleRate = audio.getStreamSampleRate();
        if (jackSampleRate != 0) {
            CuemsLogger::getLogger()->logInfo("JACK server sample rate: " + std::to_string(jackSampleRate) + " Hz");
            sampleRate = jackSampleRate;
        }

        // The output latency, kept in frames (section 4.2: not truncated to ms)
        if (m_explicitLatencyMs >= 0) {
            long clamped = m_explicitLatencyMs > 500 ? 500 : m_explicitLatencyMs;
            outputLatencyFrames_.store(clamped * (long)sampleRate / 1000);
            CuemsLogger::getLogger()->logInfo("Audio output latency explicitly set to " + std::to_string(clamped) +
                                              " ms (CLI override; JACK query skipped)");
        } else {
            long latencyFrames = audio.getStreamLatency();
            if (latencyFrames < 0) latencyFrames = 0;
            outputLatencyFrames_.store(latencyFrames);
            CuemsLogger::getLogger()->logInfo("JACK output latency: " + std::to_string(latencyFrames) + " frames @ " +
                                              std::to_string(sampleRate) + " Hz");
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
            // File has fewer channels than the JACK stream we already opened.
            // We can't shrink the JACK port count after openStream(), so the
            // file must be upmixed to match. Reopen with setTargetChannels()
            // so swresample fills every JACK channel (mono → L=R, etc.).
            // Without this the audioCallback writes fileChannels-worth of
            // floats into an nChannels-interleaved buffer, and JACK plays
            // them as nChannels/fileChannels× too fast (e.g. mono into
            // stereo plays at 2× speed with stereo aliasing).
            std::cerr << "File has " << fileChannels << " channels, upmixing to "
                      << nChannels << " channels to match JACK stream" << endl;
            CuemsLogger::getLogger()->logInfo("Upmixing " + std::to_string(fileChannels) +
                                              " channels to " + std::to_string(nChannels) +
                                              " channels to match JACK stream");

            audioFile.close();
            audioFile.setTargetChannels(nChannels);
            audioFile.open(audioPath, ios::binary | ios::in);

            if (!audioFile.good()) {
                std::string str = "Error reopening audio file with upmixing: " + audioPath;
                std::cerr << str << endl;
                CuemsLogger::getLogger()->logError(str);
                exit(CUEMS_EXIT_AUDIO_DEVICE_ERR);
            }
            // nChannels stays at the JACK stream's value; the file now delivers
            // nChannels-interleaved frames so audioFrameSize remains valid.
        } else {
            // Device has enough channels, use file's native channel count
            std::cerr << "Playing " << fileChannels << " channels (device supports " 
                      << deviceChannels << " channels)" << endl;
            CuemsLogger::getLogger()->logInfo("Playing " + std::to_string(fileChannels) + 
                                              " channels (device supports " + std::to_string(deviceChannels) + ")");
            nChannels = fileChannels;
        }
        
        // The feed engine: head cache, VR soxr, worker (E1)
        if (!audioFile.prepareFeed(sampleRate, bufferFrames, true)) {
            std::string str = "Error preparing the audio feed: " + audioPath;
            std::cerr << str << endl;
            CuemsLogger::getLogger()->logError(str);
            exit(CUEMS_EXIT_AUDIO_DEVICE_ERR);
        }
        core.reset(new PlayerCore(audioFile, sampleRate, bufferFrames));
        core->setOffsetMs((double)initOffset);
        core->setLatencyNs(latencyNs());
        core->setFollowing(mtcFollowFlag);
        core->setEndWaitMs(endWaitTime);
#ifdef CUEMS_TEST_KNOBS
        if (testknobs::knobs().failWrap) audioFile.failNextJob(AudioFstream::JobKind::WRAP);
#endif

        // The trusted-tick seqlock's writer: mtcreceiver's tick callback (the MIDI thread)
        MtcReceiver::setTickCallback([this](long headMs, bool complete) {
            const int64_t now = monoNs();
            ticks.onTick(headMs, complete, now);
#ifdef CUEMS_TEST_KNOBS
            if (testknobs::knobs().f) {
                TickSample t;
                ticks.read(t);
                testknobs::knobs().ticks.push({now, headMs, complete ? 1 : 0, (int)t.run, t.anchored ? 1 : 0});
            }
#endif
        });

        audio.startStream();

        // The latency again, now that the ports run (it is fixed per show)
        if (m_explicitLatencyMs < 0) {
            long latencyFrames = audio.getStreamLatency();
            if (latencyFrames > 0 && latencyFrames != outputLatencyFrames_.load()) {
                outputLatencyFrames_.store(latencyFrames);
                core->setLatencyNs(latencyNs());
                CuemsLogger::getLogger()->logInfo("JACK output latency after start: " + std::to_string(latencyFrames) + " frames");
            }
        }
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
    MtcReceiver::setTickCallback(nullptr);
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
    // E19: the worker is joined before the file closes (AudioFstream::close)
    audioFile.close();

    // Delete dinamically reserved members
    delete []volumeMaster;
}

int64_t AudioPlayer::latencyNs( void ) const {
    return sampleRate ? (int64_t)outputLatencyFrames_.load() * 1000000000LL / sampleRate : 0;
}

//////////////////////////////////////////////////////////
// RtAudio callback - Static member function
int AudioPlayer::audioCallback( void *outputBuffer, void * /*inputBuffer*/, unsigned int nBufferFrames,
            double /*streamTime*/, RtAudioStreamStatus /*status*/, void *data ) {

    AudioPlayer *ap = (AudioPlayer*) data;
    float* out = (float*)outputBuffer;
    const int64_t now = monoNs();

    if (!ap->core) {
        memset(out, 0, nBufferFrames * ap->nChannels * sizeof(float));
        return 0;
    }
    // the seqlock: at most 3 retries, otherwise the previous sample
    ap->ticks.read(ap->lastTick_);
    ap->core->callback(out, nBufferFrames, now, MtcReceiver::mtcHead.load(), ap->lastTick_);

    // Apply volume to each sample
    const unsigned ch = ap->nChannels;
    for ( unsigned int i = 0; i < nBufferFrames * ch; i++ ) {
        out[i] *= ap->volumeMaster[i % ch];
    }

#ifdef CUEMS_TEST_KNOBS
    testknobs::knobs().dumpPush(out, (size_t)nBufferFrames * ch);
    if (testknobs::knobs().f) {
        const lockloop::Mtc m = ap->core->mtc();
        testknobs::knobs().periods.push({now, (int64_t)(ap->core->target()), 0, (int)m, ap->audioFile.pos(),
                                         ap->core->loop().ef(), ap->core->loop().u(), ap->audioFile.fifo(),
                                         (monoNs() - now) / 1000, (int)ap->audioFile.feed(),
                                         ap->audioFile.feedPassIndex(), ap->audioFile.cursor(),
                                         ap->audioFile.inputConsumed(),
                                         (int64_t)ap->audioFile.lastPeriodProduced()});
    }
#endif

    if (ap->core->endOfPlay()) {
        ap->endOfPlay = true;
        return 1;   // as today: the -w time is over, end the stream
    }
    return 0;
}

//////////////////////////////////////////////////////////
// Main loop: the journal ring and the mode-change ack lines (section 4.3e)
void AudioPlayer::drainJournal( void ) {
    if (!core) return;
    LogEvent e;
    while (core->popLog(e)) {
        bool toStdout = false;
        int level = 6;
        const std::string line = PlayerCore::describe(e, slug, &toStdout, &level);
        if (toStdout) {
            writeLine(line);
            continue;
        }
        switch (level) {
        case 3: CuemsLogger::getLogger()->logError(line); break;
        case 4: CuemsLogger::getLogger()->logWarning(line); break;
        case 7: CuemsLogger::getLogger()->logDebug(line); break;
        default: CuemsLogger::getLogger()->logInfo(line); break;
        }
    }
#ifdef CUEMS_TEST_KNOBS
    static int64_t lastFlush = 0;
    const int64_t now = monoNs();
    if (now - lastFlush >= 1000000000LL) {
        lastFlush = now;
        testknobs::knobs().flush();
    }
#endif
}

////////////////////////////////////////////
// OSC process message callback (the OSC thread)
// Every event the audio thread acts on is latched and applied at the top of
// the next callback in arrival order (section 4.3h, h.1).
void AudioPlayer::ProcessMessage( const osc::ReceivedMessage& m, 
            const IpEndpointName& /*remoteEndpoint*/ )
{
    try{
        const string addr = (string) m.AddressPattern();
        auto is = [&](const char* a) { return addr == (OscReceiver::oscAddress + a); };
        auto intArg = [](const osc::ReceivedMessageArgument& a, int32_t& v) -> bool {
            if (a.IsInt32()) { v = a.AsInt32(); return true; }
            if (a.IsFloat()) { v = (int32_t)std::floor(a.AsFloat()); return true; }
            if (a.IsInt64()) { v = (int32_t)a.AsInt64(); return true; }
            return false;
        };
        // Volume channel 0
        if ( is("/vol0") ) {
            m.ArgumentStream() >> volumeMaster[0] >> osc::EndMessage;
            CuemsLogger::getLogger()->logInfo("OSC: new volume channel 0 " + std::to_string(volumeMaster[0]));
        // Volume channel 1
        } else if ( is("/vol1") ) {
            m.ArgumentStream() >> volumeMaster[1] >> osc::EndMessage;
            CuemsLogger::getLogger()->logInfo("OSC: new volume channel 1 " + std::to_string(volumeMaster[1]));
        // Volume master
        } else if ( is("/volmaster") ) {
            float v;
            m.ArgumentStream() >> v >> osc::EndMessage;
            for (unsigned i = 0; i < nChannels; i++) volumeMaster[i] = v;
            CuemsLogger::getLogger()->logInfo("OSC: new volume master " + std::to_string(v));
        // Offset (float ms, floored as today)
        } else if ( is("/offset") ) {
            float offsetOSC;
            m.ArgumentStream() >> offsetOSC >> osc::EndMessage;
            OscEvent e;
            e.type = OscEvent::OFFSET_F;
            e.f = offsetOSC;
            core->latch(e);
            CuemsLogger::getLogger()->logInfo("OSC: new offset value " + std::to_string((long int)floor(offsetOSC)));
        // Cue start as MTC position modulo 24 h (day alignment §7.2; it replaces
        // /offset_i): normalised into [0, DAY) with a WARNING, never rejected
        } else if ( is("/start_tod") ) {
            auto it = m.ArgumentsBegin();
            if (it == m.ArgumentsEnd()) {
                CuemsLogger::getLogger()->logError("OSC: /start_tod without a value: ignored");
                return;
            }
            bool changed = false;
            int64_t v = 0;
            if (it->IsInt32()) v = dayalign::normalizeTod(it->AsInt32(), &changed);
            else if (it->IsInt64()) v = dayalign::normalizeTod(it->AsInt64(), &changed);
            else if (it->IsFloat()) v = dayalign::todFromFloat(it->AsFloat(), &changed);
            else if (it->IsDouble()) v = dayalign::todFromFloat(it->AsDouble(), &changed);
            else {
                CuemsLogger::getLogger()->logError("OSC: /start_tod with a non-numeric value: ignored");
                return;
            }
            if (changed)
                CuemsLogger::getLogger()->logWarning("OSC: /start_tod value out of [0, 86400000): normalised to "
                                                     + std::to_string(v));
            OscEvent e;
            e.type = OscEvent::START_TOD;
            e.a = (int32_t)v;
            core->latch(e);
            CuemsLogger::getLogger()->logInfo("OSC: new start_tod value " + std::to_string(v));
        // /offset_i is gone (day alignment §7.2, packages installed together):
        // loud, and the value is ignored
        } else if ( is("/offset_i") ) {
            CuemsLogger::getLogger()->logError("/offset_i received: engine and player versions do not match");
        // Loop: i:passes [i:period_num i:period_den], parsed leniently (section 4.2)
        } else if ( is("/loop") ) {
#ifdef CUEMS_TEST_KNOBS
            if (testknobs::knobs().loopsDropped.load() < testknobs::knobs().dropLoop) {
                testknobs::knobs().loopsDropped.fetch_add(1);
                CuemsLogger::getLogger()->logWarning("HF_DROP_LOOP: /loop dropped");
                return;
            }
#endif
            OscEvent e;
            e.type = OscEvent::LOOP;
            int32_t v[3] = {1, 0, 0};
            int n = 0;
            for (auto it = m.ArgumentsBegin(); it != m.ArgumentsEnd() && n < 3; ++it) {
                if (!intArg(*it, v[n])) break;
                n++;
            }
            if (n == 0) n = 1;
            e.a = v[0]; e.b = v[1]; e.c = v[2]; e.nargs = n;
            core->latch(e);
            // the receipt ack: written here (not a real-time thread), with the
            // PREVIOUS mode and its version (L6)
            writeLine(core->loopAckLine(slug, v[0], n >= 3 ? v[1] : 0, n >= 3 ? v[2] : 0));
        // Wait
        } else if ( is("/wait") ) {
            float waitOSC;
            m.ArgumentStream() >> waitOSC >> osc::EndMessage;
            waitOSC = floor(waitOSC);
            CuemsLogger::getLogger()->logInfo("OSC: new end wait value " + std::to_string((long int)waitOSC));
            endWaitTime = waitOSC;             // In milliseconds
            core->setEndWaitMs(endWaitTime);
        // Load: refused while a stream is open (K4, E20)
        } else if ( is("/load") ) {
            CuemsLogger::getLogger()->logError("OSC: /load refused while a stream is open");
        // Play/pause, stop (E25: refused, WARNING once)
        } else if ( is("/play") || is("/stop") ) {
            OscEvent e;
            e.type = OscEvent::PLAY_STOP;
            core->latch(e);
        // Quit: a flag the main loop acts on (I16e, J16)
        } else if ( is("/quit") ) {
            CuemsLogger::getLogger()->logInfo("OSC: /quit command");
            quitRequested = true;
        // Check
        } else if ( is("/check") ) {
            CuemsLogger::getLogger()->logInfo("OSC: /check command");
            raise(SIGUSR1);
        // Stop on lost (E26: ignored in this release, WARNING once)
        } else if ( is("/stoponlost") ) {
            int32_t valueOSC = 0;
            m.ArgumentStream() >> valueOSC >> osc::EndMessage;
            OscEvent e;
            e.type = OscEvent::STOPONLOST;
            e.a = valueOSC;
            core->latch(e);
        // MTC Follow - value: 0 = don't follow MTC, non-zero = follow MTC
        } else if ( is("/mtcfollow") ) {
            int32_t valueOSC;
            m.ArgumentStream() >> valueOSC >> osc::EndMessage;
            OscEvent e;
            e.type = OscEvent::MTCFOLLOW;
            e.a = valueOSC;
            core->latch(e);
            CuemsLogger::getLogger()->logInfo("OSC: /mtcfollow set to " + std::to_string(valueOSC != 0));
        }
        
    } catch ( osc::Exception& error ) {
        CuemsLogger::getLogger()->logError(  "OSC ERR : " + 
                                        (std::string) m.AddressPattern() + 
                                        ": " + (std::string) error.what() );
    }
}

//////////////////////////////////////////////////////////
void AudioPlayer::setOutputLatencyMs(long ms) {
    if (ms < 0) ms = 0;
    if (ms > 500) ms = 500;
    outputLatencyFrames_.store(ms * (long)sampleRate / 1000);
    if (core) core->setLatencyNs(latencyNs());
    CuemsLogger::getLogger()->logInfo(
        "Audio output latency compensation updated to "
        + std::to_string(ms) + " ms");
}
