# bluesec-codec -- the proprietary algorithm core (see README.md in this directory for the
# licensing boundary: this subtree is NOT covered by the repository's top-level Apache-2.0
# LICENSE). Built as a static library so every consumer (SplatLab, Surfels_DX12, the
# console test tools) links against compiled object code rather than including source:
# the .h files here are the public API/data contracts, the .cpp files hold the actual
# algorithms and never ship as source outside this library.
cmake_minimum_required(VERSION 3.24)

project(bluesec-codec)

add_library(bluesec-codec STATIC
    LiftingWavelet.h
    LiftingWavelet.cpp
    ByteShuffle.h
    ByteShuffle.cpp
    OcclusionVolume.h
    OcclusionVolume.cpp
)

target_include_directories(bluesec-codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(bluesec-codec PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Core"
)
