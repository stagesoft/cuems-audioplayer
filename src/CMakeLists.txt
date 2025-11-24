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
pkg_check_modules(SOXR REQUIRED soxr)

# Subdirectories
add_subdirectory(cuems-mediadecoder)
add_subdirectory(oscreceiver)
add_subdirectory(mtcreceiver)
add_subdirectory(cuemslogger)

# Executable
add_executable(audioplayer-cuems main.cpp audioplayer.cpp audiofstream.cpp commandlineparser.cpp)
set_target_properties(audioplayer-cuems PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})

# Configure file
configure_file(cuems_audioplayerConfig.h.in cuems_audioplayerConfig.h)

# Link libraries
target_link_libraries(audioplayer-cuems PUBLIC cuems-mediadecoder cuemslogger mtcreceiver oscreceiver)
target_link_libraries(audioplayer-cuems PUBLIC rtaudio rtmidi pthread stdc++fs)
target_link_libraries(audioplayer-cuems PUBLIC ${SOXR_LIBRARIES})

# Include dirs
target_include_directories(audioplayer-cuems PUBLIC
                            "${PROJECT_SOURCE_DIR}"
                            "${PROJECT_BINARY_DIR}"
                            "${PROJECT_SOURCE_DIR}/mtcreceiver"
                            "${PROJECT_SOURCE_DIR}/oscreceiver"
                            "${PROJECT_SOURCE_DIR}/cuemslogger"
                            ${SOXR_INCLUDE_DIRS}
                        )
# Install rules
install(TARGETS audioplayer-cuems DESTINATION bin)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    get_filename_component(CUEMS_PROJECT_ROOT "${CMAKE_SOURCE_DIR}" DIRECTORY)
else()
    set(CUEMS_PROJECT_ROOT "${CMAKE_SOURCE_DIR}")
endif()

if(NOT TARGET distclean)
    add_custom_target(distclean
        COMMAND ${CMAKE_COMMAND}
                -DPROJECT_BINARY_DIR=${CMAKE_BINARY_DIR}
                -DPROJECT_SOURCE_DIR=${CUEMS_PROJECT_ROOT}
                -P ${CUEMS_PROJECT_ROOT}/cmake/distclean.cmake
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Removing all generated build and test artifacts"
    )
endif()
