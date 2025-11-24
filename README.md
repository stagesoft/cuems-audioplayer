# cuems-audioplayer

Cuems system audio player.

    Still under development.

Audio player for the Cuems stage system using RtAudio and RtMidi libraries for audio and midi purposes and oscpack for OSC communication.
It supports various audio formats (WAV, MP3, AAC, FLAC, OGG, etc.) and can extract and play audio from video files (MP4, AVI, MKV, MOV, etc.) via the cuems-mediadecoder module (FFmpeg-based).

## Requirements

- CMake : cmake v. 3.16.3 (minimum v. 3.10)

- RtMidi : librtmidi-dev v. 3.0.0

- RtAudio : librtaudio-dev v. 5.0.0

- OSCPack : liboscpack-dev v. 1.1.0

- FFmpeg libraries : libavformat, libavcodec, libavutil, libswresample (provided via cuems-mediadecoder)

- libsoxr : libsoxr-dev (for audio resampling)

## Build instructions

First, initialize git submodules (includes cuems-mediadecoder):

    git submodule update --init --recursive

Then run:

    cmake -S src/ -B build/

to get a standard _Release_ configuration
 
    cmake -S src/ -B build/ -DCMAKE_BUILD_TYPE=Debug

to get the _Debug_ configuration. And then:

    cd build
    make

## Usage

    audioplayer-cuems v. 0.0 - Copyright (C) 2020 Stage Lab & bTactic
    This program comes with ABSOLUTELY NO WARRANTY; for details type `show w'.
    This is free software, and you are welcome to redistribute it
    under certain conditions; type `show c' for details.

    Usage :    audioplayer-cuems --port <osc_port> [other options] <media_file_path>

           COMPULSORY OPTIONS:
           --file , -f <file_path> : media file to read audio data from.
               Supports audio files (WAV, MP3, AAC, FLAC, OGG, etc.) and video files (MP4, AVI, MKV, MOV, etc.).
               For video files, the audio track will be extracted and played.
               File name can also be stated as the last argument with no option indicator.

           --port , -p <port_number> : OSC port to listen to.

           OPTIONAL OPTIONS:
           --ciml , -c : Continue If Mtc is Lost, flag to define that the player should continue
               if the MTC sync signal is lost. If not specified (standard mode) it stops on lost.

           --offset , -o <milliseconds> : playing time offset in milliseconds.
               Positive (+) or (-) negative integer indicating time displacement.
               Default is 0.

           --uuid , -u <uuid_string> : indicates a unique identifier for the process to be recognized
               in different internal identification porpouses such as Jack streams in use.

           --wait , -w <milliseconds> : waiting time after reaching the end of the file and before
               quiting the program. Default is 0. -1 indicates the program remains
               running till SIG-TERM or OSC quit is received.

           OTHER OPTIONS:

           --show : shows license disclaimers.
               w : shows warranty disclaimer.
               c : shows copyright disclaimer.

           Default audio device params are : 2 ch x 44.1K -> default device.
           audioplayer-cuems uses Jack Audio environment, make sure it's running.
