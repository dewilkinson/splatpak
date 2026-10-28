// ShadowMesh.h
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The shadow mesh of a .gstx package (format version 12, Clause 12 of docs/GSTX_FORMAT_SPECIFICATION.md): a smooth,
// closed, low-polygon mesh following the visible surface of the level 0 splats, in up to three levels of detail,
// coloured per vertex from the splats' base (DC) colour. Hosts use it for shadows, distance fields and collision
// (UE5SplatStream) and as a coarse stand-in for the model (SplatPak's View Shadow Mesh). Data types only, with no other
// dependency, so every host can include it; the builder is declared in ShadowMeshBuilder.h and the section's encoding
// is in ShadowMeshCodec.h.

#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Splat
{
    struct ShadowMeshVec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct ShadowMeshLevel
    {
        std::vector<ShadowMeshVec3> positions; // Model space, the splats' frame
        std::vector<uint32_t> colours;         // Per vertex, 0xAABBGGRR (A = 255), display-space DC colour
        std::vector<uint32_t> indices;         // Triangle list, counter-clockwise seen from outside
        size_t TriangleCount() const { return indices.size() / 3; }
    };

    struct ShadowMesh
    {
        std::vector<ShadowMeshLevel> levels; // Finest first; empty = no mesh
        bool Empty() const { return levels.empty() || levels[0].indices.empty(); }
    };
}
