// LiftingWavelet.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Open-build LOD pyramid: plain decimation, not the private lifting-wavelet implementation.
// Each coarser level keeps every other point from the level below (so the count roughly
// halves each time, same as the original), averages the pair's normal and colour, and
// scales the surviving point's radius by sqrt(2) so it still covers the area both of its
// dropped neighbours used to -- the same convention the rest of the renderer (chunk LOD
// selection, streaming) already expects, so no other file needs to change. geometricError
// is a simple function of level and local point spacing, just detailed enough for
// LODSelector's screen-space-error comparison to pick sensible distances; it is not a real
// wavelet detail-coefficient magnitude.

#include "LiftingWavelet.h"
#include <cmath>
#include <algorithm>

namespace Splat
{
    WaveletDecompositionResult LiftingWavelet::DecomposeChunk(
        const SplatPointArray& inputSplats,
        uint32_t maxLODLevels,
        float /*deadbandThreshold*/)
    {
        WaveletDecompositionResult result;
        if (inputSplats.empty()) return result;

        WaveletLODLevel lod0;
        lod0.level = 0;
        lod0.geometricError = 0.0f;
        lod0.splats = inputSplats;
        result.lodLevels.push_back(lod0);

        SplatPointArray currentLevel = inputSplats;
        float radiusScale = 1.0f;

        for (uint32_t lvl = 1; lvl <= maxLODLevels; lvl++)
        {
            if (currentLevel.size() <= 4)
                break;

            size_t n = currentLevel.size();
            size_t half = n / 2;
            SplatPointArray coarseLevel(half);
            radiusScale *= 1.41421356f; // sqrt(2) per halving, same coverage convention as before

            for (size_t i = 0; i < half; i++)
            {
                const auto& a = currentLevel[i * 2];
                const auto& b = currentLevel[i * 2 + 1];

                SplatPoint coarse = a; // Keeps sourceIndex (splat mode bookkeeping)
                coarse.position = a.position; // Keep the "even" sample's position; drop the "odd" one

                XMFLOAT3 avgN(a.normal.x + b.normal.x, a.normal.y + b.normal.y, a.normal.z + b.normal.z);
                float nLen = std::sqrt(avgN.x * avgN.x + avgN.y * avgN.y + avgN.z * avgN.z);
                coarse.normal = (nLen > 1e-6f) ? XMFLOAT3(avgN.x / nLen, avgN.y / nLen, avgN.z / nLen) : a.normal;

                coarse.color = XMFLOAT3(
                    (a.color.x + b.color.x) * 0.5f,
                    (a.color.y + b.color.y) * 0.5f,
                    (a.color.z + b.color.z) * 0.5f);

                coarse.radius = std::max(a.radius, b.radius) * 1.41421356f;
                coarseLevel[i] = coarse;
            }

            WaveletLODLevel lod;
            lod.level = lvl;
            lod.geometricError = radiusScale * 0.01f; // Monotonically increasing; enough for LOD selection
            lod.splats = coarseLevel;
            result.lodLevels.push_back(lod);

            currentLevel = std::move(coarseLevel);
        }

        return result;
    }
}
