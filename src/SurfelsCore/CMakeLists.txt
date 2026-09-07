# surfels_core -- everything SplatLab and the standalone Surfels_DX12 viewer share: the app
# shell (SurfelsApp, with its Studio / Viewer mode), the renderer, the loaders, the octree /
# quantizer / packager, the .sflw types and validators, the shaders, and the WinMain body
# (AppEntry). Each executable is a few-line main.cpp that links this library, so a change made
# here reaches both apps at once instead of having to be made twice.
#
# The console test tools include headers from this directory directly (they need no GPU or
# Cauldron) and link only bluesec-codec; see tests/CMakeLists.txt.
cmake_minimum_required(VERSION 3.24)

project(SurfelsCore)

# Pulls in copyTargetCommand() from the Cauldron framework.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../libs/cauldron/common.cmake)

set(sources
    stdafx.h
    stdafx.cpp
    AppEntry.h
    AppEntry.cpp
    SurfelsApp.h
    SurfelsApp.cpp
    SurfelsRenderer.h
    SurfelsRenderer.cpp
    WaveletTypes.h
    PLYLoader.h
    SPLATLoader.h
    SOGLoader.h
    SpatialOctree.h
    Quantizer.h
    QualityMetrics.h
    StreamPackager.h
    SyntheticGenerator.h
    dpiawarescaling.manifest
)

# CompileShaderFromFile() looks for these under bin/ShaderLibDX at runtime.
set(shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Surfels.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/GPURadixSortCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/CullSurfelsCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/BuildIndirectArgsCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/SilhouetteEdgeExtractCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TemporalFilterCS.hlsl
)
set_source_files_properties(${shaders} PROPERTIES HEADER_FILE_ONLY TRUE)

source_group("Sources" FILES ${sources})
source_group("Shaders" FILES ${shaders})

copyTargetCommand("${shaders}" ${CMAKE_HOME_DIRECTORY}/bin/ShaderLibDX copied_surfels_core_shaders)

add_library(surfels_core STATIC ${sources} ${shaders})
target_include_directories(surfels_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Cauldron_DX12 doesn't itself propagate the D3D12/DXGI system libs or the AGS SDK it calls
# into, so they are listed here once and reach both executables through this target. DbgHelp
# backs the minidump writer in AppEntry.cpp.
target_link_libraries(surfels_core PUBLIC Cauldron_DX12 ImGUI amd_ags d3d12 dxgi DbgHelp bluesec-codec)
add_dependencies(surfels_core copied_surfels_core_shaders)

set_target_properties(surfels_core PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Core"
)

# Embeds the DPI-awareness manifest into an executable. Every Cauldron sample defines this
# helper locally; here it is defined once for both apps. CMake functions are global, so the
# manifest path is captured now (CMAKE_CURRENT_SOURCE_DIR inside a function is the caller's).
set(SURFELS_DPI_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/dpiawarescaling.manifest" CACHE INTERNAL "DPI-awareness manifest shared by the Surfels executables")
function(surfels_add_dpi_manifest PROJECT_NAME)
    if(MSVC)
        file(TO_NATIVE_PATH "${SURFELS_DPI_MANIFEST}" _manifestNative)
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD
            COMMAND "mt.exe" -manifest \"${_manifestNative}\" -inputresource:\"$<TARGET_FILE:${PROJECT_NAME}>\"\;\#1 -outputresource:\"$<TARGET_FILE:${PROJECT_NAME}>\"\;\#1
            COMMENT "Adding display aware manifest to ${PROJECT_NAME}..."
        )
    endif()
endfunction()
