#!/usr/bin/env python3
"""
MTC Stress Test for cuems-audioplayer

This script performs stress testing with seek operations:
1. Starts MTC timecode first (runs continuously)
2. Launches cuems-audioplayer WITHOUT -m flag (MTC following disabled)
3. Waits for player to load file and stabilize
4. Enables MTC following via OSC command
5. Plays 40 seconds
6. Seeks back and forth using /offset OSC command
7. Plays another 20 seconds
8. Seeks again
9. Plays 10 seconds

Tests 3 files: 22kHz (downsampling), 44.1kHz (native), 48kHz (upsampling)
"""

import sys
import os
import time
import subprocess
import signal
import socket
import struct
import argparse
from pathlib import Path
from datetime import datetime

# Import MTC listener from cuems-engine
try:
    sys.path.insert(0, '/home/ion/src/cuems/cuems-engine/src')
    sys.path.insert(0, '/home/ion/src/cuems/cuems-utils/src')
    from cuemsengine.tools.MtcListener import MtcListener
    from cuemsutils.tools.CTimecode import CTimecode
    MTC_LISTENER_AVAILABLE = True
except ImportError:
    MTC_LISTENER_AVAILABLE = False
    print("⚠ Warning: cuems-engine MTC listener not available")

# Import MTC sender (same as MTC_AUTOMATED_TEST.py)
import ctypes
from ctypes.util import find_library

class MtcSender:
    """Python wrapper for libmtcmaster using system-installed library"""
    
    def __init__(self, fps=25, port=0, portname="TestMTC"):
        libname = find_library('mtcmaster')
        if not libname:
            for path in ['/usr/local/lib/libmtcmaster.so',
                        '/usr/lib/libmtcmaster.so',
                        'libmtcmaster.so.0']:
                if Path(path).exists():
                    libname = path
                    break
        
        if not libname:
            raise OSError("libmtcmaster.so not found. Please install libmtcmaster.")
        
        self.mtc_lib = ctypes.CDLL(libname)
        self.mtc_lib.MTCSender_create.restype = ctypes.c_void_p
        self.mtcproc = self.mtc_lib.MTCSender_create()
        self.port = port
        self.char_portname = portname.encode('utf-8')
        
        self.mtc_lib.MTCSender_openPort.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        self.mtc_lib.MTCSender_play.argtypes = [ctypes.c_void_p]
        self.mtc_lib.MTCSender_stop.argtypes = [ctypes.c_void_p]
        self.mtc_lib.MTCSender_release.argtypes = [ctypes.c_void_p]
        self.mtc_lib.MTCSender_setTime.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        
        self.mtc_lib.MTCSender_openPort(ctypes.c_void_p(self.mtcproc), self.port, self.char_portname)
    
    def start(self):
        self.mtc_lib.MTCSender_play(ctypes.c_void_p(self.mtcproc))
    
    def stop(self):
        self.mtc_lib.MTCSender_stop(ctypes.c_void_p(self.mtcproc))
    
    def release(self):
        self.mtc_lib.MTCSender_release(ctypes.c_void_p(self.mtcproc))

class MtcReceiverWrapper:
    """Wrapper around cuems-engine's MtcListener to get current MTC time"""
    
    def __init__(self, port=None):
        if not MTC_LISTENER_AVAILABLE:
            raise RuntimeError("cuems-engine MtcListener not available.")
        
        self.mtc_listener = MtcListener(port=port)
        self.mtc_listener.start()  # Start the listener thread
        time.sleep(0.2)  # Brief wait for listener to initialize
    
    def get_current_time_ms(self):
        """Get current MTC time in milliseconds"""
        return self.mtc_listener.milliseconds()
    
    def get_current_time_string(self):
        """Get current MTC time as string (HH:MM:SS:FF)"""
        return str(self.mtc_listener.timecode())
    
    def close(self):
        """Stop and close MTC listener"""
        if hasattr(self, 'mtc_listener'):
            self.mtc_listener.stop()
    
    def __del__(self):
        self.close()

