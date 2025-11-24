/* LICENSE TEXT

    audioplayer for linux based using RtAudio and RtMidi libraries to
    process audio and receive MTC sync. It also uses oscpack to receive
    some configurations through osc commands.
    Copyright (C) 2020  Stage Lab Coop.

    Authors:
        Alex Ramos <alex@stagelab.coop>
        Ion Reguera <ion@stagelab.coop>

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
// Stage Lab Cuems return error codes for SysQ apps
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#ifndef CUEMS_ERRORS_H
#define CUEMS_ERRORS_H

#define CUEMS_EXIT_OK                   0               // Success
#define CUEMS_EXIT_FAILURE              -1              // Generic error
#define CUEMS_EXIT_WRONG_PARAMETERS     -2
#define CUEMS_EXIT_WRONG_DATA_FILE      -3
#define CUEMS_EXIT_AUDIO_DEVICE_ERR     -4              // Perhaps jack not running
#define CUEMS_EXIT_FAILED_OLA_SETUP     -5              // OLA setup failed, daemon running?
#define CUEMS_EXIT_FAILED_OLA_SEL_SERV  -6              // OLA select server failed, daemon running?
#define CUEMS_EXIT_FAILED_XML_INIT      -7              // XML parser initializer failed
#define CUEMS_EXIT_NO_MIDI_PORTS_FOUND  -8              // Midi Error when opening port

#endif // CUEMS_ERRORS_H