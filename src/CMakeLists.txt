cmake_minimum_required(VERSION 3.10)

# Project name and version
project(cuems_audioplayer VERSION 0.0)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Subdirectories
add_subdirectory(oscreceiver)
add_subdirectory(mtcreceiver)
add_subdirectory(cuemslogger)

# Executable
add_executable(audioplayer-cuems main.cpp audioplayer.cpp audiofstream.cpp commandlineparser.cpp)

# Configure file
configure_file(cuems_audioplayerConfig.h.in cuems_audioplayerConfig.h)

# Link libraries
target_link_libraries(audioplayer-cuems PUBLIC cuemslogger mtcreceiver oscreceiver)
target_link_libraries(audioplayer-cuems PUBLIC rtaudio rtmidi pthread stdc++fs)

# Include dirs
target_include_directories(audioplayer-cuems PUBLIC
                            "${PROJECT_SOURCE_DIR}"
                            "${PROJECT_BINARY_DIR}"
                            "${PROJECT_SOURCE_DIR}/mtcreceiver"
                            "${PROJECT_SOURCE_DIR}/oscreceiver"
                            "${PROJECT_SOURCE_DIR}/cuemslogger"
                        )
# Install rules
install(TARGETS audioplayer-cuems DESTINATION bin)
