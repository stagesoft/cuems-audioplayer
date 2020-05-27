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
// Stage Lab SysQ logger class source file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "sysqlogger_class.h"

// Static class members initialize
SysQLogger* SysQLogger::myObjectPointer = NULL;
fs::path SysQLogger::logPath;
std::string SysQLogger::programSlug;
ofstream SysQLogger::logFile;

//////////////////////////////////////////////////////////
SysQLogger::SysQLogger (    const string file = "~/sysq/log/sysqlogger.log", 
                            const string slug = "sysqlogger" )
{
    // Reinitialize our static log file data
    logPath = file;
    programSlug = slug;

    // Open the file to append text
    logFile.open( logPath.string(), ios::app );

    myObjectPointer = this;

    logIN("Log started");
}

//////////////////////////////////////////////////////////
SysQLogger::~SysQLogger (void)
{
    logIN("Log finished");
}

//////////////////////////////////////////////////////////
SysQLogger* SysQLogger::getLogger( void ) {
    if (myObjectPointer == NULL){
        myObjectPointer = new SysQLogger();
    }
    return myObjectPointer;
}

//////////////////////////////////////////////////////////
const std::string SysQLogger::curDateTime( void )
{
    time_t now;
    time(&now);
    char dateTime[20];

    strftime( dateTime, 20, "%Y%m%d:%H%M%S", localtime(&now) );

    return (string) dateTime;

}

//////////////////////////////////////////////////////////
void SysQLogger::log(const string& message)
{
    logFile << "[" << programSlug << "] : ";
    logFile << curDateTime() << " : ";
    logFile << message << "\n" << std::flush;
}

//////////////////////////////////////////////////////////
void SysQLogger::logOK(const string& message)
{
    logFile << "[OK] ";
    log(message);
}

//////////////////////////////////////////////////////////
void SysQLogger::logER(const string& message)
{
    logFile << "[ER] ";
    log(message);
}

//////////////////////////////////////////////////////////
void SysQLogger::logWA(const string& message)
{
    logFile << "[WA] ";
    log(message);
}

//////////////////////////////////////////////////////////
void SysQLogger::logIN(const string& message)
{
    logFile << "[IN] ";
    log(message);
}

//////////////////////////////////////////////////////////
void SysQLogger::setNewSlug(const string newSlug) {
    logIN("Changed program slug from " + programSlug + " to " + newSlug);

    programSlug = newSlug;
}

//////////////////////////////////////////////////////////
string  SysQLogger::getSlug( void ) {
    return programSlug;
}

//////////////////////////////////////////////////////////
bool SysQLogger::isLogOpen( void ) {
    return logFile.good();
}