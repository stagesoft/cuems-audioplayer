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
// Stage Lab Cuems OSC receiver class source file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "oscreceiver.h"

////////////////////////////////////////////
// Initializing static class members


////////////////////////////////////////////
OscReceiver::OscReceiver( int port, const string oscRoute )
                        : oscPort(port), oscAddress(oscRoute)
{
    // OSC UDP Listener
    udpListener = new UdpListeningReceiveSocket(
            IpEndpointName( IpEndpointName::ANY_ADDRESS, oscPort ),
            this );

    // Set and detach our threaded checker loop
	std::thread runnerThread( & OscReceiver::threadedRun, this );
	runnerThread.detach();

}

////////////////////////////////////////////
OscReceiver::~OscReceiver( void )
{

}

////////////////////////////////////////////
void OscReceiver::threadedRun ( void )
{
    std::cout << endl << endl << "OSC object ready & listening at port " << oscPort << " address " << oscAddress << endl << endl;
    udpListener->Run();
} 

////////////////////////////////////////////
void OscReceiver::setOscAddress( std::string address )
{
    oscAddress = address;
}

////////////////////////////////////////////
std::string OscReceiver::getOscAddress( void )
{
    return oscAddress;
}
