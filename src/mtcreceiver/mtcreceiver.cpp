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
// Stage Lab Cuems MTC receiver class source file
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#include "mtcreceiver.h"

////////////////////////////////////////////
// Initializing static class members
bool MtcReceiver::isTimecodeRunning = false;
long int MtcReceiver::mtcHead = 0;
unsigned char MtcReceiver::curFrameRate = 25;

//////////////////////////////////////////////////////////
MtcReceiver::MtcReceiver( 	RtMidi::Api api, 
							const std::string& clientName,
							unsigned int queueSizeLimit) :
							RtMidiIn( api, clientName, queueSizeLimit ) {
    // Check for midi ports available
    if ( RtMidiIn::getPortCount() == 0 ) {
		CuemsLogger::getLogger()->logError("No midi ports found.");

        exit(CUEMS_EXIT_NO_MIDI_PORTS_FOUND);
    }

	// Set and detach our threaded checker loop
	std::thread checkerThread( &MtcReceiver::threadedChecker, this );
	checkerThread.detach();

    // Set the midi callback function to process incoming midi messages
    RtMidiIn::setCallback( &midiCallback, (void*) this );

    // Don't ignore sysex, timing, or active sensing messages
    RtMidiIn::ignoreTypes( false, false, false );

    // Then, at last, open midi default port
    RtMidiIn::openPort( 0 );

}

//////////////////////////////////////////////////////////
MtcReceiver::~MtcReceiver( void ) {
	checkerOn = false;
    RtMidiIn::closePort();
}

//////////////////////////////////////////////////////////
std::string MtcFrame::toString() const {
	std::stringstream stream;
	stream << std::setw(2) << std::setfill('0') << hours << ":"
	       << std::setw(2) << std::setfill('0') << minutes << ":"
	       << std::setw(2) << std::setfill('0') << seconds << ":"
	       << std::setw(2) << std::setfill('0') << frames;
	return stream.str();
}

//////////////////////////////////////////////////////////
long int MtcFrame::toSeconds() const {
	long int time = hours * 3600.0;
	time += minutes * 60.0;
	time += seconds;
	time += frames / getFps();
	return time;
}

//////////////////////////////////////////////////////////
long int MtcFrame::toMilliseconds() const {
	double time = hours * 3600.0;
	time += minutes * 60.0;
	time += seconds;
	time += frames / getFps();
	time *= 1000;
	return (long int) time;
}

//////////////////////////////////////////////////////////
void MtcFrame::fromSeconds( long int s ) {
	seconds = (int)s % 60;
	minutes = (int)( (s - seconds) * (1 / 60) ) % 60;
	hours = (int)(s * (1 / 3600) ) % 60;

	// round fractional part of seconds for ms
	long int ms = (int)(floor((s - floor(s)) * 1000.0) + 0.5);
	frames = msToFrames(ms);
}

//////////////////////////////////////////////////////////
long int MtcFrame::msToFrames( long int ms ) {
	return ( ms / ( getFps() / 1000.0) );
}

//////////////////////////////////////////////////////////
float MtcFrame::getFps( void ) const {
	// NOTE: this function returns a float due to the decimals
	// in the 29.97 mode, it could be that this value is somewhere
	// truncated... 
	// TO DO : 	review it carefully in the future to avoid problems
	// 			in different calculus using this function
	switch ( rate ) {
		case FR_24:
			return 24;
		case FR_25:
			return 25;
		case FR_29:
			return 29.97;
		case FR_30:
		default:
			return 30;
	}
}

//////////////////////////////////////////////////////////
// RtMidi callback - Static member function
void MtcReceiver::midiCallback( double deltatime, std::vector< unsigned char > *m, void * data ) {
    MtcReceiver *mtcr = (MtcReceiver*) data;
    std::vector< unsigned char > message = *m;

	// First of all just a small message time gap check
	if ( deltatime > 0.10 )
		mtcr->isTimecodeRunning = false;
	else 
		mtcr->isTimecodeRunning = true;

	// Then we note down the timestamp when last midi message arrived
	mtcr->timecodeTimestamp = chrono::duration_cast<chrono::nanoseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();

    // So, we have a new mide message and we check whether it is time
    // information and in that case store it in our current frame and 
    // quarter frame data structures
    mtcr->decodeNewMidiMessage(message);
}

//////////////////////////////////////////////////////////
bool MtcReceiver::isFullFrame(std::vector<unsigned char> &message) {
	return
		(message.size() == FF_LEN) && 	// Message length is right
		(message[1] == 0x7F) && 		// universal message
		(message[2] == 0x7F) && 		// global broadcast
		(message[3] == 0x01) && 		// time code
		(message[4] == 0x01) && 		// full frame
		(message[9] == 0xF7);   		// end of sysex
}

