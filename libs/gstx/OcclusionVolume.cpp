// OcclusionVolume.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Open-build stand-in: no interior occlusion volume feature (see the "PUBLIC BUILD NOTE"
// in OcclusionVolume.h). BuildGrid() does just enough bookkeeping to report a resolution
// and bounds through the trace log; Bake() always returns zero cubes.

#include "OcclusionVolume.h"
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <cmath>

namespace Splat
{
namespace OcclusionVolume
{
    void Trace(std::string& out, const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        out += buf;
        out += '\n';
    }

    XMFLOAT3 GradeColor(const ColorGrade& /*grade*/, const XMFLOAT3& rgb)
    {
        return rgb; // Identity: no volume to grade in this build
    }

    void BuildGrid(const SplatPointArray& sourcePoints, const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax, Grid& g, std::string& trace, int resolutionOverride, float /*shaveBiasCells*/)
    {
        g = Grid{};
        if (sourcePoints.empty()) return;

        g.gMin = aabbMin;
        float maxExtent = std::max(1e-4f, std::max(aabbMax.x - aabbMin.x, std::max(aabbMax.y - aabbMin.y, aabbMax.z - aabbMin.z)));
        g.resolution = resolutionOverride > 0 ? resolutionOverride : 64;
        g.cellSize = maxExtent / (float)g.resolution;
        g.valid = false; // No solid was ever computed in this build -- see Bake() below

        Trace(trace, "BuildOcclusionGrid: open build has no occlusion volume feature (%zu points, resolution=%d not used)",
            sourcePoints.size(), g.resolution);
    }

    void Bake(const Grid& /*g*/, float /*shaveSlider*/, const ColorGrade& /*grade*/, std::vector<OcclusionVoxelGPU>& out, OcclusionMipTable& outMips, std::string& trace, uint32_t /*maxMips*/)
    {
        out.clear();
        outMips = OcclusionMipTable{};
        Trace(trace, "BuildOcclusionVolume: open build has no occlusion volume feature -- 0 cubes");
    }
}
}
