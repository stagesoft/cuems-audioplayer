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

    AudioPlayer* myAudioPlayer = NULL;

    //////////////////////////////////////////////////////////
    // Parse command line
    if ( argc < 2 ) {
        showcopyright();        
        showusage();
        
        exit( EXIT_FAILURE );
    }

    // If there is at least one we parse the command line
    CommandLineParser argParser(argc, argv);

    // --show or -s command parse
    if ( argParser.optionExists("--show") ) {
        std::string arg = argParser.getParam("--show");

        showcopyright();

        if ( arg.empty() ) {
            showusage();
        } else if ( arg == "w" ) {
            showwarrantydisclaimer();
        } else if ( arg == "c" ) {
            showcopydisclaimer();
        }

        exit( EXIT_FAILURE );
    }

    // --file or -f command parse and filename retreival and check
    std::filesystem::path filePath;

    if ( argParser.optionExists("--file") || argParser.optionExists("-f") ) {
        filePath = argParser.getParam("--file");

        if ( filePath.empty() ) filePath = argParser.getParam("-f");

        if ( filePath.empty() ) {
            // Not file path specified after file option
            std::cout << "File not specified after --file or -f option." << endl;

            exit( EXIT_FAILURE );
        }
    }

    if ( !filePath.empty() && filePath.is_relative() ) {
        filePath = std::filesystem::absolute( filePath );

        if ( !std::filesystem::exists(filePath) ) {
            // File does not exist
            std::cout << "Unable to locate file: " << filePath << endl;

            exit( EXIT_FAILURE );
        }
    }

    // --port or -p command parse and port number retreival and check
    unsigned int portNumber = 0;

    if ( argParser.optionExists("--port") || argParser.optionExists("-p") ) {
        std::string portParam = argParser.getParam("--port");

        if ( portParam.empty() ) portParam = argParser.getParam("-p");

        if ( portParam.empty() ) {
            // Not valid port number specified after port option
            std::cout << "Not valid port number after --port or -p option." << endl;

            exit( EXIT_FAILURE );
        }
        else {
            portNumber = std::stoi( portParam );
        }
    }

    // End of command line parsing
    //////////////////////////////////////////////////////////



    if ( filePath.empty() || portNumber == 0 ) {
        std::cout << "Wrong parameters! Check usage..." << endl << endl;
        showcopyright();
        showusage();

        exit ( EXIT_FAILURE );
    }
    else {
        myAudioPlayer = new AudioPlayer( portNumber, "", filePath.c_str() );
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

//////////////////////////////////////////////////////////
void showusage( void ) {
    std::cout << "Usage :    audioplayer --port <osc_port> --file <wav_file_path>" << endl <<
        "           --port , -p : OSC port to listen to." << endl <<
        "           --file , -f : xml file to read DMX scenes from." << endl <<
        "               If not stated, default XML file is ./dmx.xml" << endl <<
        "           --show : shows license disclaimers" << endl <<
        "               w : shows warranty disclaimer" << endl << 
        "               c : shows copyright disclaimer" << endl << endl << 
        "           Default audio params are : 2 ch x 44.1K -> default device" << endl << endl <<
        "           audioplayer uses Jack Audio environment, make sure it's running." << endl;
}
