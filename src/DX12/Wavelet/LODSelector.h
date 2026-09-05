// LODSelector.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Screen-space-error LOD selection: given the active camera, picks the coarsest LOD
// level per chunk whose projected geometric error still fits within a target pixel
// budget, and sorts the result front-to-back for cache-friendly streaming priority.

#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "WaveletTypes.h"

namespace Surfels
{
    struct ActiveChunkSelection
    {
        uint32_t chunkId;
        uint32_t selectedLOD;
        float    distanceToCamera;
        float    screenErrorPixels;
    };

    class LODSelector
    {
    public:
        // Evaluates all chunks against camera and selects appropriate LOD level based on Screen-Space Error
        static std::vector<ActiveChunkSelection> SelectLODs(
            const std::vector<ChunkManifest>& chunks,
            XMFLOAT3 cameraPos,
            XMMATRIX viewProj,
            float viewportHeight = 900.0f,
            float fovY = XM_PIDIV4,
            float targetPixelError = 3.0f) // Target max screen error in pixels
        {
            std::vector<ActiveChunkSelection> selected;
            selected.reserve(chunks.size());

            float focalLength = (viewportHeight * 0.5f) / std::tan(fovY * 0.5f);

            for (const auto& chunk : chunks)
            {
                // 1. Frustum Culling Check (against chunk bounding sphere)
                XMVECTOR centerVec = XMLoadFloat3(&chunk.center);
                XMVECTOR clipCenter = XMVector3TransformCoord(centerVec, viewProj);
                XMFLOAT3 clipPos;
                XMStoreFloat3(&clipPos, clipCenter);

                // Quick Z culling
                if (clipPos.z < -chunk.boundingRadius)
                    continue;

                // 2. Distance to camera calculation
                float dx = chunk.center.x - cameraPos.x;
                float dy = chunk.center.y - cameraPos.y;
                float dz = chunk.center.z - cameraPos.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                dist = std::max(0.1f, dist - chunk.boundingRadius);

                // 3. Screen-Space Error evaluation
                // Base geometric error scaling per LOD
                uint32_t chosenLOD = chunk.numLODs > 0 ? (chunk.numLODs - 1) : 0; // default to coarsest

                float screenError = 0.0f;
                for (int lvl = 0; lvl < (int)chunk.lods.size(); lvl++)
                {
                    float geomErr = chunk.lods[lvl].geometricError;
                    if (geomErr <= 1e-6f) geomErr = 0.005f;

                    screenError = (geomErr / dist) * focalLength;

                    // If error is within acceptable pixel threshold, use this LOD
                    if (screenError <= targetPixelError)
                    {
                        chosenLOD = (uint32_t)lvl;
                        break;
                    }
                    chosenLOD = (uint32_t)lvl;
                }

                ActiveChunkSelection sel;
                sel.chunkId = chunk.chunkId;
                sel.selectedLOD = chosenLOD;
                sel.distanceToCamera = dist;
                sel.screenErrorPixels = screenError;
                selected.push_back(sel);
            }

            // Sort by distance (front-to-back for cache priority)
            std::sort(selected.begin(), selected.end(), [](const ActiveChunkSelection& a, const ActiveChunkSelection& b) {
                return a.distanceToCamera < b.distanceToCamera;
            });

            return selected;
        }
    };
}

