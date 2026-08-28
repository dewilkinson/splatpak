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
    dpiawarescaling.manifest
)

set(shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Surfels.hlsl
)

source_group("Sources" FILES ${sources})
source_group("Shaders" FILES ${shaders})

# CompileShaderFromFile() looks for shaders under bin/ShaderLibDX at runtime.
copyTargetCommand("${shaders}" ${CMAKE_HOME_DIRECTORY}/bin/ShaderLibDX copied_surfels_shaders)

add_executable(Surfels_DX12 WIN32 ${sources} ${shaders})
target_link_libraries(Surfels_DX12 LINK_PUBLIC Cauldron_DX12 ImGUI)
add_dependencies(Surfels_DX12 copied_surfels_shaders)

set_target_properties(Surfels_DX12 PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin" DEBUG_POSTFIX "d")

addManifest(Surfels_DX12)
