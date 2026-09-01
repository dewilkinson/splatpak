cmake_minimum_required(VERSION 3.24)

project(SurfelsTests)

add_executable(TestGPUSort ${CMAKE_CURRENT_SOURCE_DIR}/TestGPUSort.cpp)
target_link_libraries(TestGPUSort LINK_PUBLIC d3d12 dxgi d3dcompiler)
set_target_properties(TestGPUSort PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tests"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d"
)

add_executable(TestBitonicCPU ${CMAKE_CURRENT_SOURCE_DIR}/TestBitonicCPU.cpp)
set_target_properties(TestBitonicCPU PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tests"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d"
)

add_executable(CompressVenus ${CMAKE_CURRENT_SOURCE_DIR}/CompressVenus.cpp)
set_target_properties(CompressVenus PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tests"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}"
    DEBUG_POSTFIX "d"
)

