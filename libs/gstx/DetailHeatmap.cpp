// DetailHeatmap.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Open-build detail grid: occupancy only. Every cell that holds at least one point gets the same
// mid-range score, so a package written by this build still carries a valid grid (and reads back
// with the same code), but the streaming order it drives is plain -- there is no ranking of one
// region over another. The private build scores cells; see the note in DetailHeatmap.h.

#include "DetailHeatmap.h"

namespace Splat
{
    DetailGrid DetailGrid::Build(const SplatPointArray& points, const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax, int longestAxisCells)
    {
        DetailGrid g;
        if (points.empty()) return g;
        XMFLOAT3 ext(std::max(1e-4f, aabbMax.x - aabbMin.x), std::max(1e-4f, aabbMax.y - aabbMin.y), std::max(1e-4f, aabbMax.z - aabbMin.z));
        const float maxExtent = std::max(ext.x, std::max(ext.y, ext.z));
        longestAxisCells = std::max(4, std::min(96, longestAxisCells));
        g.cellSize = maxExtent / (float)longestAxisCells;
        g.gMin = aabbMin;
        g.nx = (uint32_t)std::max(1, (int)std::ceil(ext.x / g.cellSize));
        g.ny = (uint32_t)std::max(1, (int)std::ceil(ext.y / g.cellSize));
        g.nz = (uint32_t)std::max(1, (int)std::ceil(ext.z / g.cellSize));
        g.score.assign((size_t)g.nx * g.ny * g.nz, 0);
        for (const auto& p : points)
        {
            int32_t ix, iy, iz;
            g.CellOf(p.position, ix, iy, iz);
            uint8_t& s = g.score[g.Index((uint32_t)ix, (uint32_t)iy, (uint32_t)iz)];
            if (s == 0) { s = 128; g.occupiedCells++; }
        }
        return g;
    }
}
