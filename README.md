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

## Generating Test Files

### Audio Test Files

Generate comprehensive audio test files from source MP3s in the `audio/` directory:

    ./generate_test_files.sh

This creates test files in `test_audio_files/` with various formats (WAV, AIFF, MP3, FLAC, OGG, OPUS, AAC, etc.) and sample rates.

### Video Test Files

There are two ways to get video test files:

**Option 1: Download from legal sources**

Download test videos with audio from Creative Commons and public domain sources:

    ./download_test_videos.sh [output_directory] [max_parallel]

This downloads videos like Big Buck Bunny (Creative Commons) and sample test files in various formats (MP4, MOV, AVI, MKV) from legal sources. By default, videos are saved to `test_video_files/` directory.

Downloads run in parallel (default: 10 concurrent downloads) to maximize bandwidth usage. You can adjust the parallelism with the second parameter:

    ./download_test_videos.sh test_video_files 20  # Use 20 parallel downloads

**Option 2: Generate with embedded audio**

Generate video test files with embedded audio tracks from your audio files:

    ./generate_test_videos.sh [output_directory] [duration]

This creates video files in various formats (MP4, MOV, AVI, MKV) with different codecs (H.264, HEVC, AV1, HAP, MPEG-4) and audio sample rates (44.1kHz, 48kHz). The script uses the same formats as cuems-videocomposer but adds audio tracks from the `audio/` directory.

By default, videos are saved to `test_video_files/` directory.

Examples:

    ./download_test_videos.sh test_video_files
    ./generate_test_videos.sh test_video_files 30

To remove every generated artifact (equivalent to `git clean -xdf` for the build
outputs), you can run:

    cmake --build build --target distclean

This keeps the `build/` directory itself but removes all compiled binaries,
intermediate files, and cached CTest/MTC logs so you can start from a pristine
state.

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
