# SplatLabViewer -- the standalone viewer. A thin main.cpp over splat_core (src/SplatCore)
# started in Viewer mode; see that directory for everything it does.
cmake_minimum_required(VERSION 3.24)

project(SplatLabViewer)

add_executable(SplatLabViewer WIN32 main.cpp)
target_link_libraries(SplatLabViewer PRIVATE splat_core)

set_target_properties(SplatLabViewer PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Apps"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d"
)

splat_add_dpi_manifest(SplatLabViewer)
