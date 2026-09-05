cmake_minimum_required(VERSION 3.24)

project(SurfelLab)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../libs/cauldron/common.cmake)

function(addManifest PROJECT_NAME)
    if(MSVC)
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD
            COMMAND "mt.exe" -manifest \"${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/dpiawarescaling.manifest\" -inputresource:\"$<TARGET_FILE:${PROJECT_NAME}>\"\;\#1 -outputresource:\"$<TARGET_FILE:${PROJECT_NAME}>\"\;\#1
            COMMENT "Adding display aware manifest to ${PROJECT_NAME}..."
        )
    endif()
endfunction()

set(sources
    main.cpp
    PreprocessApp.h
    PreprocessApp.cpp
    PreprocessRenderer.h
    PreprocessRenderer.cpp
    SyntheticGenerator.h
    PLYLoader.h
    SPLATLoader.h
    SpatialOctree.h
    Quantizer.h
    StreamPackager.h
    ../../src/DX12/Wavelet/WaveletTypes.h
)

set(shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/Shaders/Surfels.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/Shaders/GPURadixSortCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/Shaders/CullSurfelsCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/Shaders/BuildIndirectArgsCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/Shaders/SilhouetteEdgeExtractCS.hlsl
)

set_source_files_properties(${shaders} PROPERTIES HEADER_FILE_ONLY TRUE)

source_group("Sources" FILES ${sources})
source_group("Shaders" FILES ${shaders})

copyTargetCommand("${shaders}" ${CMAKE_HOME_DIRECTORY}/bin/ShaderLibDX copied_preprocess_shaders)

add_executable(SurfelLab WIN32 ${sources} ${shaders})
target_link_libraries(SurfelLab LINK_PUBLIC Cauldron_DX12 ImGUI amd_ags d3d12 dxgi SurfelsCore)
add_dependencies(SurfelLab copied_preprocess_shaders)

set_target_properties(SurfelLab PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d"
)

addManifest(SurfelLab)

add_custom_command(
    TARGET SurfelLab
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/Shaders"
        "${CMAKE_HOME_DIRECTORY}/bin/ShaderLibDX"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$ENV{LOCALAPPDATA}/AMD/Cauldron/ShaderCacheDX"
    COMMENT "Refreshing shaders and clearing Cauldron shader cache..."
)

if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxcompiler.dll")
    add_custom_command(
        TARGET SurfelLab
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxcompiler.dll"
            "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxil.dll"
            "${CMAKE_HOME_DIRECTORY}/bin/"
        COMMENT "Overwriting Cauldron's vendored DXC with the Windows SDK's redistributable DXC"
    )
endif()
