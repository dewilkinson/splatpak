// OcclusionVolume.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// PUBLIC BUILD NOTE: the private version of this project bakes a closed, adaptive-octree
// interior occlusion volume from the raw point cloud. That algorithm is not included in
// this open build. BuildGrid()/Bake() below keep the same struct layout and function
// signatures as the private build, so every other file in this repo compiles and links
// unchanged, but Bake() always returns zero occluder cubes: this open build has no
// interior occlusion volume feature. Everything downstream already handles that case
// (an empty result is indistinguishable from "no volume baked for this model").

#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "GstxTypes.h"

namespace Splat
{
namespace OcclusionVolume
{
    // Same layout as the private build's cached-grid struct, so any code that reads its
    // fields (e.g. the generator-tab UI showing resolution/cell size) still compiles. Most
    // fields stay at their default/empty value in this build; see OcclusionVolume.cpp.
    struct Grid
    {
        bool     valid = false;
        int32_t  nx = 0, ny = 0, nz = 0;
        int      resolution = 0;
        float    cellSize = 0.0f;
        XMFLOAT3 gMin = XMFLOAT3(0, 0, 0);
        std::vector<uint8_t>  solid;
        std::vector<XMFLOAT3> colorSum;
        std::vector<uint32_t> colorCount;
        std::vector<float>    dist;
        std::vector<float>    localMax;
        std::vector<uint8_t>  pokes;
        std::vector<float>    band;
        int      gapClose = 0;
        size_t   solidCount = 0;
        size_t   pokeCount  = 0;
    };

    // Hue / saturation / brightness grade; unused by this build's Bake() but kept in the
    // API so callers don't need conditional code.
    struct ColorGrade
    {
        float hueShift   = 0.0f;
        float saturation = 1.0f;
        float brightness = 1.0f;
    };

    // printf-style line appended to the trace string (one line per call).
    void Trace(std::string& out, const char* fmt, ...);

    // Identity in this build (colour grading only matters once there is a volume to grade).
    XMFLOAT3 GradeColor(const ColorGrade& grade, const XMFLOAT3& rgb);

    // Open build: records basic bookkeeping (bounds, a placeholder resolution) but does not
    // voxelize anything. g.valid is left false, so Bake() below is a no-op.
    void BuildGrid(
        const SplatPointArray& sourcePoints,
        const XMFLOAT3& aabbMin,
        const XMFLOAT3& aabbMax,
        Grid& g,
        std::string& trace,
        int resolutionOverride = 0,
        float shaveBiasCells = 0.0f);

    // Open build: always clears `out`, resets `outMips` to an empty table and returns with no
    // occluder cubes emitted. See the file header above -- this feature is not part of the open
    // build. (The private build fills `out` with a nested mip chain of up to maxMips volumes, mip 0
    // first, described by `outMips`; see OcclusionMipTable in WaveletTypes.h.)
    void Bake(
        const Grid& g,
        float shaveSlider,
        const ColorGrade& grade,
        std::vector<OcclusionVoxelGPU>& out,
        OcclusionMipTable& outMips,
        std::string& trace,
        uint32_t maxMips = kMaxOcclusionMips);
}
}
