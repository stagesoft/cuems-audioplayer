cmake_minimum_required(VERSION 3.10)

# Find Google Test
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    # Try to find it via pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTEST QUIET gtest)
    
    if(NOT GTEST_FOUND)
        # Download and build Google Test if not found
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG release-1.12.1
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

# Find required packages for tests
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SOXR REQUIRED soxr)

# Test executable
add_executable(audioplayer_tests
    test_commandlineparser.cpp
    test_audiofstream.cpp
    test_audioplayer.cpp
    test_main.cpp
    # Source files needed for testing
    ../src/commandlineparser.cpp
    ../src/audiofstream.cpp
    ../src/audioplayer.cpp
    # Use test version of main functions (without main())
    main_functions.cpp
)

# Link test libraries
target_link_libraries(audioplayer_tests PRIVATE
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

# Link project libraries
# Note: These are built in src/CMakeLists.txt subdirectories
target_link_libraries(audioplayer_tests PRIVATE
    cuems-mediadecoder
    cuemslogger
    mtcreceiver
    oscreceiver
    rtaudio
    rtmidi
    pthread
    stdc++fs
    ${SWRESAMPLE_LIBRARIES}
    ${SOXR_LIBRARIES}
)

# Include directories
target_include_directories(audioplayer_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/mtcreceiver"
    "${CMAKE_SOURCE_DIR}/src/oscreceiver"
    "${CMAKE_SOURCE_DIR}/src/cuemslogger"
    "${CMAKE_BINARY_DIR}/src"  # For generated config header
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${SOXR_INCLUDE_DIRS}
)

# Add test to CTest
include(GoogleTest)
gtest_discover_tests(audioplayer_tests)

