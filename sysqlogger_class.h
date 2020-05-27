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
// Stage Lab SysQ logger class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#ifndef SYSQLOGGER_CLASS_H
#define SYSQLOGGER_CLASS_H

#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <ctime>

using namespace std;
namespace fs = std::filesystem;

class SysQLogger
{
    public:
        SysQLogger( const string file, const string slug );
        ~SysQLogger( void );

        void logOK( const string& message );
        void logER( const string& message );
        void logWA( const string& message );
        void logIN( const string& message );

        // Static funcion to get our singleton pointer everywhere
        static SysQLogger* getLogger( void );

        void setNewSlug( const string newSlug );
        string getSlug( void );
        bool isLogOpen( void );

    private:
        // Log file path
        static fs::path logPath;
        static std::string programSlug;

        // Our own singleton pointer
        static SysQLogger* myObjectPointer;

        SysQLogger(const SysQLogger&) {};
        SysQLogger& operator=(const SysQLogger&){ return *this; };
        
        void log( const string& message );

        static ofstream logFile;           // File stream

        // Utility to get date and time string
        const std::string curDateTime( void);

};

#endif // SYSQLOGGER_CLASS_H