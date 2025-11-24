# Unit Tests for cuems-audioplayer

This directory contains comprehensive unit tests for the cuems-audioplayer project.

## Test Coverage

The test suite covers:

### 1. CommandLineParser Tests (`test_commandlineparser.cpp`)
- ✅ Constructor with various argument combinations
- ✅ Option existence checking (long and short options)
- ✅ Parameter retrieval
- ✅ Ending filename extraction
- ✅ Multiple options handling
- ✅ Boolean flags
- ✅ Edge cases (empty command line, missing values)

### 2. AudioFstream Tests (`test_audiofstream.cpp`)
- ✅ Constructor with/without filename
- ✅ File open/close operations
- ✅ File loading
- ✅ Resampling quality settings
- ✅ Target sample rate configuration
- ✅ File information accessors (size, channels, sample rate, bits)
- ✅ Seek operations (begin, current, end)
- ✅ Read operations
- ✅ EOF and error state handling
- ✅ Multiple operations sequence

### 3. AudioPlayer Tests (`test_audioplayer.cpp`)
- ✅ Static member initialization and modification
- ✅ Atomic operations on shared state
- ✅ Thread safety of atomic members
- ✅ Constant definitions
- ✅ Class structure verification

**Note**: Full AudioPlayer testing requires:
- JACK audio server running
- Real audio hardware
- MTC receiver setup
- Valid audio files

These are better suited for integration tests (see `MTC_AUTOMATED_TEST.py`).

### 4. Main Functions Tests (`test_main.cpp`)
- ✅ Copyright display function
- ✅ Usage display function
- ✅ Warranty disclaimer display
- ✅ Copyright disclaimer display
- ✅ Error code definitions and values
- ✅ Output format verification

## Building Tests

### Prerequisites

1. **Google Test Framework**: The CMakeLists.txt will automatically download Google Test if not found, or you can install it:

```bash
# Debian/Ubuntu
sudo apt-get install libgtest-dev

# Or let CMake download it automatically
```

2. **All project dependencies** (same as main project):
   - FFmpeg libraries (libavformat, libavcodec, libavutil, libswresample)
   - libsoxr
   - RtAudio
   - RtMidi
   - JACK (for integration tests, not unit tests)

### Build Instructions

```bash
cd build
cmake .. -DBUILD_TESTS=ON
make
```

Or to disable tests:
```bash
cmake .. -DBUILD_TESTS=OFF
```

## Running Tests

### Run all tests:
```bash
cd build
ctest
```

### Run with verbose output:
```bash
ctest --verbose
```

### Run specific test executable:
```bash
./test/audioplayer_tests
```

### Run with Google Test options:
```bash
./test/audioplayer_tests --gtest_list_tests
./test/audioplayer_tests --gtest_filter=CommandLineParserTest.*
./test/audioplayer_tests --gtest_repeat=10
```

## Test Structure

```
test/
├── CMakeLists.txt              # Test build configuration
├── test_commandlineparser.cpp  # CommandLineParser unit tests
├── test_audiofstream.cpp      # AudioFstream unit tests
├── test_audioplayer.cpp        # AudioPlayer unit tests
├── test_main.cpp              # Main function tests
└── README.md                  # This file
```

## Test Philosophy

### Unit Tests (this directory)
- Test individual components in isolation
- Use mocks/stubs for hardware dependencies
- Fast execution
- No external dependencies (JACK, audio hardware)

### Integration Tests (root directory)
- Test full system with real hardware
- Use `MTC_AUTOMATED_TEST.py` and `MTC_STRESS_TEST.py`
- Require JACK running
- Test MTC synchronization
- Test audio playback

## Adding New Tests

When adding new functionality:

1. **Add unit tests** in the appropriate test file
2. **Follow naming convention**: `TestClassName::TestName`
3. **Use descriptive test names** that explain what is being tested
4. **Test both success and failure cases**
5. **Test edge cases** (empty inputs, boundary values, etc.)

Example:
```cpp
TEST_F(CommandLineParserTest, NewFeatureTest) {
    // Arrange
    int argc = 3;
    char* argv[] = {
        (char*)"program",
        (char*)"--newoption",
        (char*)"value"
    };
    
    // Act
    CommandLineParser parser(argc, argv);
    
    // Assert
    EXPECT_TRUE(parser.optionExists("--newoption"));
    EXPECT_EQ(parser.getParam("--newoption"), "value");
}
```

## Continuous Integration

These tests are designed to run in CI environments:
- No hardware dependencies
- Fast execution
- Deterministic results
- Can run in parallel

## Coverage Goals

- **CommandLineParser**: 100% coverage
- **AudioFstream**: Core functionality covered (file operations, resampling)
- **AudioPlayer**: Static members and constants (hardware-dependent parts in integration tests)
- **Main functions**: Display functions and error codes

## Known Limitations

1. **AudioPlayer constructor/destructor**: Requires JACK and audio hardware
2. **AudioPlayer audioCallback**: Requires complex mocking setup
3. **AudioPlayer ProcessMessage**: Requires OSC message construction
4. **Signal handlers**: Require process forking to test properly

These limitations are addressed by integration tests in the root directory.


