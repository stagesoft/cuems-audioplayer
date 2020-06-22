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
// Stage Lab Cuems OSC receiver class header file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#ifndef OSCRECEIVER_H
#define OSCRECEIVER_H

#include <iostream>
#include <thread>
#include "oscpack/osc/OscPacketListener.h"
#include "oscpack/osc/OscReceivedElements.h"
#include "oscpack/ip/UdpSocket.h"
#include "oscpack/ip/IpEndpointName.h"

//////////////////////////////////////////////////////////
// Preprocessor definitions
#define PORT 7000

using namespace std;

class OscReceiver : public osc::OscPacketListener 
{
    public:
        OscReceiver( int port = 7000, const string oscRoute = "/master" );
        ~OscReceiver( void );

        UdpListeningReceiveSocket *udpListener;

        void setOscAddress( std::string address );
        std::string getOscAddress( void );

    protected:

        inline virtual void ProcessMessage( const osc::ReceivedMessage& /*m*/ , 
                                            const IpEndpointName& /*remoteEndpoint*/ ) {};

        void threadedRun( void );

        int oscPort;
        std::string oscAddress;

};


#endif // OSCRECEIVER_H