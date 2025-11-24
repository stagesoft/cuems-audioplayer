#!/usr/bin/env python3
"""
MTC Automated Integration Test for cuems-audioplayer

This script:
1. Starts MTC timecode first (runs continuously)
2. Launches cuems-audioplayer WITHOUT -m flag (MTC following disabled)
3. Waits for player to load file and stabilize (short wait)
4. Enables MTC following via OSC command
5. Plays for specified duration per file (default: 15 seconds)
6. Uses offset so next file starts from beginning with timecode already running

Supports testing:
- Audio files: WAV, AIFF, MP3, FLAC, OGG, OPUS, AAC
- Video files: MP4, MKV, AVI, MOV (audio track extraction)

Usage:
    ./MTC_AUTOMATED_TEST.py [--format wav|aiff|mp3|video|all] [--duration SECONDS] [--file PATH]
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
import glob
from datetime import datetime

# Import ctypes for direct library loading
import ctypes
from ctypes.util import find_library

# Import MTC listener from cuems-engine
try:
    import sys
    sys.path.insert(0, '/home/ion/src/cuems/cuems-engine/src')
    sys.path.insert(0, '/home/ion/src/cuems/cuems-utils/src')
    from cuemsengine.tools.MtcListener import MtcListener
    from cuemsutils.tools.CTimecode import CTimecode
    MTC_LISTENER_AVAILABLE = True
except ImportError as e:
    MTC_LISTENER_AVAILABLE = False
    print(f"⚠ Warning: cuems-engine MTC listener not available: {e}")
    print("  Trying to find in system Python path...")
    try:
        from cuemsengine.tools.MtcListener import MtcListener
        from cuemsutils.tools.CTimecode import CTimecode
        MTC_LISTENER_AVAILABLE = True
    except ImportError:
        pass

# MTC Sender wrapper class (using system library)
class MtcSender:
    """Python wrapper for libmtcmaster using system-installed library"""
    
    def __init__(self, fps=25, port=0, portname="TestMTC"):
        # Try to find library in system paths
        libname = find_library('mtcmaster')
        
        if not libname:
            # Try common locations
            for path in ['/usr/local/lib/libmtcmaster.so',
                        '/usr/lib/libmtcmaster.so',
                        'libmtcmaster.so.0']:
                if Path(path).exists():
                    libname = path
                    break
        
        if not libname:
            raise OSError("libmtcmaster.so not found. Please install libmtcmaster.")
        
        try:
            self.mtc_lib = ctypes.CDLL(libname)
        except OSError as e:
            print(f"ERROR: Could not load libmtcmaster from {libname}")
            print(f"Error: {e}")
            raise
        
        self.mtc_lib.MTCSender_create.restype = ctypes.c_void_p
        self.mtcproc = self.mtc_lib.MTCSender_create()
        self.port = port
        self.char_portname = portname.encode('utf-8')
        self.fps = fps  # Store FPS (not passed to library yet)
        
        # Set up function signatures (using actual function names from libmtcmaster)
        # MTCSender_openPort takes: (void_p, int port, char_p portname)
        self.mtc_lib.MTCSender_openPort.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        self.mtc_lib.MTCSender_play.argtypes = [ctypes.c_void_p]
        self.mtc_lib.MTCSender_stop.argtypes = [ctypes.c_void_p]
        self.mtc_lib.MTCSender_pause.argtypes = [ctypes.c_void_p]
        # MTCSender_setTime takes nanoseconds (uint64)
        self.mtc_lib.MTCSender_setTime.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        self.mtc_lib.MTCSender_release.argtypes = [ctypes.c_void_p]
        
        # Open port with port number and port name
        self.mtc_lib.MTCSender_openPort(ctypes.c_void_p(self.mtcproc), self.port, self.char_portname)
    
    def start(self):
        self.mtc_lib.MTCSender_play(ctypes.c_void_p(self.mtcproc))
    
    def stop(self):
        self.mtc_lib.MTCSender_stop(ctypes.c_void_p(self.mtcproc))
    
    def pause(self):
        self.mtc_lib.MTCSender_pause(ctypes.c_void_p(self.mtcproc))
    
    def set_time(self, hours, minutes, seconds, frames):
        """Set MTC time - converts hours:minutes:seconds:frames to nanoseconds"""
        # Calculate total seconds
        total_seconds = hours * 3600 + minutes * 60 + seconds + (frames / self.fps)
        # Convert to nanoseconds
        nanos = int(total_seconds * 1000000000)
        self.mtc_lib.MTCSender_setTime(ctypes.c_void_p(self.mtcproc), ctypes.c_uint64(nanos))
    
    def __del__(self):
        if hasattr(self, 'mtcproc') and self.mtcproc:
            try:
                self.mtc_lib.MTCSender_release(ctypes.c_void_p(self.mtcproc))
            except:
                pass  # Ignore errors during cleanup

# MTC Receiver wrapper using cuems-engine's MtcListener
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
    """Send OSC message via UDP"""
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

def find_player_binary():
    """Find the audioplayer-cuems binary
    
    Searches relative to the script's location, so it works regardless of
    the current working directory.
    """
    # Get the directory where this script is located
    script_dir = Path(__file__).parent.absolute()
    
    # Possible paths relative to project root (where script is located)
    possible_paths = [
        script_dir / 'build' / 'audioplayer-cuems_dbg',
        script_dir / 'build' / 'audioplayer-cuems',
        script_dir / 'build' / 'src' / 'audioplayer-cuems_dbg',
        script_dir / 'build' / 'src' / 'audioplayer-cuems',
        # Also check if we're already in build directory
        script_dir / 'audioplayer-cuems_dbg',
        script_dir / 'audioplayer-cuems',
        script_dir / 'src' / 'audioplayer-cuems_dbg',
        script_dir / 'src' / 'audioplayer-cuems',
        # Check parent directory (if script is in build/)
        script_dir.parent / 'build' / 'audioplayer-cuems_dbg',
        script_dir.parent / 'build' / 'audioplayer-cuems',
        script_dir.parent / 'build' / 'src' / 'audioplayer-cuems_dbg',
        script_dir.parent / 'build' / 'src' / 'audioplayer-cuems',
    ]
    
    for path in possible_paths:
        if path.exists():
            return path.absolute()
    
    return None

def run_test(test_name, audio_file, player_binary, osc_port, mtc_sender, mtc_receiver, port_base=7000, use_pw_jack=False, play_duration=15):
    """Run a single test scenario
    
    Args:
        test_name: Name of the test
        audio_file: Path to audio file
        player_binary: Path to player binary
        osc_port: OSC port number
        mtc_sender: MtcSender instance (MTC timecode should already be running)
        mtc_receiver: MtcReceiver instance (to get current MTC time)
        port_base: Base port number
        use_pw_jack: If True, wrap player command with pw-jack
        play_duration: How many seconds to play (default: 15)
    """
    print(f"\n{'='*80}")
    print(f"TEST: {test_name}")
    print(f"{'='*80}")
    print(f"Audio file: {audio_file}")
    
    # Initial offset calculation (will be updated via OSC when MTC following is enabled)
    # We use offset=0 initially so audio can be read even when MTC following is disabled
    # The offset will be recalculated and set via OSC right before enabling MTC following
    offset_ms = 0
    
    if mtc_receiver:
        # Brief wait to ensure we have latest MTC time
        time.sleep(0.1)
        current_mtc_ms = mtc_receiver.get_current_time_ms()
        mtc_time_str = mtc_receiver.get_current_time_string()
        
        print(f"Current MTC time: {mtc_time_str} ({current_mtc_ms} ms)")
        print(f"Initial offset: {offset_ms} ms (will be updated via OSC when MTC following is enabled)")
    else:
        print(f"⚠ MTC receiver not available, using initial offset: {offset_ms} ms")
    
    # Find available port
    port = port_base
    
    # Step 1: Start player WITHOUT -m flag (MTC following disabled)
    print(f"\n[1/4] Starting player WITHOUT MTC following...")
    player_cmd = [
        str(player_binary),
        '-f', str(audio_file),
        '-p', str(port),
        '-o', str(offset_ms),  # Offset in milliseconds (calculated from current MTC time)
        # NO -m flag - MTC following disabled initially
    ]
    
    # Wrap with pw-jack if needed
    if use_pw_jack:
        player_cmd = ['pw-jack'] + player_cmd
    
    print(f"Command: {' '.join(player_cmd)}")
    
    try:
        # Set LD_LIBRARY_PATH to ensure libmtcmaster is found
        env = os.environ.copy()
        env['LD_LIBRARY_PATH'] = '/usr/local/lib:' + env.get('LD_LIBRARY_PATH', '')
        
        player_process = subprocess.Popen(
            player_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env
        )
        
        # Wait for player to initialize and load file (reduced wait time)
        print("   Waiting for player to load file and stabilize...")
        time.sleep(1.5)  # Reduced from 3 seconds
        
        if player_process.poll() is not None:
            stdout, stderr = player_process.communicate()
            print(f"✗ Player exited immediately")
            print(f"STDOUT: {stdout.decode()}")
            print(f"STDERR: {stderr.decode()}")
            return False
        
        print("✓ Player started and file loaded")
        
        # Step 2: Recalculate offset based on CURRENT MTC time and set it via OSC
        print(f"\n[2/4] Setting offset based on current MTC time and enabling MTC following...")
        
        if mtc_receiver:
            # Get current MTC time RIGHT NOW (when we're about to enable MTC following)
            time.sleep(0.1)  # Brief wait for latest MTC time
            current_mtc_ms_now = mtc_receiver.get_current_time_ms()
            mtc_time_str_now = mtc_receiver.get_current_time_string()
            
            # Calculate offset so file position 0 aligns with current MTC time
            # When MTC following is enabled: playHead = current_mtc_ms_now
            # We want: playHead + headOffset = 0
            # So: headOffset = -playHead = -current_mtc_ms_now * audioMillisecondSize
            # offset_ms = -current_mtc_ms_now
            offset_ms_now = -current_mtc_ms_now
            
            print(f"  Current MTC time: {mtc_time_str_now} ({current_mtc_ms_now} ms)")
            print(f"  Calculated offset: {offset_ms_now} ms")
            print(f"  Setting offset via OSC...")
            
            # Send offset via OSC (before enabling MTC following)
            # oscRoute is "" in main.cpp, so address is "/offset"
            if send_osc_message('127.0.0.1', osc_port, "/offset", float(offset_ms_now)):
                print(f"  ✓ Offset OSC command sent: /offset = {offset_ms_now} ms")
                offset_sent = True
            else:
                print(f"  ⚠ Could not send offset OSC command, using command-line offset")
                offset_sent = False
        else:
            print(f"  ⚠ MTC receiver not available, using command-line offset")
            osc_offset_addresses = []  # Set empty if not available
        
        # Brief pause for offset to be set and processed
        # Need to wait for OSC message to be received and processed by player
        time.sleep(0.5)
        
        # Step 2b: Enable MTC following via OSC
        print(f"  Enabling MTC following via OSC...")
        
        # oscRoute is "" in main.cpp, so address is "/mtcfollow"
        if send_osc_message('127.0.0.1', osc_port, "/mtcfollow"):
            print(f"  ✓ MTC following OSC command sent: /mtcfollow")
            osc_sent = True
        else:
            print("  ⚠ Could not send OSC command, but continuing...")
            osc_sent = False
        
        # Brief pause for OSC command to take effect and for MTC following to start
        # Note: We do NOT send offset again after enabling MTC following, as that would cause audio to skip
        time.sleep(0.5)
        
        # Verify MTC time is advancing
        if mtc_receiver:
            verify_mtc_ms = mtc_receiver.get_current_time_ms()
            verify_mtc_str = mtc_receiver.get_current_time_string()
            print(f"  MTC time after enabling: {verify_mtc_str} ({verify_mtc_ms} ms)")
            if verify_mtc_ms > current_mtc_ms_now:
                print(f"  ✓ MTC timecode is advancing (+{verify_mtc_ms - current_mtc_ms_now} ms)")
            else:
                print(f"  ⚠ MTC timecode may not be advancing")
        
        # Step 3: Play for specified duration
        print(f"\n[3/4] Playing for {play_duration} seconds...")
        print("   MTC timecode is running, player should now follow...")
        
        # Check player is still running
        if player_process.poll() is not None:
            stdout, stderr = player_process.communicate()
            print(f"✗ Player exited unexpectedly!")
            print(f"STDOUT: {stdout.decode()}")
            print(f"STDERR: {stderr.decode()}")
            print(f"\n[ERROR DETAILS]")
            print(f"  Test: {test_name}")
            print(f"  Audio file: {audio_file}")
            print(f"  Player exited with code: {player_process.returncode}")
            if mtc_receiver:
                final_mtc_ms = mtc_receiver.get_current_time_ms()
                final_mtc_str = mtc_receiver.get_current_time_string()
                print(f"  MTC time when player exited: {final_mtc_str} ({final_mtc_ms} ms)")
                print(f"  Offset was: {offset_ms_now if 'offset_ms_now' in locals() else 'N/A'} ms")
                print(f"  Expected seek position: {(final_mtc_ms + (offset_ms_now if 'offset_ms_now' in locals() else 0))} ms")
            return False
        
        # Let it play for specified duration, checking periodically
        for i in range(play_duration):
            time.sleep(1)
            if player_process.poll() is not None:
                stdout, stderr = player_process.communicate()
                print(f"\n✗ Player exited during playback (after {i+1} seconds)!")
                print(f"STDOUT: {stdout.decode()}")
                print(f"STDERR: {stderr.decode()}")
                print(f"\n[ERROR DETAILS]")
                print(f"  Test: {test_name}")
                print(f"  Audio file: {audio_file}")
                print(f"  Player exited with code: {player_process.returncode}")
                print(f"  Playback duration before failure: {i+1} seconds")
                return False
            
            # Check MTC time is advancing (every 5 seconds)
            if (i + 1) % 5 == 0 and mtc_receiver:
                current_check_ms = mtc_receiver.get_current_time_ms()
                elapsed_ms = current_check_ms - verify_mtc_ms
                print(f"   [{i+1}s] MTC time: {mtc_receiver.get_current_time_string()} ({current_check_ms} ms, +{elapsed_ms} ms)")
        
        print(f"✓ Playback test completed ({play_duration} seconds)")
        
        # Step 4: Stop player (but keep MTC running)
        print("\n[4/4] Stopping player (MTC continues running)...")
        player_process.terminate()
        try:
            player_process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            player_process.kill()
        
        print("✓ Test completed successfully")
        return True
        
    except Exception as e:
        print(f"✗ Test failed: {e}")
        import traceback
        traceback.print_exc()
        if 'player_process' in locals():
            player_process.terminate()
        return False

def main():
    parser = argparse.ArgumentParser(description='MTC Automated Integration Test for cuems-audioplayer')
    parser.add_argument('--format', choices=['wav', 'aiff', 'mp3', 'video', 'all'], default='all',
                       help='Format to test: audio formats (wav, aiff, mp3) or video formats (video), or all (default: all)')
    parser.add_argument('--log', type=str, default=None,
                       help='Log file path for test results (default: MTC_TEST_RESULTS_<timestamp>.log)')
    parser.add_argument('--file', type=str, default=None,
                       help='Test only a specific media file (e.g., "test_audio_files/file.wav" or "test_audio_files/video.mp4")')
    parser.add_argument('--duration', type=int, default=15,
                       help='How many seconds to play during each test (default: 15)')
    args = parser.parse_args()
    
    # Set up logging to file
    if args.log:
        log_file = Path(args.log)
    else:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_file = Path(f"MTC_TEST_RESULTS_{timestamp}.log")
    
    # Create a class to handle both console and file output
    class TeeOutput:
        def __init__(self, *files):
            self.files = files
        
        def write(self, obj):
            for f in self.files:
                f.write(obj)
                f.flush()
        
        def flush(self):
            for f in self.files:
                f.flush()
    
    # Open log file and redirect stdout/stderr
    log_file_handle = open(log_file, 'w', encoding='utf-8')
    original_stdout = sys.stdout
    original_stderr = sys.stderr
    sys.stdout = TeeOutput(original_stdout, log_file_handle)
    sys.stderr = TeeOutput(original_stderr, log_file_handle)
    
    print(f"Test results will be logged to: {log_file.absolute()}")
    print("="*80)
    
    print("="*80)
    print("CUEMS AUDIOPLAYER - AUTOMATED MTC INTEGRATION TEST")
    print("="*80)
    print(f"Testing format: {args.format.upper()}")
    print("="*80)
    
    # Check prerequisites
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
    
    player_binary = find_player_binary()
    if not player_binary:
        print("✗ Player binary not found. Please build the project first:")
        print("  cd build && cmake .. && make")
        sys.exit(1)
    print(f"✓ Player binary found: {player_binary}")
    
    # Find test media files (relative to script location)
    # Check both test_audio_files and test_video_files directories
    script_dir = Path(__file__).parent.absolute()
    test_files_dir = script_dir / "test_audio_files"
    test_video_dir = script_dir / "test_video_files"
    
    # At least one directory should exist
    if not test_files_dir.exists() and not test_video_dir.exists():
        print(f"✗ Test media files directories not found:")
        print(f"  Expected: {test_files_dir} or {test_video_dir}")
        sys.exit(1)
    
    # Use test_audio_files as primary (may contain both audio and video)
    if not test_files_dir.exists():
        test_files_dir = test_video_dir
    
    # Initialize MTC sender ONCE (runs continuously for all tests)
    print("\n[INIT] Initializing MTC sender...")
    try:
        mtc_sender = MtcSender(fps=25, port=0, portname="TestMTC")
        print("✓ MTC Sender initialized")
    except Exception as e:
        print(f"✗ Failed to initialize MTC Sender: {e}")
        sys.exit(1)
    
    # Start MTC timecode (runs continuously)
    print("[INIT] Starting MTC timecode (will run continuously)...")
    mtc_sender.set_time(0, 0, 0, 0)
    mtc_sender.start()
    print("✓ MTC timecode running")
    
    # Initialize MTC receiver to listen to timecode and get current time
    print("[INIT] Initializing MTC receiver (using cuems-engine MtcListener)...")
    try:
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
    
    # Check if user wants to test a specific file
    if args.file:
        test_file = Path(args.file)
        if not test_file.exists():
            print(f"✗ File not found: {test_file}")
            sys.exit(1)
        
        # Create a single test scenario for the specific file
        test_name = f"Single file test: {test_file.name}"
        test_scenarios = [(test_name, str(test_file))]
        print(f"Testing single file: {test_file}")
    else:
        # Test scenarios organized by format (WAV → AIFF → MP3 → rest → video)
        # Use script_dir for all paths so it works from any directory
        # Check both test_audio_files and test_video_files directories
        if (script_dir / "test_video_files").exists():
            test_files_prefix = str(script_dir / "test_video_files")
        else:
            test_files_prefix = str(script_dir / "test_audio_files")
        all_test_scenarios = {
            'wav': [
                ("WAV 44.1kHz 16-bit", f"{test_files_prefix}/*_44100_16bit.wav"),
                ("WAV 44.1kHz 24-bit", f"{test_files_prefix}/*_44100_24bit.wav"),
                ("WAV 44.1kHz 32-bit", f"{test_files_prefix}/*_44100_32bit.wav"),
                ("WAV 48kHz 16-bit (resampling)", f"{test_files_prefix}/*_48000_16bit.wav"),
                ("WAV 48kHz 24-bit (resampling)", f"{test_files_prefix}/*_48000_24bit.wav"),
            ],
        'aiff': [
            ("AIFF 44.1kHz 16-bit", f"{test_files_prefix}/*_44100_16bit.aiff"),
            ("AIFF 44.1kHz 24-bit", f"{test_files_prefix}/*_44100_24bit.aiff"),
            ("AIFF 44.1kHz 32-bit", f"{test_files_prefix}/*_44100_32bit.aiff"),
            ("AIFF 48kHz 16-bit (resampling)", f"{test_files_prefix}/*_48000_16bit.aiff"),
            ("AIFF 48kHz 24-bit (resampling)", f"{test_files_prefix}/*_48000_24bit.aiff"),
        ],
        'mp3': [
            ("MP3 44.1kHz 128k", f"{test_files_prefix}/*_44100_128k.mp3"),
            ("MP3 44.1kHz 192k", f"{test_files_prefix}/*_44100_192k.mp3"),
            ("MP3 44.1kHz 256k", f"{test_files_prefix}/*_44100_256k.mp3"),
            ("MP3 44.1kHz 320k", f"{test_files_prefix}/*_44100_320k.mp3"),
            ("MP3 48kHz 192k (resampling)", f"{test_files_prefix}/*_48000_192k.mp3"),
        ],
        'other': [
            ("FLAC 44.1kHz", f"{test_files_prefix}/*_44100_flac*.flac"),
            ("OGG 44.1kHz", f"{test_files_prefix}/*_44100_ogg*.ogg"),
            ("OPUS 48kHz", f"{test_files_prefix}/*_48000_opus*.opus"),
            ("AAC 44.1kHz", f"{test_files_prefix}/*_44100_aac*.m4a"),
        ],
        'video': [
            # Test all video files in test_video_files directory
            # Get all video files and create test scenarios for each
        ]
    }
        
        # Select test scenarios based on format argument
        if args.format == 'all':
            test_scenarios = []
            # Test in order: WAV → AIFF → MP3 → other audio → video
            for format_key in ['wav', 'aiff', 'mp3', 'other', 'video']:
                test_scenarios.extend(all_test_scenarios[format_key])
        elif args.format == 'video':
            # For video format, test all files in test_video_files directory
            test_scenarios = []
            video_extensions = ['.mp4', '.mkv', '.avi', '.mov', '.m4v', '.webm', '.flv']
            if test_video_dir.exists():
                # Get all video files from test_video_files directory
                video_files = []
                for ext in video_extensions:
                    video_files.extend(glob.glob(str(test_video_dir / f"*{ext}")))
                    video_files.extend(glob.glob(str(test_video_dir / f"*{ext.upper()}")))
                
                # Sort files for consistent testing order
                video_files.sort()
                
                # Create a test scenario for each video file
                for video_file in video_files:
                    video_path = Path(video_file)
                    # Create a descriptive test name from the filename
                    test_name = f"Video: {video_path.name}"
                    test_scenarios.append((test_name, str(video_path)))
                
                if not test_scenarios:
                    print(f"⚠ No video files found in {test_video_dir}")
            else:
                print(f"⚠ Video directory not found: {test_video_dir}")
        else:
            test_scenarios = all_test_scenarios.get(args.format, [])
    
    if not test_scenarios:
        print(f"✗ No test scenarios found for format: {args.format}")
        sys.exit(1)
    
    # OSC port matches the player port (default 7000)
    # Each test uses a different port to avoid conflicts
    base_port = 7000
    
    results = []
    port_counter = 0
    
    for test_name, pattern in test_scenarios:
        # Find matching file
        if args.file:
            # For single file test, pattern is already the file path
            audio_file = Path(pattern)
        elif args.format == 'video':
            # For video format, pattern is already the full file path (from test_video_files)
            audio_file = Path(pattern)
            if not audio_file.exists():
                print(f"\n⚠ Skipping {test_name}: File not found: {audio_file}")
                continue
        else:
            files = glob.glob(pattern)
            if not files:
                print(f"\n⚠ Skipping {test_name}: No matching files found")
                continue
            audio_file = Path(files[0])
        osc_port = base_port + port_counter
        
        # Run test with dynamic offset calculation from current MTC time
        if mtc_receiver:
            success = run_test(test_name, audio_file, player_binary, osc_port, mtc_sender, mtc_receiver, base_port + port_counter, use_pw_jack, args.duration)
        else:
            # Fallback: use static offset if receiver not available
            print(f"\n⚠ Using static offset (MTC receiver not available)")
            success = run_test(test_name, audio_file, player_binary, osc_port, mtc_sender, None, base_port + port_counter, use_pw_jack, args.duration)
        
        results.append((test_name, success))
        port_counter += 1
        
        # Brief pause between tests
        time.sleep(1)
    
    # Cleanup
    if mtc_receiver:
        mtc_receiver.close()
    
    # Stop MTC timecode after all tests
    print("\n[FINAL] Stopping MTC timecode...")
    mtc_sender.stop()
    
    # Summary
    print("\n" + "="*80)
    print("TEST SUMMARY")
    print("="*80)
    
    passed = sum(1 for _, success in results if success)
    total = len(results)
    failed_tests = [(name, success) for name, success in results if not success]
    
    for test_name, success in results:
        status = "✓ PASS" if success else "✗ FAIL"
        print(f"{status}: {test_name}")
    
    print("="*80)
    print(f"Total: {passed}/{total} tests passed")
    
    if failed_tests:
        print(f"\n✗ {len(failed_tests)} test(s) failed:")
        for test_name, _ in failed_tests:
            print(f"  - {test_name}")
        print(f"\nCheck log file for details: {log_file.absolute()}")
    
    # Close log file and restore stdout/stderr
    sys.stdout = original_stdout
    sys.stderr = original_stderr
    log_file_handle.close()
    
    if passed == total:
        print("✓ All tests passed!")
        print(f"Full test log: {log_file.absolute()}")
        sys.exit(0)
    else:
        print(f"✗ {total - passed} test(s) failed")
        print(f"Full test log: {log_file.absolute()}")
        sys.exit(1)

if __name__ == "__main__":
    main()
