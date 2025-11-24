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

// This file contains the testable functions from main.cpp
// We extract them here to avoid linking the main() function which conflicts with gtest_main

#include "main.h"
#include <iostream>

using namespace std;

//////////////////////////////////////////////////////////
void showcopyright( void ) {
    std::cout << "audioplayer-cuems v. " << 
        cuems_audioplayer_VERSION_MAJOR << "." << cuems_audioplayer_VERSION_MINOR <<
        " - Copyright (C) 2020 Stage Lab & bTactic" << endl <<
        "This program comes with ABSOLUTELY NO WARRANTY; for details type `show w'." << endl <<
        "This is free software, and you are welcome to redistribute it" << endl <<
        "under certain conditions; type `show c' for details." << endl << endl;

}

//////////////////////////////////////////////////////////
void showwarrantydisclaimer( void ) {
    std::cout << "Warranty disclaimer : " << endl << endl <<
        "THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY" << endl <<
        "APPLICABLE LAW.  EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT" << endl <<
        "HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM \"AS IS\" WITHOUT WARRANTY" << endl <<
        "OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO," << endl <<
        "THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR" << endl <<
        "PURPOSE.  THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM" << endl <<
        "IS WITH YOU.  SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF" << endl <<
        "ALL NECESSARY SERVICING, REPAIR OR CORRECTION." << endl << endl;

}

//////////////////////////////////////////////////////////
void showcopydisclaimer( void ) {
    std::cout << "Copyright disclaimer : " << endl << endl <<
        "This program is free software: you can redistribute it and/or modify" << endl <<
        "it under the terms of the GNU General Public License as published by" << endl <<
        "the Free Software Foundation, either version 3 of the License, or" << endl <<
        "(at your option) any later version." << endl << endl <<
        "This program is distributed in the hope that it will be useful," << endl <<
        "but WITHOUT ANY WARRANTY; without even the implied warranty of" << endl <<
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the" << endl <<
        "GNU General Public License for more details." << endl << endl << 
        "You should have received a copy of the GNU General Public License" << endl <<
        "along with this program.  If not, see <https://www.gnu.org/licenses/>." << endl << endl;
}

//////////////////////////////////////////////////////////
void showusage( void ) {
    std::cout << "Usage :    audioplayer-cuems --port <osc_port> [other options] <wav_file_path>" << endl << endl <<
        "           COMPULSORY OPTIONS:" << endl << 
        "           --file , -f <file_path> : wav file to read audio data from." << endl <<
        "               File name can also be stated as the last argument with no option indicator." << endl << endl <<
        "           --port , -p <port_number> : OSC port to listen to." << endl << endl <<
        "           OPTIONAL OPTIONS:" << endl << 
        "           --ciml , -c : Continue If Mtc is Lost, flag to define that the player should continue" << endl <<
        "               if the MTC sync signal is lost. If not specified (standard mode) it stops on lost." << endl << endl <<
        "           --device , -d : Audio device name to connect the player to. If not stated it will" << endl <<
        "               try to connect to the default device." << endl << endl <<
        "           --mtcfollow , -m : Start the player following MTC directly. Default is not to follow until" << endl <<
        "               it is indicated to the player through OSC." << endl << endl <<
        "           --offset , -o <milliseconds> : playing time offset in milliseconds." << endl <<
        "               Positive (+) or (-) negative integer indicating time displacement." << endl <<
        "               Default is 0." << endl << endl <<
        "           --resample-quality , -r <quality> : resampling quality when file sample rate differs from" << endl <<
        "               JACK sample rate. Options: vhq (very high), hq (high, default), mq (medium), lq (low)." << endl <<
        "               Higher quality = better audio but more CPU usage. Default is 'hq'." << endl << endl <<
        "           --uuid , -u <uuid_string> : indicates a unique identifier for the process to be recognized" << endl <<
        "               in different internal identification porpouses such as Jack streams in use." << endl << endl <<
        "           --wait , -w <milliseconds> : waiting time after reaching the end of the file and before" << endl <<
        "               quiting the program. Default is 0. -1 indicates the program remains" << endl <<
        "               running till SIG-TERM or OSC quit is received." << endl << endl <<
        "           OTHER OPTIONS:" << endl << endl <<
        "           --show : shows license disclaimers." << endl <<
        "               w : shows warranty disclaimer." << endl << 
        "               c : shows copyright disclaimer." << endl << endl << 
        "           Default audio device params are : 2 ch x 44.1K -> default device." << endl <<
        "           audioplayer-cuems uses Jack Audio environment, make sure it's running." << endl << endl;
}

