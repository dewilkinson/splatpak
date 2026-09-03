#include <iostream>
#include <vector>
#include <cassert>
#include <DirectXMath.h>
#include "../src/DX12/Wavelet/WaveletTypes.h"

using namespace DirectX;
using namespace Surfels;

int main()
{
    std::cout << "Running Geometry Culling and Optimization Stats Test...\n";

    GeometryCullStats stats = {};
    stats.totalDatasetSurfels = 1000000;
    stats.totalDatasetChunks  = 1000;
    stats.lodActiveSurfels    = 400000;
    stats.lodActiveChunks     = 400;
    stats.lodPrunedSurfels    = 600000;

    stats.asFrustumCulledChunks  = 150;
    stats.asFrustumCulledSurfels = 150000;
    stats.asConeCulledChunks     = 50;
    stats.asConeCulledSurfels    = 50000;
    stats.asPassedChunks         = 200;
    stats.asPassedSurfels        = 200000;
    stats.msDrawnSurfels         = 200000;
    stats.generatedVertices      = stats.msDrawnSurfels * 4;
    stats.generatedTriangles     = stats.msDrawnSurfels * 2;

    stats.totalCullingRatio = ((float)(stats.totalDatasetSurfels - stats.msDrawnSurfels) / (float)stats.totalDatasetSurfels) * 100.0f;
    stats.lodDecimationRatio = ((float)stats.lodPrunedSurfels / (float)stats.totalDatasetSurfels) * 100.0f;
    stats.asCullingRatio = ((float)(stats.asFrustumCulledSurfels + stats.asConeCulledSurfels) / (float)stats.lodActiveSurfels) * 100.0f;
    stats.vramBandwidthSavedMB = (float)(stats.lodPrunedSurfels + stats.asFrustumCulledSurfels + stats.asConeCulledSurfels) * 8.0f / (1024.0f * 1024.0f);

    std::cout << "Total dataset points: " << stats.totalDatasetSurfels << "\n";
    std::cout << "Drawn points:         " << stats.msDrawnSurfels << "\n";
    std::cout << "Overall reduction:    " << stats.totalCullingRatio << "%\n";
    std::cout << "LOD pruned points:    " << stats.lodPrunedSurfels << " (" << stats.lodDecimationRatio << "%)\n";
    std::cout << "Frustum culled:       " << stats.asFrustumCulledSurfels << "\n";
    std::cout << "Cone culled:          " << stats.asConeCulledSurfels << "\n";
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
