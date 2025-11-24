#!/bin/bash
# MTC Integration Test Script
# Tests MTC synchronization with cuems-audioplayer

set -e

# Get script directory for path resolution
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="$SCRIPT_DIR/build"
TEST_FILE_DIR="$SCRIPT_DIR/test_audio_files"
TEST_FILE="$TEST_FILE_DIR/music_44100_16bit.wav"
PORT=7000

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=========================================="
echo "MTC Integration Test"
echo "=========================================="

# Check JACK or pw-jack
USE_PW_JACK=false
if pgrep -x jackd > /dev/null; then
    echo -e "${GREEN}✓ JACK is running${NC}"
elif command -v pw-jack > /dev/null 2>&1; then
    # Check if PipeWire is running
    if pw-cli info > /dev/null 2>&1; then
        echo -e "${YELLOW}⚠ Warning: JACK daemon not running, using pw-jack (PipeWire JACK compatibility layer)${NC}"
        echo -e "${GREEN}✓ Using pw-jack as fallback${NC}"
        USE_PW_JACK=true
    else
        echo -e "${YELLOW}⚠ Warning: JACK daemon not running, pw-jack found but PipeWire may not be running${NC}"
        USE_PW_JACK=true
    fi
else
    echo -e "${RED}✗ JACK is not running and pw-jack is not available${NC}"
    echo "Please start JACK: jackd -d alsa -r 44100"
    echo "Or install PipeWire with JACK support for pw-jack fallback"
    exit 1
fi

# Find player binary (check multiple locations)
PLAYER=""
for player_path in \
    "$BUILD_DIR/src/audioplayer-cuems_dbg" \
    "$BUILD_DIR/src/audioplayer-cuems" \
    "$BUILD_DIR/audioplayer-cuems_dbg" \
    "$BUILD_DIR/audioplayer-cuems" \
    "$SCRIPT_DIR/build/src/audioplayer-cuems_dbg" \
    "$SCRIPT_DIR/build/src/audioplayer-cuems" \
    "$SCRIPT_DIR/build/audioplayer-cuems_dbg" \
    "$SCRIPT_DIR/build/audioplayer-cuems"
do
    if [ -f "$player_path" ]; then
        PLAYER="$player_path"
        break
    fi
done

if [ -z "$PLAYER" ]; then
    echo -e "${RED}✗ Player binary not found${NC}"
    echo "Please build the project first: cd build && cmake .. && make"
    exit 1
fi
echo -e "${GREEN}✓ Player binary found: $PLAYER${NC}"

# Check test file
if [ ! -f "$TEST_FILE" ]; then
    echo -e "${YELLOW}⚠ Test file not found: $TEST_FILE${NC}"
    echo "Generating test files..."
    if [ -f "$SCRIPT_DIR/generate_test_files.sh" ]; then
        cd "$SCRIPT_DIR"
        ./generate_test_files.sh || {
            echo -e "${RED}✗ Failed to generate test files${NC}"
            exit 1
        }
    else
        echo -e "${RED}✗ Test file not found and generate_test_files.sh not available${NC}"
        exit 1
    fi
fi
echo -e "${GREEN}✓ Test file found: $TEST_FILE${NC}"

echo ""
echo "Starting player with MTC following..."

# Build player command array
if [ "$USE_PW_JACK" = true ]; then
    PLAYER_CMD=(pw-jack "$PLAYER" -f "$TEST_FILE" -p "$PORT" -m)
    echo "Player command: pw-jack $PLAYER -f \"$TEST_FILE\" -p $PORT -m"
else
    PLAYER_CMD=("$PLAYER" -f "$TEST_FILE" -p "$PORT" -m)
    echo "Player command: $PLAYER -f \"$TEST_FILE\" -p $PORT -m"
fi

# Start player in background
"${PLAYER_CMD[@]}" &
PLAYER_PID=$!

sleep 2

# Check if player is still running
if ! kill -0 $PLAYER_PID 2>/dev/null; then
    echo -e "${RED}✗ Player exited unexpectedly${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Player started (PID: $PLAYER_PID)${NC}"
echo ""
echo "Use the Python MTC test script for comprehensive testing:"
echo "  ./MTC_AUTOMATED_TEST.py"
echo ""
echo "Or manually test with libmtcmaster Python interface"
echo ""
echo "Press Ctrl+C to stop the player"

# Wait for user interrupt
trap "kill $PLAYER_PID 2>/dev/null; exit" INT TERM
wait $PLAYER_PID

