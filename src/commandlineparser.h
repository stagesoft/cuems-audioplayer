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
// Stage Lab Cuems command line parser class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
#ifndef COMMANDLINEPARSER_H
#define COMMANDLINEPARSER_H

#include <string>
#include <vector>
#include <algorithm>
#include <experimental/filesystem>

using namespace std;
namespace fs = experimental::filesystem;

//////////////////////////////////////////////////////////
class CommandLineParser
{
    public:
        CommandLineParser (int &argc, char **argv);
        ~CommandLineParser ( void );

        const std::string getParam( const std::string &option ) const;
        const std::string getEndingFilename( void );
        bool optionExists( const std::string &option ) const;

    private:
        std::vector <std::string> args;
};

#endif // COMMANDLINEPARSER_H
