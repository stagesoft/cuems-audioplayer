#!/bin/bash
# MTC Integration Test Script
# Tests MTC synchronization with cuems-audioplayer

set -e

BUILD_DIR="build"
PLAYER="$BUILD_DIR/audioplayer-cuems_dbg"
TEST_FILE="test_audio_files/music_44100_16bit.wav"
PORT=7000

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=========================================="
echo "MTC Integration Test"
echo "=========================================="

# Check JACK
if ! pgrep -x jackd > /dev/null; then
    echo -e "${RED}✗ JACK is not running${NC}"
    echo "Please start JACK: jackd -d alsa -r 44100"
    exit 1
fi
echo -e "${GREEN}✓ JACK is running${NC}"

# Check player binary
if [ ! -f "$PLAYER" ]; then
    echo -e "${YELLOW}⚠ Player binary not found: $PLAYER${NC}"
    PLAYER="$BUILD_DIR/audioplayer-cuems"
    if [ ! -f "$PLAYER" ]; then
        echo -e "${RED}✗ Player binary not found${NC}"
        exit 1
    fi
fi
echo -e "${GREEN}✓ Player binary found: $PLAYER${NC}"

# Check test file
if [ ! -f "$TEST_FILE" ]; then
    echo -e "${YELLOW}⚠ Test file not found: $TEST_FILE${NC}"
    echo "Generating test files..."
    ./generate_test_files.sh || {
        echo -e "${RED}✗ Failed to generate test files${NC}"
        exit 1
    }
fi
echo -e "${GREEN}✓ Test file found: $TEST_FILE${NC}"

echo ""
echo "Starting player with MTC following..."
echo "Player command: $PLAYER -f $TEST_FILE -p $PORT -m"

# Start player in background
$PLAYER -f "$TEST_FILE" -p $PORT -m &
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

