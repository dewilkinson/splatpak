# bluesec-codec -- the proprietary algorithm core (see README.md in this directory for the
# licensing boundary: this subtree is NOT covered by the repository's top-level Apache-2.0
# LICENSE). Built as a static library so every consumer (SplatLab, Surfels_DX12, the
# console test tools) links against compiled object code rather than including source:
# the .h files here are the public API/data contracts, the .cpp files hold the actual
# algorithms and never ship as source outside this library.
cmake_minimum_required(VERSION 3.24)

project(bluesec-codec C CXX)

# Brotli (MIT, libs/brotli) is compiled into this library: the payload decoder (PayloadDecode) and the
# app's payload encoder (src/SurfelsCore/PayloadCodec.h) both use it, and a host links one library.
set(BROTLI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../brotli)
file(GLOB BROTLI_SOURCES ${BROTLI_DIR}/c/common/*.c ${BROTLI_DIR}/c/dec/*.c ${BROTLI_DIR}/c/enc/*.c)

add_library(bluesec-codec STATIC
    ${BROTLI_SOURCES}
    PayloadDecode.h
    PayloadDecode.cpp
    LiftingWavelet.h
    LiftingWavelet.cpp
    ByteShuffle.h
    ByteShuffle.cpp
    OcclusionVolume.h
    OcclusionVolume.cpp
    StreamOrder.h
    StreamOrder.cpp
    DetailHeatmap.h
    DetailHeatmap.cpp
    CodecBuild.h
    CodecBuild.cpp
    SplatCodec.h
    SplatCodec.cpp
)

target_include_directories(bluesec-codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${BROTLI_DIR}/c/include)
source_group("Brotli" FILES ${BROTLI_SOURCES})

set_target_properties(bluesec-codec PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Core"
)
