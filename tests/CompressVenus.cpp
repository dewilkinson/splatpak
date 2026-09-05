// CompressVenus.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Command-line packager: loads a .ply point cloud and writes the same .sflw that SurfelLab's
// "Save Compressed Package" would produce for it with default settings -- octree chunking, the
// wavelet LOD pyramid, the baked interior occlusion volume, and the source file size in the header.
// Used to regenerate the bundled assets without the GUI, and as an end-to-end pipeline smoke test.
//   CompressVenus <input.ply> <output base path> [shave 0..10]

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include "../tools/SurfelsPreprocess/PLYLoader.h"
#include "../tools/SurfelsPreprocess/SpatialOctree.h"
#include "../tools/SurfelsPreprocess/StreamPackager.h"
#include "../libs/SurfelsCore/OcclusionVolume.h"

int main(int argc, char** argv)
{
    std::string inputPLY = (argc > 1) ? argv[1] : "models/venus.ply";
    std::string outputBase = (argc > 2) ? argv[2] : "models/venus";
    float shave = (argc > 3) ? (float)atof(argv[3]) : 0.0f;

    std::cout << "Loading " << inputPLY << "..." << std::endl;
    std::vector<Surfels::SurfelVertex> surfels;
    double origin[3] = {0,0,0};
    if (!Surfels::PLYLoader::LoadPLY(inputPLY, surfels, origin))
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

    // Same automatic defaults SurfelLab applies when a .ply is opened (PreprocessApp::RecomputeWaveletHierarchy)
    float chunkSize = std::max(0.10f, maxDim / 4.0f);              // ~4x4x4 chunks per model
    uint32_t maxLODs = surfels.size() > 2000000 ? 5 : surfels.size() > 500000 ? 4 : surfels.size() > 100000 ? 3 : 2;
    float deadbandMeters = std::max(0.5f, maxDim * 1.5f) / 1000.0f; // Deadband scales with model size
    const float splatRadius = 1.0f;

    std::cout << "Loaded " << surfels.size() << " points, extent " << maxDim << ". chunk=" << chunkSize
              << " lods=" << maxLODs << " deadband=" << deadbandMeters * 1000.0f << "mm" << std::endl;

    std::cout << "Baking occlusion volume (shave " << shave << ")..." << std::endl;
    Surfels::OcclusionVolume::Grid grid;
    std::vector<Surfels::OcclusionVoxelGPU> occlusion;
    {
        std::string trace;
        Surfels::OcclusionVolume::BuildGrid(surfels, minP, maxP, grid, trace);
        Surfels::OcclusionVolume::Bake(grid, shave, Surfels::OcclusionVolume::ColorGrade{}, occlusion, trace);
        std::cout << trace;
    }

    auto chunks = Surfels::SpatialOctree::PartitionIntoChunks(surfels, chunkSize);
    std::cout << "Partitioned into " << chunks.size() << " chunks. Packaging to " << outputBase << ".sflw..." << std::endl;

    if (!Surfels::StreamPackager::PackageDataset(outputBase, chunks, maxLODs, deadbandMeters, splatRadius, occlusion, sourceFileBytes))
    {
        std::cerr << "Failed to package dataset" << std::endl;
        return 1;
    }

    std::ifstream out(outputBase + ".sflw", std::ios::ate | std::ios::binary);
    uint64_t packageBytes = out.is_open() ? (uint64_t)out.tellg() : 0;
    std::cout << "Successfully generated " << outputBase << ".sflw: " << packageBytes / (1024.0 * 1024.0) << " MB, "
              << (packageBytes > 0 ? (double)sourceFileBytes / (double)packageBytes : 0.0) << "x vs source, "
              << occlusion.size() << " occluder blocks" << std::endl;
    return 0;
}
