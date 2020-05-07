/* LICENSE TEXT

    DmxPlayer for linux based using RtAudio and RtMidi libraries to
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
// Stage Lab SysQ command line parser class source file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "commandlineparser_class.h"

//////////////////////////////////////////////////////////
CommandLineParser::CommandLineParser (int &argc, char **argv) {
    for ( int i = 1; i < argc; ++i ) {
        this->args.push_back( std::string(argv[i]) );
    }
}

//////////////////////////////////////////////////////////
CommandLineParser::~CommandLineParser ( void ) {
}


//////////////////////////////////////////////////////////
const std::string& CommandLineParser::getParam( const std::string &option ) const {
    std::vector<std::string>::const_iterator itr;
    
    itr =  std::find( this->args.begin(), this->args.end(), option );

    if ( itr != this->args.end() && ++itr != this->args.end()){
        return *itr;
    }

    static const std::string empty_string("");
    return empty_string;
}

//////////////////////////////////////////////////////////
bool CommandLineParser::optionExists( const std::string &option ) const {
    return std::find(this->args.begin(), this->args.end(), option) 
        != this->args.end();
}
