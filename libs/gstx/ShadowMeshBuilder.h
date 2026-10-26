// ShadowMeshBuilder.h
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Builds a model's shadow mesh (ShadowMesh.h) from its level 0 splats. Public API of the codec library; the open
// stand-in library builds nothing.

#pragma once
#include <functional>
#include <string>
#include <vector>
#include "ShadowMesh.h"
#include "../../src/SplatCore/WaveletTypes.h"

namespace Splat
{
    struct ShadowMeshBuilder
    {
        // Builds the mesh from a model's level 0 splats (their Gaussian attributes, when present, shape the surface).
        // progress, when given, gets the fraction done and may return false to stop (Build then returns false).
        // trace receives a few lines of what was built. Returns false when no mesh could be built; the open
        // stand-in library always returns false.
        static bool Build(const std::vector<SplatPoint>& points, const std::vector<SplatAttributes>* attributes,
                          const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax, ShadowMesh& out, std::string& trace,
                          const std::function<bool(float)>& progress = nullptr);
    };
}
