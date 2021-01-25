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
// Stage Lab SysQ audio player main header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include <string>
#include <csignal>
#include <filesystem>
#include "cuems_audioplayerConfig.h"
#include "commandlineparser.h"
#include "audioplayer.h"
#include "cuemslogger.h"
#include "cuems_errors.h"

//////////////////////////////////////////////////////////
// Functions declarations

void showcopyright( void );
void showusage( void );
void showwarrantydisclaimer( void );
void showcopydisclaimer( void );

// System signal handlers
void sigTermHandler( int signum );
void sigUsr1Handler( int signum );
void sigIntHandler( int signum );