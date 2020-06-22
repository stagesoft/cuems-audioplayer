cmake_minimum_required(VERSION 3.10)

# set the project name
project(cuems_audioplayer VERSION 0.0)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(oscreceiver)
add_subdirectory(mtcreceiver)
add_subdirectory(cuemslogger)

# add the executable
add_executable(audioplayer-cuems main.cpp audioplayer.cpp audiofstream.cpp commandlineparser.cpp)

configure_file(cuems_audioplayerConfig.h.in cuems_audioplayerConfig.h)

#target_link_directories(audioplayer-cuems PRIVATE
#                            "${PROJECT_SOURCE_DIR}/mtcreceiver"
#                            "${PROJECT_SOURCE_DIR}/oscreceiver"
#                            "${PROJECT_SOURCE_DIR}/cuemslogger"
#                            )

target_link_libraries(audioplayer-cuems PUBLIC cuemslogger mtcreceiver oscreceiver)
target_link_libraries(audioplayer-cuems PUBLIC rtaudio rtmidi pthread stdc++fs)

# add the binary tree to the search path for include files
# so that we will find TutorialConfig.h
target_include_directories(audioplayer-cuems PUBLIC
                            "${PROJECT_SOURCE_DIR}"
                            "${PROJECT_BINARY_DIR}"
                            "${PROJECT_SOURCE_DIR}/mtcreceiver"
                            "${PROJECT_SOURCE_DIR}/oscreceiver"
                            "${PROJECT_SOURCE_DIR}/cuemslogger"
                        )