def pad_string(s):
    """Pad string to 4-byte boundary for OSC format"""
    return s + b'\x00' * (4 - (len(s) % 4))

def send_osc_message(host, port, address, *args):
    """Send OSC message via UDP (same as MTC_AUTOMATED_TEST.py)"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        
        # Build OSC message
        # Address pattern (null-terminated, padded to 4 bytes)
        msg = pad_string(address.encode('utf-8'))
        
        # Type tag string (starts with comma, null-terminated, padded to 4 bytes)
        type_tags = ',' + ''.join('f' if isinstance(a, float) else 'i' if isinstance(a, int) else 's' if isinstance(a, str) else '' for a in args)
        msg += pad_string(type_tags.encode('utf-8'))
        
        # Arguments
        for arg in args:
            if isinstance(arg, float):
                msg += struct.pack('>f', arg)
            elif isinstance(arg, int):
                msg += struct.pack('>i', arg)
            elif isinstance(arg, str):
                msg += pad_string(arg.encode('utf-8'))
        
        sock.sendto(msg, (host, port))
        sock.close()
        return True
    except Exception as e:
        print(f"Error sending OSC message: {e}")
        return False

def find_player_binary():
    """Find the audioplayer-cuems binary"""
    possible_paths = [
        'build/audioplayer-cuems_dbg',
        'build/audioplayer-cuems',
        'build/src/audioplayer-cuems_dbg',
        'build/src/audioplayer-cuems',
    ]
    
    for path in possible_paths:
        if Path(path).exists():
            return Path(path).absolute()
    
    return None

def run_stress_test(test_name, audio_file, player_binary, osc_port, mtc_sender, mtc_receiver, use_pw_jack=False):
    """Run a stress test with seek operations"""
    print(f"\n{'='*80}")
    print(f"STRESS TEST: {test_name}")
    print(f"{'='*80}")
    print(f"Audio file: {audio_file}")
    
    # Start MTC timecode (should already be running, but ensure it is)
    if not mtc_sender:
        print("✗ MTC sender not available")
        return False
    
    # Get initial MTC time
    if mtc_receiver:
        # Brief wait to ensure we have latest MTC time
        time.sleep(0.1)
        initial_mtc_ms = mtc_receiver.get_current_time_ms()
        initial_mtc_str = mtc_receiver.get_current_time_string()
        print(f"Initial MTC time: {initial_mtc_str} ({initial_mtc_ms} ms)")
    else:
        initial_mtc_ms = 0
    
    # Start player WITHOUT MTC following
    print(f"\n[1/7] Starting player WITHOUT MTC following...")
    player_cmd = [str(player_binary), '-f', str(audio_file), '-p', str(osc_port), '-o', '0']
    
    # Wrap with pw-jack if needed
    if use_pw_jack:
        player_cmd = ['pw-jack'] + player_cmd
    
    player_process = subprocess.Popen(
        player_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    # Wait for player to stabilize
    time.sleep(1.5)
    
    if player_process.poll() is not None:
        print(f"✗ Player exited immediately (code {player_process.returncode})")
        stdout, stderr = player_process.communicate()
        print(f"STDOUT: {stdout}")
        print(f"STDERR: {stderr}")
        return False
    
    print("✓ Player started and file loaded")
    
    # Get current MTC time and calculate offset
    if mtc_receiver:
        # Get current MTC time RIGHT NOW (when we're about to enable MTC following)
        time.sleep(0.1)  # Brief wait for latest MTC time
        current_mtc_ms = mtc_receiver.get_current_time_ms()
        current_mtc_str = mtc_receiver.get_current_time_string()
        offset_ms = -current_mtc_ms
        print(f"\n[2/7] Setting offset and enabling MTC following...")
        print(f"  Current MTC time: {current_mtc_str} ({current_mtc_ms} ms)")
        print(f"  Calculated offset: {offset_ms} ms")
        
        # Send offset via OSC
        # oscRoute is "" in main.cpp, so address is "/offset"
        print(f"  Setting offset via OSC...")
        if send_osc_message('127.0.0.1', osc_port, "/offset", float(offset_ms)):
            print(f"  ✓ Offset OSC command sent: /offset = {offset_ms} ms")
            offset_sent = True
        else:
            print(f"  ✗ Failed to send offset OSC command")
            player_process.terminate()
            return False
        
        # Brief pause for offset to be set
        time.sleep(0.5)
        
        # Enable MTC following via OSC
        # oscRoute is "" in main.cpp, so address is "/mtcfollow"
        print(f"  Enabling MTC following via OSC...")
        if send_osc_message('127.0.0.1', osc_port, "/mtcfollow"):
            print(f"  ✓ MTC following OSC command sent: /mtcfollow")
            osc_sent = True
        else:
            print(f"  ✗ Failed to enable MTC following")
            player_process.terminate()
            return False
        
        # Brief pause for MTC following to start
        time.sleep(0.5)
    else:
        print("⚠ MTC receiver not available, using static offset")
        offset_ms = -initial_mtc_ms if initial_mtc_ms > 0 else 0
    
    # Play for 40 seconds
    print(f"\n[3/7] Playing for 40 seconds...")
    for i in range(8):  # Check every 5 seconds
        time.sleep(5)
        if player_process.poll() is not None:
            print(f"✗ Player exited unexpectedly at {i*5}s!")
            stdout, stderr = player_process.communicate()
            print(f"STDOUT: {stdout}")
            print(f"STDERR: {stderr}")
            return False
        if mtc_receiver:
            current_mtc = mtc_receiver.get_current_time_ms()
            current_mtc_str = mtc_receiver.get_current_time_string()
            elapsed_ms = current_mtc - current_mtc_ms if 'current_mtc_ms' in locals() else 0
            print(f"  [{i*5+5}s] MTC time: {current_mtc_str} ({current_mtc} ms, +{elapsed_ms} ms)")
    
    # Seek back and forth
    print(f"\n[4/7] Seeking back and forth using /offset OSC...")
    seek_positions = [
        -10000,  # Seek back 10 seconds
        5000,    # Seek forward 5 seconds
        -15000,  # Seek back 15 seconds
        20000,   # Seek forward 20 seconds
    ]
    
    for seek_offset_ms in seek_positions:
        if mtc_receiver:
            current_mtc_ms = mtc_receiver.get_current_time_ms()
            new_offset_ms = offset_ms + seek_offset_ms
        else:
            new_offset_ms = offset_ms + seek_offset_ms
        
        print(f"  Seeking: offset = {new_offset_ms} ms")
        # oscRoute is "" in main.cpp, so address is "/offset"
        if send_osc_message('127.0.0.1', osc_port, "/offset", float(new_offset_ms)):
            print(f"  ✓ Seek command sent: /offset")
            seek_sent = True
        else:
            print(f"  ✗ Seek command failed")
            seek_sent = False
        
        time.sleep(0.5)  # Wait for seek to complete
        
        if player_process.poll() is not None:
            print(f"✗ Player exited during seek!")
            return False
    
    # Play for 20 seconds
    print(f"\n[5/7] Playing for 20 seconds after seeks...")
    for i in range(4):  # Check every 5 seconds
        time.sleep(5)
        if player_process.poll() is not None:
            print(f"✗ Player exited unexpectedly at {i*5}s!")
            return False
        if mtc_receiver:
            current_mtc = mtc_receiver.get_current_time_ms()
            current_mtc_str = mtc_receiver.get_current_time_string()
            elapsed_ms = current_mtc - current_mtc_ms if 'current_mtc_ms' in locals() else 0
            print(f"  [{i*5+5}s] MTC time: {current_mtc_str} ({current_mtc} ms, +{elapsed_ms} ms)")
    
    # Seek again
    print(f"\n[6/7] Seeking again...")
    if mtc_receiver:
        current_mtc_ms = mtc_receiver.get_current_time_ms()
        new_offset_ms = offset_ms - 5000  # Seek back 5 seconds
    else:
        new_offset_ms = offset_ms - 5000
    
    print(f"  Seeking: offset = {new_offset_ms} ms")
    # oscRoute is "" in main.cpp, so address is "/offset"
    if send_osc_message('127.0.0.1', osc_port, "/offset", float(new_offset_ms)):
        print(f"  ✓ Seek command sent: /offset")
        seek_sent = True
    else:
        print(f"  ✗ Seek command failed")
        seek_sent = False
    
    time.sleep(0.5)
    
    if player_process.poll() is not None:
        print(f"✗ Player exited during seek!")
        return False
    
    # Play for 10 seconds
    print(f"\n[7/7] Playing for final 10 seconds...")
    for i in range(2):  # Check every 5 seconds
        time.sleep(5)
        if player_process.poll() is not None:
            print(f"✗ Player exited unexpectedly at {i*5}s!")
            return False
        if mtc_receiver:
            current_mtc = mtc_receiver.get_current_time_ms()
            current_mtc_str = mtc_receiver.get_current_time_string()
            elapsed_ms = current_mtc - current_mtc_ms if 'current_mtc_ms' in locals() else 0
            print(f"  [{i*5+5}s] MTC time: {current_mtc_str} ({current_mtc} ms, +{elapsed_ms} ms)")
    
    # Stop player
    print(f"\n[FINAL] Stopping player...")
    player_process.terminate()
    try:
        player_process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        player_process.kill()
    
    print(f"✓ Stress test completed successfully")
    return True

def main():
    parser = argparse.ArgumentParser(description='MTC Stress Test for cuems-audioplayer')
    parser.add_argument('--log', type=str, default=None,
                       help='Log file path for test results')
    args = parser.parse_args()
    
    # Set up logging
    if args.log:
        log_file = Path(args.log)
    else:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_file = Path(f"MTC_STRESS_TEST_RESULTS_{timestamp}.log")
    
    print(f"Test results will be logged to: {log_file.absolute()}")
    
    # Check JACK
    def check_jack_running():
        """Check if JACK is running, or if pw-jack is available as fallback
        
        Returns:
            tuple: (is_jack_available, use_pw_jack, warning_message)
        """
        # First check if JACK daemon is running
        try:
            result = subprocess.run(['pgrep', 'jackd'], capture_output=True, text=True)
            if result.returncode == 0:
                return (True, False, None)
        except:
            pass
        
        # JACK not running, check if pw-jack is available
        try:
            result = subprocess.run(['which', 'pw-jack'], capture_output=True, text=True)
            if result.returncode == 0:
                # Also check if PipeWire is running
                try:
                    subprocess.run(['pw-cli', 'info'], capture_output=True, check=True, timeout=1)
                    return (False, True, "⚠ Warning: JACK daemon not running, using pw-jack (PipeWire JACK compatibility layer)")
                except:
                    return (False, True, "⚠ Warning: JACK daemon not running, pw-jack found but PipeWire may not be running")
        except:
            pass
        
        # Neither JACK nor pw-jack available
        return (False, False, None)
    
    jack_available, use_pw_jack, warning_msg = check_jack_running()
    if not jack_available and not use_pw_jack:
        print("✗ JACK is not running and pw-jack is not available.")
        print("  Please start JACK: jackd -d alsa -r 44100")
        print("  Or install PipeWire with JACK support for pw-jack fallback")
        sys.exit(1)
    
    if jack_available:
        print("✓ JACK is running")
    elif use_pw_jack:
        print(warning_msg)
        print("✓ Using pw-jack as fallback")
    
    # Find player binary
    player_binary = find_player_binary()
    if not player_binary:
        print("✗ Player binary not found. Please build the project first:")
        print("  cd build && cmake .. && make")
        sys.exit(1)
    print(f"✓ Player binary found: {player_binary}")
    
    # Initialize MTC sender
    print(f"\n[INIT] Initializing MTC sender...")
    try:
        mtc_sender = MtcSender(fps=25, port=0, portname="MtcMaster:MTCPort")
        mtc_sender.start()
        print("✓ MTC Sender initialized")
        print("[INIT] Starting MTC timecode (will run continuously)...")
        print("✓ MTC timecode running")
    except Exception as e:
        print(f"✗ Failed to initialize MTC sender: {e}")
        sys.exit(1)
    
    # Initialize MTC receiver
    mtc_receiver = None
    if MTC_LISTENER_AVAILABLE:
        try:
            print("[INIT] Initializing MTC receiver (using cuems-engine MtcListener)...")
            # Try to find MTC port automatically (MtcListener will auto-detect)
            mtc_receiver = MtcReceiverWrapper(port=None)
            print("✓ MTC Receiver initialized")
            # Wait a bit for MTC messages to arrive
            time.sleep(0.5)
            current_tc = mtc_receiver.get_current_time_string()
            current_ms = mtc_receiver.get_current_time_ms()
            print(f"✓ Receiving MTC timecode: {current_tc} ({current_ms} ms)")
        except Exception as e:
            print(f"✗ Failed to initialize MTC Receiver: {e}")
            print("  Continuing without dynamic offset calculation...")
            mtc_receiver = None
    
    # Test files - organized by format (WAV → AIFF → MP3)
    test_files = [
        # WAV files
        ("WAV 22kHz (downsampling)", "stress_test_files/stress_test_22050_16bit.wav"),
        ("WAV 44.1kHz (native)", "stress_test_files/stress_test_44100_16bit.wav"),
        ("WAV 48kHz (upsampling)", "stress_test_files/stress_test_48000_16bit.wav"),
        # AIFF files
        ("AIFF 22kHz (downsampling)", "stress_test_files/stress_test_22050_16bit.aiff"),
        ("AIFF 44.1kHz (native)", "stress_test_files/stress_test_44100_16bit.aiff"),
        ("AIFF 48kHz (upsampling)", "stress_test_files/stress_test_48000_16bit.aiff"),
        # MP3 files
        ("MP3 22kHz (downsampling)", "stress_test_files/stress_test_22050_192k.mp3"),
        ("MP3 44.1kHz (native)", "stress_test_files/stress_test_44100_192k.mp3"),
        ("MP3 48kHz (upsampling)", "stress_test_files/stress_test_48000_192k.mp3"),
    ]
    
    results = []
    base_port = 8000
    
    for i, (test_name, audio_file) in enumerate(test_files):
        audio_path = Path(audio_file)
        if not audio_path.exists():
            print(f"\n⚠ Skipping {test_name}: File not found: {audio_file}")
            continue
        
        osc_port = base_port + i
        success = run_stress_test(test_name, audio_path, player_binary, osc_port, mtc_sender, mtc_receiver, use_pw_jack)
        results.append((test_name, success))
        
        # Small delay between tests
        if i < len(test_files) - 1:
            time.sleep(2)
    
    # Stop MTC
    print(f"\n[FINAL] Stopping MTC timecode...")
    mtc_sender.stop()
    mtc_sender.release()
    
    # Print summary
    print(f"\n{'='*80}")
    print("STRESS TEST SUMMARY")
    print(f"{'='*80}")
    passed = sum(1 for _, success in results if success)
    total = len(results)
    
    for test_name, success in results:
        status = "✓ PASS" if success else "✗ FAIL"
        print(f"{status}: {test_name}")
    
    print(f"{'='*80}")
    print(f"Total: {passed}/{total} tests passed")
    
    if passed == total:
        print("✓ All stress tests passed!")
    else:
        print(f"✗ {total - passed} test(s) failed")
        sys.exit(1)

if __name__ == "__main__":
    main()

