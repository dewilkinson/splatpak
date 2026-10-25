# Brotli 1.1.0 (https://github.com/google/brotli, MIT; see LICENSE): the entropy stage of every
# .sflw payload written since format version 9 (src/SurfelsCore/PayloadCodec.h). A vendored copy of
# the upstream c/common, c/dec, c/enc and c/include directories, nothing else of its tree, built as
# one static library that surfels_core and the console tools link.
cmake_minimum_required(VERSION 3.24)

project(brotli C)

file(GLOB BROTLI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/c/common/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/c/dec/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/c/enc/*.c
)

add_library(brotli STATIC ${BROTLI_SOURCES})
target_include_directories(brotli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/c/include)

set_target_properties(brotli PROPERTIES
    C_STANDARD 99
    FOLDER "ThirdParty"
)
