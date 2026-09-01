cmake_minimum_required(VERSION 3.24)

project(SurfelsPreprocess)

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
    LiftingWavelet.h
    Quantizer.h
    ByteShuffle.h
    StreamPackager.h
    ../../src/DX12/Wavelet/WaveletTypes.h
)

set(shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/DX12/Shaders/Surfels.hlsl
)

set_source_files_properties(${shaders} PROPERTIES HEADER_FILE_ONLY TRUE)

source_group("Sources" FILES ${sources})
source_group("Shaders" FILES ${shaders})

add_executable(SurfelsPreprocess WIN32 ${sources} ${shaders})
target_link_libraries(SurfelsPreprocess LINK_PUBLIC Cauldron_DX12 ImGUI amd_ags d3d12 dxgi)

set_target_properties(SurfelsPreprocess PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d"
)

addManifest(SurfelsPreprocess)

if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxcompiler.dll")
    add_custom_command(
        TARGET SurfelsPreprocess
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxcompiler.dll"
            "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxil.dll"
            "${CMAKE_HOME_DIRECTORY}/bin/"
        COMMENT "Overwriting Cauldron's vendored DXC with the Windows SDK's redistributable DXC"
    )
endif()
