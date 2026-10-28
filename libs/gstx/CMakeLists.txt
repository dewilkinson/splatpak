# gstx -- the algorithm core (lifting-wavelet LOD decomposition, compression codec,
# interior occlusion volume generator, streaming order and detail grid). See README.md in
# this directory.
#
# Two ways to satisfy the `gstx` target every consumer (SplatPak, SplatPakViewer, the
# console test tools) links against:
#
#   1. PREBUILT (default when present): libs/gstx/prebuilt/{Debug,Release}/gstx.lib
#      are the real, proprietary algorithms compiled from the private repository and checked in
#      here via Git LFS (same arrangement as libs/cauldron-prebuilt; licensed under
#      prebuilt/LICENSE.txt, not Apache-2.0). Linking them gives this
#      public build the genuine wavelet, codec and occlusion-volume behaviour -- packages it
#      writes carry a baked occlusion volume, and it reads packages written by the private build.
#      The headers here are the public API/data contract those binaries were compiled against.
#
#   2. OPEN STAND-INS (fallback): if the prebuilt library is missing for this platform (LFS
#      objects not pulled, a non-MSVC/non-x64 toolchain, an MSVC older than 14.51, or GSTX_USE_PREBUILT turned
#      off), the simplified Apache-2.0 .cpp files in this directory are compiled instead. They
#      keep the same signatures, but the wavelet is plain decimation, the codec is transpose-only,
#      the occlusion generator bakes nothing and the streaming order is plain -- see README.md for
#      exactly what differs. The app shows a banner row naming the missing pieces (CodecBuild.h).
cmake_minimum_required(VERSION 3.24)

project(gstx C CXX)

# Brotli (MIT, libs/brotli) is part of this library in both forms: compiled into the prebuilt .lib, and
# compiled here with the stand-ins. Its headers are public either way (src/SplatCore/PayloadCodec.h uses them).
set(BROTLI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../brotli)

option(GSTX_USE_PREBUILT "Link the prebuilt proprietary gstx.lib instead of compiling the open stand-ins" ON)

set(GSTX_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt")
set(GSTX_PREBUILT_RELEASE "${GSTX_PREBUILT_DIR}/Release/gstx.lib")
set(GSTX_PREBUILT_DEBUG   "${GSTX_PREBUILT_DIR}/Debug/gstx.lib")

if(GSTX_USE_PREBUILT)
    # The .lib files are MSVC x64 static libraries (compiled /MD for Release, /MDd for Debug --
    # the same CRT the rest of this project uses), so only an MSVC x64 build can consume them.
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(STATUS "gstx: prebuilt library is MSVC x64 only -- compiling the open stand-ins for this toolchain instead.")
        set(GSTX_USE_PREBUILT OFF)
    elseif(MSVC_VERSION LESS 1951)
        # Built with MSVC 14.51 (VS 2026); it references STL helpers such as __std_rotate that
        # older toolsets' runtime libraries lack, so linking with e.g. VS 2022 fails.
        message(STATUS "gstx: prebuilt library needs MSVC 14.51+ (Visual Studio 2026); this toolset is MSVC_VERSION ${MSVC_VERSION} -- compiling the open stand-ins instead.")
        set(GSTX_USE_PREBUILT OFF)
    elseif(NOT EXISTS "${GSTX_PREBUILT_RELEASE}" OR NOT EXISTS "${GSTX_PREBUILT_DEBUG}")
        message(STATUS "gstx: prebuilt/{Debug,Release}/gstx.lib missing (Git LFS objects not pulled?) -- compiling the open stand-ins instead. Run 'git lfs pull' and re-configure to link the real codec.")
        set(GSTX_USE_PREBUILT OFF)
    else()
        # A Git LFS pointer file that was never smudged is a ~130-byte text file, not a library;
        # linking it fails with an unhelpful "invalid or corrupt file". Catch that up front.
        file(SIZE "${GSTX_PREBUILT_RELEASE}" _gstxReleaseBytes)
        if(_gstxReleaseBytes LESS 4096)
            message(STATUS "gstx: prebuilt/Release/gstx.lib is a Git LFS pointer, not the library (run 'git lfs pull') -- compiling the open stand-ins instead.")
            set(GSTX_USE_PREBUILT OFF)
        endif()
    endif()
endif()

if(GSTX_USE_PREBUILT)
    message(STATUS "gstx: linking the prebuilt proprietary library from ${GSTX_PREBUILT_DIR}")
    add_library(gstx STATIC IMPORTED GLOBAL)
    set_target_properties(gstx PROPERTIES
        IMPORTED_LOCATION_DEBUG          "${GSTX_PREBUILT_DEBUG}"
        IMPORTED_LOCATION_RELEASE        "${GSTX_PREBUILT_RELEASE}"
        IMPORTED_LOCATION_MINSIZEREL     "${GSTX_PREBUILT_RELEASE}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${GSTX_PREBUILT_RELEASE}"
        # Single-config generators (Ninja, Makefiles) consult IMPORTED_LOCATION when no
        # per-config location matches; MSVC single-config Debug builds still pick the entry above.
        IMPORTED_LOCATION                "${GSTX_PREBUILT_RELEASE}")
    # IMPORTED targets only accept INTERFACE here (nothing of gstx itself is compiled);
    # consumers see the include directory exactly as they would from PUBLIC on a source target.
    target_include_directories(gstx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} ${BROTLI_DIR}/c/include)
else()
    message(STATUS "gstx: compiling the open stand-in implementation (no occlusion volume, decimation LOD, transpose-only codec, plain streaming order, no shadow mesh)")
    file(GLOB BROTLI_SOURCES ${BROTLI_DIR}/c/common/*.c ${BROTLI_DIR}/c/dec/*.c ${BROTLI_DIR}/c/enc/*.c)
    add_library(gstx STATIC
        ${BROTLI_SOURCES}
        PayloadDecode.h
        PayloadDecode.cpp
        LiftingWavelet.h
        LiftingWavelet.cpp
        ByteShuffle.h
        ByteShuffle.cpp
        OcclusionVolume.h
        OcclusionVolume.cpp
        StreamOrder.h
        StreamOrder.cpp
        StreamRuntime.h
        StreamRuntime.cpp
        DetailHeatmap.h
        DetailHeatmap.cpp
        CodecBuild.h
        CodecBuild.cpp
        SplatCodec.h
        SplatCodec.cpp
        ColourTransform.h
        SHCoding.h
        ShadowMesh.h
        ShadowMeshBuilder.h
        ShadowMesh.cpp
        ShadowMeshCodec.h
        ShadowMeshCodec.cpp
    )

    target_include_directories(gstx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${BROTLI_DIR}/c/include)

    set_target_properties(gstx PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        FOLDER "Core"
    )
endif()
