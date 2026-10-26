# SplatPak -- preprocessor and viewer in one window. A thin main.cpp over splat_core
# (src/SplatCore) started in Studio mode; see that directory for everything it does.
cmake_minimum_required(VERSION 3.24)

project(SplatPak)

add_executable(SplatPak WIN32 main.cpp)
target_link_libraries(SplatPak PRIVATE splat_core)

set_target_properties(SplatPak PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d"
)

splat_add_dpi_manifest(SplatPak)
