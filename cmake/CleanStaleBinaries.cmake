# CleanStaleBinaries.cmake
# Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
# SPDX-License-Identifier: Apache-2.0
#
# Run with `cmake -DBIN_DIR=<dir> -P CleanStaleBinaries.cmake` before any executable links (see the
# clean_stale_binaries target in the top-level CMakeLists.txt): deletes every executable and its
# symbols in the output directory, whatever configuration built them, so a build can only ever run
# what it just produced. A target that was not part of the build is simply absent afterwards,
# never an older copy.
if(NOT BIN_DIR)
    message(FATAL_ERROR "CleanStaleBinaries.cmake needs -DBIN_DIR=<output directory>")
endif()
file(GLOB _stale "${BIN_DIR}/*.exe" "${BIN_DIR}/*.pdb" "${BIN_DIR}/*.ilk")
foreach(_f IN LISTS _stale)
    file(REMOVE "${_f}")
endforeach()
list(LENGTH _stale _n)
if(_n GREATER 0)
    message(STATUS "clean_stale_binaries: removed ${_n} file(s) from ${BIN_DIR}")
endif()
