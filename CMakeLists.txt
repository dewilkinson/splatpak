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

# Prebuilt Cauldron: libs/cauldron-prebuilt/lib/{Debug,Release}/*.lib are checked in (via Git LFS) so a
# normal clone can link Cauldron_Common/Cauldron_DX12/ImGUI directly instead of compiling ~50 files of
# vendored framework code from source. Each of the three CMakeLists.txt that build those targets
# (libs/cauldron/libs/ImGUI, libs/cauldron/src/Common, libs/cauldron/src/DX12) branch on this option to
# create an IMPORTED library pointing at the prebuilt .lib instead of an ordinary add_library(... STATIC
# <sources>); everything else about them (include dirs, link deps, shader/media copy commands) is
# unchanged either way. Falls back to building from source automatically if the prebuilt libs aren't
# present (e.g. LFS objects not pulled, or a fresh submodule bump before the libs are rebuilt), so this
# never hard-fails a build -- it can also be turned off explicitly to force a from-source build.
option(CAULDRON_USE_PREBUILT "Link prebuilt Cauldron static libraries instead of building from source" ON)
if(CAULDRON_USE_PREBUILT AND NOT EXISTS "${CMAKE_HOME_DIRECTORY}/libs/cauldron-prebuilt/lib/Release/Cauldron_DX12.lib")
    message(STATUS "CAULDRON_USE_PREBUILT requested but libs/cauldron-prebuilt/lib/Release/Cauldron_DX12.lib is missing (Git LFS objects not pulled?) -- building Cauldron from source instead.")
    set(CAULDRON_USE_PREBUILT OFF)
endif()

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

# Without this, Visual Studio picks the alphabetically-first project as the
# startup project, which is ALL_BUILD (or Docs) - neither is runnable, so F5
# fails with "Unable to start program ... access denied". Pin it to the
# client-facing app instead. Right-click another project > Set as Startup
# Project to override for a session.
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SurfelsPreprocess)

# Docs project: a build-nothing target whose only purpose is to list the
# Markdown docs in Solution Explorer so they can be opened and edited from
# inside Visual Studio. SOURCES on a custom target are never compiled.
add_custom_target(Docs SOURCES
    README.md
    docs/USER_GUIDE.md
)
source_group("" FILES README.md docs/USER_GUIDE.md)

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
