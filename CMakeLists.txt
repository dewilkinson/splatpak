cmake_minimum_required(VERSION 3.24)

# Build Cauldron (the vendored framework under libs/cauldron) DX12-only so the
# Vulkan SDK is never required just to build this sample.
set(GFX_API "DX12")

project(Surfels_${GFX_API})

if(MSVC_TOOLSET_VERSION VERSION_LESS 142)
    message(FATAL_ERROR "Cannot find MSVC toolset version 142 or greater. Please make sure Visual Studio 2019 or newer is installed")
endif()

# output exe (and copied shaders/runtime dlls) to the bin directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_HOME_DIRECTORY}/bin)
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_HOME_DIRECTORY}/bin)
endforeach()

add_compile_options(/MP)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Release-with-symbols: keep the exact Release optimizer settings (this is the
# build we're chasing crashes in) but still emit a PDB so a debugger can
# resolve symbols and locals instead of a bare stack of addresses.
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

add_subdirectory(libs/cauldron)
add_subdirectory(src/DX12)
add_subdirectory(tools/SurfelsPreprocess)
add_subdirectory(tests)

# Cauldron's own CMakeLists.txt files don't set FOLDER, so its targets
# (Cauldron_DX12, Cauldron_Common, ImGUI, amd_ags, ...) land ungrouped at the
# solution root, mixed in with our own targets. Sweep every target defined
# anywhere under libs/cauldron (recursing into its nested add_subdirectory
# calls) into one "ThirdParty/Cauldron" Solution Explorer folder instead.
function(group_targets_under_folder dir folder)
    get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(subdir ${subdirs})
        group_targets_under_folder(${subdir} ${folder})
    endforeach()

    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(tgt ${targets})
        get_target_property(isImported ${tgt} IMPORTED)
        if(NOT isImported)
            set_target_properties(${tgt} PROPERTIES FOLDER ${folder})
        endif()
    endforeach()
endfunction()

group_targets_under_folder(${CMAKE_SOURCE_DIR}/libs/cauldron "ThirdParty/Cauldron")
