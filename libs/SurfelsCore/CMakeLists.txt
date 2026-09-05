# SurfelsCore -- the proprietary algorithm core (see README.md in this directory for the
# licensing boundary: this subtree is NOT covered by the repository's top-level Apache-2.0
# LICENSE). Built as a static library so every consumer (SurfelLab, Surfels_DX12, the
# console test tools) links against compiled object code rather than including source:
# the .h files here are the public API/data contracts, the .cpp files hold the actual
# algorithms and never ship as source outside this library.
cmake_minimum_required(VERSION 3.24)

project(SurfelsCore)

add_library(SurfelsCore STATIC
    LiftingWavelet.h
    LiftingWavelet.cpp
    ByteShuffle.h
    ByteShuffle.cpp
    OcclusionVolume.h
    OcclusionVolume.cpp
)

target_include_directories(SurfelsCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(SurfelsCore PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Core"
)
