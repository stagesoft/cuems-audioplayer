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
// Stage Lab SysQ return error codes for SysQ apps
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#ifndef SYSQ_ERRORS_H
#define SYSQ_ERRORS_H

#define SYSQ_EXIT_OK                    0               // Success
#define SYSQ_EXIT_FAILURE               -1              // Generic error
#define SYSQ_EXIT_WRONG_PARAMETERS      -2
#define SYSQ_EXIT_WRONG_DATA_FILE       -3
#define SYSQ_EXIT_AUDIO_DEVICE_ERR      -4              // Perhaps jack not running

#endif