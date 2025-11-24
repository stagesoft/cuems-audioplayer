#!/bin/bash
# Simple script to build and run unit tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "=========================================="
echo "cuems-audioplayer Unit Tests"
echo "=========================================="

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Configure CMake with tests enabled
echo ""
echo "Configuring CMake with tests enabled..."
cmake .. -DBUILD_TESTS=ON

# Build tests
echo ""
echo "Building tests..."
make -j$(nproc)

# Run tests
echo ""
echo "Running tests..."
echo "=========================================="
ctest --output-on-failure --verbose

echo ""
echo "=========================================="
echo "Tests completed!"
echo "=========================================="

# Optionally run the test executable directly for more control
if [ "$1" == "--direct" ]; then
    echo ""
    echo "Running test executable directly..."
    ./test/audioplayer_tests
fi


