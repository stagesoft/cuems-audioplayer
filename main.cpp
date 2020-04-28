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
// Stage Lab SysQ audio player main source code file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "main.h"

using namespace std;

//////////////////////////////////////////////////////////
// Initializing static class members
// unsigned short MtcMaster::instanceCount = 0; // Instance counter
// std::mutex MtcMaster::mtx;
// bool MtcMaster::playing = false;

//////////////////////////////////////////////////////////
// Main application function
int main( int argc, char *argv[] ) {

    AudioPlayer* myAudioPlayer;

    //////////////////////////////////////////////////////////
    // Parse command line
    if ( argc == 3 && !strcmp( argv[1], "show" ) ) {
        showcopyright();
        if ( !strcmp( argv[2], "w") ) showwarrantydisclaimer();
        else if ( !strcmp( argv[2], "c") ) showcopydisclaimer();

        exit( EXIT_FAILURE );
    }

    if ( argc < 3 ) {
        showcopyright();        
        std::cout << "Usage :    audioplayer <osc_port> <osc_path> <wav_file_path>" << endl <<
            "           Default audio params are : 2 ch x 44.1K -> default device" << endl;

        exit( EXIT_FAILURE );
    }
    else if ( argc < 4 ) {
        // Creating our audio player without audio file spec
        myAudioPlayer = new AudioPlayer( atoi(argv[1]), argv[2] );

    }
    else if ( argc == 4 ) {
        // Creating our audio player with audio file spec
        myAudioPlayer = new AudioPlayer( atoi(argv[1]), argv[2], argv[3] );
    }
    else if ( argc > 4 ) {
        showcopyright();        
        std::cout << "Usage :    audioplayer <osc_port> <osc_path> <wav_file_path>" << endl <<
            "           Default audio params are : 2 ch x 44.1K -> default device" << endl;

        exit( EXIT_FAILURE );
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "Starting object with " << myAudioPlayer->nChannels << " channels" <<
        " at " << myAudioPlayer->sampleRate << " samples/sec" <<
        " on device number " << myAudioPlayer->deviceID << endl;

    std::cout << "- Osc path : " << myAudioPlayer->getOscAddress() << endl ;
    std::cout << "- Audio File : " << myAudioPlayer->audioPath << endl << endl;

    //////////////////////////////////////////////////////////
    // Clear screen
    // cout << "\033[2J";

    //////////////////////////////////////////////////////////
    // Wait for it to finnish somehow
    while ( !myAudioPlayer->endOfStream ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

}

//////////////////////////////////////////////////////////
void showcopyright( void ) {
    std::cout << "audioplayer - Copyright (C) 2020 Stage Lab & bTactic" << endl <<
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