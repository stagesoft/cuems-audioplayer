cmake_minimum_required(VERSION 3.10)

# Project name and version
project(cuems_audioplayer VERSION 0.0)

# Debug postfix
set(CMAKE_DEBUG_POSTFIX _dbg)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(AUDIOFILE REQUIRED audiofile)
pkg_check_modules(SOXR REQUIRED soxr)

# Subdirectories
add_subdirectory(oscreceiver)
add_subdirectory(mtcreceiver)
add_subdirectory(cuemslogger)

# Executable
add_executable(audioplayer-cuems main.cpp audioplayer.cpp audiofstream.cpp commandlineparser.cpp)
set_target_properties(audioplayer-cuems PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})

# Configure file
configure_file(cuems_audioplayerConfig.h.in cuems_audioplayerConfig.h)

# Link libraries
target_link_libraries(audioplayer-cuems PUBLIC cuemslogger mtcreceiver oscreceiver)
target_link_libraries(audioplayer-cuems PUBLIC rtaudio rtmidi pthread stdc++fs)
target_link_libraries(audioplayer-cuems PUBLIC ${AUDIOFILE_LIBRARIES} ${SOXR_LIBRARIES})

# Include dirs
target_include_directories(audioplayer-cuems PUBLIC
                            "${PROJECT_SOURCE_DIR}"
                            "${PROJECT_BINARY_DIR}"
                            "${PROJECT_SOURCE_DIR}/mtcreceiver"
                            "${PROJECT_SOURCE_DIR}/oscreceiver"
                            "${PROJECT_SOURCE_DIR}/cuemslogger"
                            ${AUDIOFILE_INCLUDE_DIRS}
                            ${SOXR_INCLUDE_DIRS}
                        )
# Install rules
install(TARGETS audioplayer-cuems DESTINATION bin)
