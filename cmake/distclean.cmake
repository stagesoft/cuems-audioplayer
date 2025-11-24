cmake_minimum_required(VERSION 3.10)

# Distclean script removes all generated files inside the build directory and
# optional test result logs that tend to accumulate in the source tree.

if(NOT DEFINED PROJECT_BINARY_DIR)
    message(FATAL_ERROR "PROJECT_BINARY_DIR variable not defined")
endif()

if(NOT EXISTS "${PROJECT_BINARY_DIR}")
    message(STATUS "Distclean: build directory '${PROJECT_BINARY_DIR}' does not exist. Nothing to do.")
    return()
endif()

message(STATUS "Distclean: removing generated files from '${PROJECT_BINARY_DIR}'")

# Remove everything under the build directory, but keep the directory itself so
# that the current working directory (which may be inside build/) remains valid.
file(GLOB _distclean_children "${PROJECT_BINARY_DIR}/*")
foreach(child ${_distclean_children})
    file(REMOVE_RECURSE "${child}")
endforeach()

# Also remove common test result logs that live in the source tree.
if(DEFINED PROJECT_SOURCE_DIR AND EXISTS "${PROJECT_SOURCE_DIR}")
    file(GLOB _test_logs "${PROJECT_SOURCE_DIR}/MTC_TEST_RESULTS_*.log")
    foreach(log_file ${_test_logs})
        file(REMOVE "${log_file}")
    endforeach()
endif()

message(STATUS "Distclean: done.")

