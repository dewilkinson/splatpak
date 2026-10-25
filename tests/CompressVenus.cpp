// CompressVenus.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Command-line packager: loads a .ply (a 3DGS scene, or a plain point cloud shown as Gaussians at its
// points, see PointCloudGaussians.h) and writes the same .gstx that SplatLab's "Save Compressed
// Package" would produce for it with default settings -- octree chunking, the wavelet LOD pyramid,
// 20-byte Gaussian records, the baked interior occlusion volume, and the source file size in the header.
// Used to regenerate the bundled assets without the GUI, and as an end-to-end pipeline smoke test.
//   CompressVenus <input.ply> <output base path> [shave 0..10] [spherical-harmonics degree 0..3, default 3]

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include "../src/SurfelsCore/PLYLoader.h"
#include "../src/SurfelsCore/SpatialOctree.h"
#include "../src/SurfelsCore/StreamPackager.h"
#include "../src/SurfelsCore/PointCloudGaussians.h"
#include "../libs/gstx/OcclusionVolume.h"

int main(int argc, char** argv)
{
    std::string inputPLY = (argc > 1) ? argv[1] : "models/venus.ply";
    std::string outputBase = (argc > 2) ? argv[2] : "models/venus";
    float shave = (argc > 3) ? (float)atof(argv[3]) : 0.0f;
    // The spherical-harmonics degree the package keeps (0 to 3; a point cloud has none, so 0)
    const int splatDegree = std::max(0, std::min(3, (argc > 4) ? atoi(argv[4]) : 3));

    std::cout << "Loading " << inputPLY << "..." << std::endl;
    std::vector<Surfels::SurfelVertex> surfels;
    std::vector<Surfels::SplatAttributes> splatAttrs;
    double origin[3] = {0,0,0};
    if (!Surfels::PLYLoader::LoadPLY(inputPLY, surfels, origin, &splatAttrs))
    {
        std::cerr << "Failed to load " << inputPLY << std::endl;
        return 1;
    }

    uint64_t sourceFileBytes = 0;
    {
        std::ifstream in(inputPLY, std::ios::ate | std::ios::binary);
        if (in.is_open()) sourceFileBytes = (uint64_t)in.tellg();
    }

    DirectX::XMFLOAT3 minP(1e9f, 1e9f, 1e9f), maxP(-1e9f, -1e9f, -1e9f);
    for (const auto& s : surfels)
    {
        minP.x = std::min(minP.x, s.position.x);
        minP.y = std::min(minP.y, s.position.y);
        minP.z = std::min(minP.z, s.position.z);
        maxP.x = std::max(maxP.x, s.position.x);
        maxP.y = std::max(maxP.y, s.position.y);
        maxP.z = std::max(maxP.z, s.position.z);
    }
    float maxDim = std::max(maxP.x - minP.x, std::max(maxP.y - minP.y, maxP.z - minP.z));

    // Same automatic defaults SplatLab applies when a .ply is opened (SurfelsApp::RecomputeWaveletHierarchy)
    float chunkSize = std::max(0.10f, maxDim / 4.0f);              // ~4x4x4 chunks per model
    uint32_t maxLODs = surfels.size() > 2000000 ? 5 : surfels.size() > 500000 ? 4 : surfels.size() > 100000 ? 3 : 2;
    float deadbandMeters = std::max(0.5f, maxDim * 1.5f) / 1000.0f; // Deadband scales with model size
    const float splatRadius = 1.0f;

    std::cout << "Loaded " << surfels.size() << " points, extent " << maxDim << ". chunk=" << chunkSize
              << " lods=" << maxLODs << " deadband=" << deadbandMeters * 1000.0f << "mm" << std::endl;

    // Detail grid (v7+): an input to the streaming order, baked from the original cloud (libs/gstx/DetailHeatmap.h)
    Surfels::DetailGrid detailGrid = Surfels::DetailGrid::Build(surfels, minP, maxP);
    std::cout << "Detail heatmap: detail grid " << detailGrid.nx << "x" << detailGrid.ny << "x" << detailGrid.nz
              << " cells (" << detailGrid.cellSize << " m), " << detailGrid.occupiedCells << " occupied" << std::endl;

    std::cout << "Baking occlusion volume (shave " << shave << ")..." << std::endl;
    Surfels::OcclusionVolume::Grid grid;
    std::vector<Surfels::OcclusionVoxelGPU> occlusion;
    Surfels::OcclusionMipTable occlusionMips;
    {
        std::string trace;
        Surfels::OcclusionVolume::BuildGrid(surfels, minP, maxP, grid, trace);
        Surfels::OcclusionVolume::Bake(grid, shave, Surfels::OcclusionVolume::ColorGrade{}, occlusion, occlusionMips, trace);
        std::cout << trace;
    }

    // A plain point cloud: a Gaussian at every point, before the chunking so each point keeps its index
    const bool pointCloud = splatAttrs.empty();
    if (pointCloud)
    {
        const float sigma = Surfels::PointCloudGaussians::Build(surfels, splatAttrs);
        std::cout << "Point cloud: a Gaussian of sigma " << sigma << " at each point (half the median spacing)" << std::endl;
    }

    auto chunks = Surfels::SpatialOctree::PartitionIntoChunks(surfels, chunkSize);
    std::cout << "Partitioned into " << chunks.size() << " chunks. Packaging to " << outputBase << ".gstx..." << std::endl;

    Surfels::SplatBakeInput splatInput;
    splatInput.attributes = &splatAttrs;
    splatInput.shDegree = pointCloud ? 0u : (uint32_t)splatDegree;
    std::cout << "Splat mode: 20-byte Gaussian records, spherical-harmonics degree " << splatInput.shDegree << std::endl;
    if (!Surfels::StreamPackager::PackageDataset(outputBase, chunks, maxLODs, deadbandMeters, splatRadius, occlusion, sourceFileBytes, &occlusionMips, &detailGrid, &splatInput))
    {
        std::cerr << "Failed to package dataset" << std::endl;
        return 1;
    }

    std::ifstream out(outputBase + ".gstx", std::ios::ate | std::ios::binary);
    uint64_t packageBytes = out.is_open() ? (uint64_t)out.tellg() : 0;
    std::cout << "Successfully generated " << outputBase << ".gstx: " << packageBytes / (1024.0 * 1024.0) << " MB, "
              << (packageBytes > 0 ? (double)sourceFileBytes / (double)packageBytes : 0.0) << "x vs source, "
              << occlusion.size() << " occluder blocks in " << occlusionMips.mipCount << " mips (";
    for (uint32_t k = 0; k < occlusionMips.mipCount; k++)
        std::cout << (k ? " / " : "") << occlusionMips.blockCount[k];
    std::cout << ")" << std::endl;
    return 0;
}
