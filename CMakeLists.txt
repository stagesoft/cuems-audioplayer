cmake_minimum_required(VERSION 3.10)

# Project name and version
project(cuems_audioplayer VERSION 0.0.2)

# Debug postfix
set(CMAKE_DEBUG_POSTFIX _dbg)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Subdirectories
add_subdirectory(src)
