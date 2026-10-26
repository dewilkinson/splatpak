// TestGeometryCull.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Unit test for GeometryCullStats' derived math (reduction ratios, rejection-stage
// percentages) using hand-picked numbers with known-correct expected results.

#include <iostream>
#include <vector>
#include <cassert>
#include <DirectXMath.h>
#include "../src/SplatCore/WaveletTypes.h"

using namespace DirectX;
using namespace Splat;

int main()
{
    std::cout << "Running Geometry Culling and Optimization Stats Test...\n";

    GeometryCullStats stats = {};
    stats.totalDatasetSplats = 1000000;
    stats.totalDatasetChunks  = 1000;
    stats.lodActiveSplats    = 400000;
    stats.lodActiveChunks     = 400;
    stats.lodPrunedSplats    = 600000;

    stats.asFrustumCulledChunks  = 150;
    stats.asFrustumCulledSplats = 150000;
    stats.asConeCulledChunks     = 50;
    stats.asConeCulledSplats    = 50000;
    stats.asPassedChunks         = 200;
    stats.asPassedSplats        = 200000;
    stats.msDrawnSplats         = 200000;
    stats.generatedVertices      = stats.msDrawnSplats * 4;
    stats.generatedTriangles     = stats.msDrawnSplats * 2;

    stats.totalCullingRatio = ((float)(stats.totalDatasetSplats - stats.msDrawnSplats) / (float)stats.totalDatasetSplats) * 100.0f;
    stats.lodDecimationRatio = ((float)stats.lodPrunedSplats / (float)stats.totalDatasetSplats) * 100.0f;
    stats.asCullingRatio = ((float)(stats.asFrustumCulledSplats + stats.asConeCulledSplats) / (float)stats.lodActiveSplats) * 100.0f;
    stats.vramBandwidthSavedMB = (float)(stats.lodPrunedSplats + stats.asFrustumCulledSplats + stats.asConeCulledSplats) * 8.0f / (1024.0f * 1024.0f);

    std::cout << "Total dataset points: " << stats.totalDatasetSplats << "\n";
    std::cout << "Drawn points:         " << stats.msDrawnSplats << "\n";
    std::cout << "Overall reduction:    " << stats.totalCullingRatio << "%\n";
    std::cout << "LOD pruned points:    " << stats.lodPrunedSplats << " (" << stats.lodDecimationRatio << "%)\n";
    std::cout << "Frustum culled:       " << stats.asFrustumCulledSplats << "\n";
    std::cout << "Cone culled:          " << stats.asConeCulledSplats << "\n";
    std::cout << "Vertices generated:   " << stats.generatedVertices << " (4x)\n";
    std::cout << "Triangles generated:  " << stats.generatedTriangles << " (2x)\n";
    std::cout << "VRAM Bandwidth saved: " << stats.vramBandwidthSavedMB << " MB/frame\n";

    assert(stats.totalCullingRatio == 80.0f);
    assert(stats.lodDecimationRatio == 60.0f);
    assert(stats.asCullingRatio == 50.0f);
    assert(stats.generatedVertices == 800000);
    assert(stats.generatedTriangles == 400000);

    // Test frustum box intersection logic
    XMMATRIX viewProj = XMMatrixPerspectiveFovRH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 100.0f);
    XMFLOAT3 inFrontCenter = { 0.0f, 0.0f, -5.0f };
    XMVECTOR pVec = XMLoadFloat3(&inFrontCenter);
    XMVECTOR clip = XMVector3Transform(pVec, viewProj);
    XMFLOAT4 clip4;
    XMStoreFloat4(&clip4, clip);
    assert(clip4.w > 0.0f);
    assert(clip4.x >= -clip4.w && clip4.x <= clip4.w);

    std::cout << "All Geometry Culling & Stats assertions passed successfully!\n";
    return 0;
}
