// DetailHeatmap.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// PUBLIC BUILD NOTE: the private version of this project scores each cell of this grid so the
// streamer can deliver some regions of a model before others. This open build keeps the grid, its
// storage in the .gstx package (v7+) and its lookups, but Build() marks occupancy only -- every cell
// holding a point gets the same score -- so the streaming order is plain. See DetailHeatmap.cpp.
// Same header, same signatures as the private build, so nothing else in the codebase changes.

#pragma once
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include "GstxTypes.h"

namespace Splat
{
    struct DetailGrid
    {
        uint32_t nx = 0, ny = 0, nz = 0;
        XMFLOAT3 gMin = { 0.0f, 0.0f, 0.0f }; // World position of cell (0,0,0)'s min corner (the model's AABB min)
        float    cellSize = 0.0f;             // Cell edge in metres (isotropic)
        std::vector<uint8_t> score;           // nx*ny*nz, x fastest; 0 = empty cell, 1..255 = lowest..highest priority
        uint32_t occupiedCells = 0;           // Cells with at least one point (for logs / UI)

        bool Valid() const
        {
            return nx > 0 && ny > 0 && nz > 0 && cellSize > 0.0f && score.size() == (size_t)nx * ny * nz;
        }
        size_t Index(uint32_t x, uint32_t y, uint32_t z) const
        {
            return (size_t)x + (size_t)y * nx + (size_t)z * (size_t)nx * ny;
        }
        void CellOf(const XMFLOAT3& p, int32_t& ix, int32_t& iy, int32_t& iz) const
        {
            ix = std::max(0, std::min((int32_t)nx - 1, (int32_t)std::floor((p.x - gMin.x) / cellSize)));
            iy = std::max(0, std::min((int32_t)ny - 1, (int32_t)std::floor((p.y - gMin.y) / cellSize)));
            iz = std::max(0, std::min((int32_t)nz - 1, (int32_t)std::floor((p.z - gMin.z) / cellSize)));
        }

        // Score 0..1 of the cell containing p (0 for an empty cell or no grid).
        float Sample(const XMFLOAT3& p) const
        {
            if (!Valid()) return 0.0f;
            int32_t ix, iy, iz;
            CellOf(p, ix, iy, iz);
            return (float)score[Index((uint32_t)ix, (uint32_t)iy, (uint32_t)iz)] / 255.0f;
        }

        // Mean score 0..1 over the occupied cells a box touches -- what a streaming block (a 64-splat
        // meshlet with a small AABB) uses. Falls back to the centre cell when the box covers no
        // occupied cell (a coarse-LOD block drifting slightly outside the LOD 0 footprint).
        float SampleBox(const XMFLOAT3& bmin, const XMFLOAT3& bmax) const
        {
            if (!Valid()) return 0.0f;
            int32_t x0, y0, z0, x1, y1, z1;
            CellOf(bmin, x0, y0, z0);
            CellOf(bmax, x1, y1, z1);
            float sum = 0.0f;
            uint32_t n = 0;
            for (int32_t z = z0; z <= z1; z++)
                for (int32_t y = y0; y <= y1; y++)
                    for (int32_t x = x0; x <= x1; x++)
                    {
                        uint8_t s = score[Index((uint32_t)x, (uint32_t)y, (uint32_t)z)];
                        if (s == 0) continue;
                        sum += (float)s;
                        n++;
                    }
            if (n > 0) return (sum / (float)n) / 255.0f;
            XMFLOAT3 c((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f, (bmin.z + bmax.z) * 0.5f);
            return Sample(c);
        }

        // Builds and scores the grid from a point cloud (LOD 0 at preprocessing time; the reconstructed
        // LOD 0 on load of an older package). longestAxisCells sets the resolution (clamped to 4..96);
        // the other axes follow the model's proportions. Implemented in DetailHeatmap.cpp.
        static DetailGrid Build(const SplatPointArray& points, const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax, int longestAxisCells = 48);
    };
}
