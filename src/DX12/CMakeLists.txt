# Surfels_DX12 -- the standalone viewer. A thin main.cpp over surfels_core (src/SurfelsCore)
# started in Viewer mode; see that directory for everything it does.
cmake_minimum_required(VERSION 3.24)

project(Surfels_DX12)

add_executable(Surfels_DX12 WIN32 main.cpp)
target_link_libraries(Surfels_DX12 PRIVATE surfels_core)

set_target_properties(Surfels_DX12 PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Apps"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d"
)

surfels_add_dpi_manifest(Surfels_DX12)
