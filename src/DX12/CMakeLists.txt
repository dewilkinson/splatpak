# Pulls in copyTargetCommand() from the Cauldron framework.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../libs/cauldron/common.cmake)

# addManifest() is not part of Cauldron itself -- every Cauldron sample project
# (glTFSample, FidelityFX-CAS, ...) defines this same helper locally to embed the
# DPI-awareness manifest into the built exe.
function(addManifest PROJECT_NAME)
    if(MSVC)
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD
            COMMAND "mt.exe" -manifest \"${CMAKE_CURRENT_SOURCE_DIR}\\dpiawarescaling.manifest\" -inputresource:\"$<TARGET_FILE:${PROJECT_NAME}>\"\;\#1 -outputresource:\"$<TARGET_FILE:${PROJECT_NAME}>\"\;\#1
            COMMENT "Adding display aware manifest..."
        )
    endif()
endfunction()

set(sources
    stdafx.h
    stdafx.cpp
    SurfelsSample.h
    SurfelsSample.cpp
    SurfelsRenderer.h
    SurfelsRenderer.cpp
    Wavelet/WaveletTypes.h
    Wavelet/StreamingManager.h
    Wavelet/LODSelector.h
    Wavelet/ZstdDecompressor.h
    dpiawarescaling.manifest
)

set(shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Surfels.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/GPURadixSortCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/CullSurfelsCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/BuildIndirectArgsCS.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/SilhouetteEdgeExtractCS.hlsl
)

source_group("Sources" FILES ${sources})
source_group("Shaders" FILES ${shaders})

# CompileShaderFromFile() looks for shaders under bin/ShaderLibDX at runtime.
copyTargetCommand("${shaders}" ${CMAKE_HOME_DIRECTORY}/bin/ShaderLibDX copied_surfels_shaders)

# Cauldron_DX12 doesn't itself propagate the D3D12/DXGI system libs or the AGS SDK
# it calls into (Device.cpp uses agsInitialize/agsDriverExtensionsDX12_* directly) --
# every consumer executable has to link them explicitly.
add_executable(Surfels_DX12 WIN32 ${sources} ${shaders})
target_link_libraries(Surfels_DX12 LINK_PUBLIC Cauldron_DX12 ImGUI amd_ags d3d12 dxgi)
add_dependencies(Surfels_DX12 copied_surfels_shaders)

set_target_properties(Surfels_DX12 PROPERTIES 
    FOLDER "Apps"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin" 
    DEBUG_POSTFIX "d"
)

addManifest(Surfels_DX12)

add_custom_command(
    TARGET Surfels_DX12
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
        "${CMAKE_HOME_DIRECTORY}/bin/ShaderLibDX"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$ENV{LOCALAPPDATA}/AMD/Cauldron/ShaderCacheDX"
    COMMENT "Refreshing shaders and clearing Cauldron shader cache..."
)

# Cauldron vendors an old DXC build (libs/cauldron/libs/DXC, dxcompiler.dll v1.6.2106.3
# from 2021) that silently fails to compile even trivial shaders on newer Windows --
# Compile() returns an empty result instead of throwing, and a use-after-free in
# Cauldron's own error-reporting path (DXCHelper.cpp's DXCompileToDXO, which calls
# pLibrary->GetBlobAsUtf8 after already releasing pLibrary) swallows the real DXC error,
# so it just hits `assert(SpvSize != 0)` in ShaderCompilerHelper.cpp with no explanation.
# The Windows SDK's redistributable DXC (Windows Kits/10/Redist/D3D) compiles the exact
# same shader without issue, so overwrite the vendored DLLs with it after linking.
if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxcompiler.dll")
    add_custom_command(
        TARGET Surfels_DX12
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxcompiler.dll"
            "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxil.dll"
            "${CMAKE_HOME_DIRECTORY}/bin/"
        COMMENT "Overwriting Cauldron's vendored DXC with the Windows SDK's redistributable DXC"
    )
else()
    message(WARNING "Windows SDK DXC redist not found at the expected path -- if shader compilation asserts with SpvSize != 0, manually copy a newer dxcompiler.dll/dxil.dll (e.g. from a Windows Kits install's Redist/D3D/x64) over bin/dxcompiler.dll and bin/dxil.dll.")
endif()