//////////////////////////////////////////////////////////
void MtcReceiver::decodeQuarterFrame(std::vector<unsigned char> &message) {
	bool complete = false;
	unsigned char dataByte = message[1];
	unsigned char msgType = dataByte & 0xF0;

	if(direction == 0 && qfCount > 1) {
		// If not set direction and we are already counting quarters...
		// let's update the last message type flag
		unsigned char lastMsgType = lastDataByte & 0xF0;
		if(lastMsgType < msgType) {
			// Forwards
			direction = 1;
		}
		else if(lastMsgType > msgType) {
			// Backwards
			direction = -1;
		}
	}

	// Each time we process a quarter we assume that the head is
	// stil going on...
	// TO DO : adjust for both directions
	// 1/4 * 1000 milliseconds * (1 / framerate)
	mtcHead += 250 / curFrame.getFps();

    // Updateing lastDataByte flag
    lastDataByte = dataByte;

	switch(msgType) {
		case 0x00: // frame LSB
			quarterFrame.frames = (int)(dataByte & 0x0F);
			qfCount += 1;

			// Yes, it is a first quarter
			firstQFlag = true;

			// Check if we are going backwards, we have all the
			// quarters and so we have a complete MTC code
			if(qfCount >= QF_LEN && direction == -1 && lastQFlag) {
				complete = true;
			}
			break;
		case 0x10: // frame MSB
			quarterFrame.frames |= (int)((dataByte & 0x01) << 4);
			qfCount += 1;
			break;
		case 0x20: // second LSB
			quarterFrame.seconds = (int)(dataByte & 0x0F);
			qfCount += 1;
			break;
		case 0x30: // second MSB
			quarterFrame.seconds |= (int)((dataByte & 0x03) << 4);
			qfCount += 1;
			break;
		case 0x40: // minute LSB
			quarterFrame.minutes = (int)(dataByte & 0x0F);
			qfCount += 1;
			break;
		case 0x50: // minute MSB
			quarterFrame.minutes |= (int)((dataByte & 0x03) << 4);
			qfCount += 1;
			break;
		case 0x60: // hours LSB
			quarterFrame.hours = (int)(dataByte & 0x0F);
			qfCount += 1;
			break;
		case 0x70: // hours MSB & framerate
			quarterFrame.hours |= (int)((dataByte & 0x01) << 4);
			quarterFrame.rate = (dataByte & 0x06) >> 1;
			qfCount += 1;

			// Yes, it is a last quarter 
			lastQFlag = true;

			// Check if we are going forwards, we have all the
			// quarters and so we have a complete MTC code
			if( qfCount >= QF_LEN && direction == 1 && firstQFlag ) {
				complete = true;
			}
			break;
		default:
			return;
	}

	// Update time using the (hopefully) complete message
	if(complete) {
		// Add a 2 frames adjust to compensate the time 
		// it takes to receive all 8 QF messages
		quarterFrame.frames += 2;

		// Our current frame is the current quarter frame structure
		curFrame = quarterFrame;

		// We have complete valid MTC time info so, we can update 
		// our MTC head position
		mtcHead = curFrame.toMilliseconds();
		curFrameRate = curFrame.getFps();

		// Reset quarter frame structure and detection flags
		quarterFrame = MtcFrame();
		direction = 0;
		qfCount = 0;
		lastQFlag = firstQFlag = false;
	}
}

//////////////////////////////////////////////////////////
void MtcReceiver::decodeFullFrame(std::vector<unsigned char> &message) {
	curFrame.hours = (int)(message[5] & 0x1F);
	curFrame.rate = (int)((message[5] & 0x60) >> 5);
	curFrame.minutes = (int)(message[6]);
	curFrame.seconds = (int)(message[7]);
	curFrame.frames = (int)(message[8]);

	// A full message is always valid qhole MTC time info so
	// we can update our MTC head position
	mtcHead = curFrame.toMilliseconds();
}

//////////////////////////////////////////////////////////
bool MtcReceiver::decodeNewMidiMessage( std::vector<unsigned char> &message ) {
    // Is it a time codification frame??
	if( message[0] == MIDI_TIME_CODE ) {
        // A MTC quarter frame?
		decodeQuarterFrame(message);
		return true;
	}
	else if( message[0] == MIDI_SYSEX && isFullFrame(message) ) {
        // A SysEx full frame?
        decodeFullFrame(message);
		return true;
	}

    return false;
}

//////////////////////////////////////////////////////////
void MtcReceiver::threadedChecker( void ) {
	while ( checkerOn ) {
		if ( isTimecodeRunning ) {
			long int timecodeNow = chrono::duration_cast<chrono::nanoseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();
			
			long int timecodeDiff =  (timecodeNow - timecodeTimestamp) / 1E6;

			if ( timecodeDiff > 50 )
				isTimecodeRunning = false;
		}

		std::this_thread::sleep_for( std::chrono::milliseconds(20) );
	}
}

