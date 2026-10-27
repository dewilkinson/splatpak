// CompressVenus.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Command-line packager: loads a .ply (a 3DGS scene, or a plain point cloud shown as Gaussians at its
// points, see PointCloudGaussians.h) and writes the same .gstx that SplatPak's "Save Compressed
// Package" would produce for it with default settings -- octree chunking, the wavelet LOD pyramid,
// 20-byte Gaussian records, the baked interior occlusion volume, and the source file size in the header.
// Used to regenerate the bundled assets without the GUI, and as an end-to-end pipeline smoke test.
//   CompressVenus <input.ply> <output base path> [shave 0..10] [spherical-harmonics degree 0..3, default 3]
//                 [--no-colour-transform]   (writes colour as RGB, as before v11: for measuring what YCoCg saves)
//                 [--sh-bits b1,b2,b3]   (requantise SH bands 1-3 to fewer bits, as SplatPak's SH Precision does: lossy)

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <chrono>
#include "../src/SplatCore/PLYLoader.h"
#include "../src/SplatCore/SpatialOctree.h"
#include "../src/SplatCore/StreamPackager.h"
#include "../src/SplatCore/PointCloudGaussians.h"
#include "../libs/gstx/OcclusionVolume.h"
#include "../libs/gstx/ShadowMeshBuilder.h"

int main(int argcIn, char** argvIn)
{
    // Flags may appear anywhere; the rest are positional
    bool colourTransform = true;
    int shBits[3] = { 8, 8, 8 };
    uint32_t transforms = Splat::PAYLOAD_TRANSFORMS_DEFAULT; // --transforms N: the payloadTransforms bits, for measurement
    std::vector<char*> args;
    for (int i = 0; i < argcIn; i++)
    {
        const std::string a = argvIn[i];
        if (a == "--no-colour-transform") colourTransform = false;
        else if (a == "--sh-bits" && i + 1 < argcIn) sscanf(argvIn[++i], "%d,%d,%d", &shBits[0], &shBits[1], &shBits[2]);
        else if (a == "--transforms" && i + 1 < argcIn) transforms = (uint32_t)atoi(argvIn[++i]);
        else args.push_back(argvIn[i]);
    }
    const int argc = (int)args.size();
    char** argv = args.data();
    std::string inputPLY = (argc > 1) ? argv[1] : "models/venus.ply";
    std::string outputBase = (argc > 2) ? argv[2] : "models/venus";
    float shave = (argc > 3) ? (float)atof(argv[3]) : 0.0f;
    // The spherical-harmonics degree the package keeps (0 to 3; a point cloud has none, so 0)
    const int splatDegree = std::max(0, std::min(3, (argc > 4) ? atoi(argv[4]) : 3));

    std::cout << "Loading " << inputPLY << "..." << std::endl;
    Splat::SplatPointArray splats;
    Splat::SplatAttributeArray splatAttrs;
    double origin[3] = {0,0,0};
    if (!Splat::PLYLoader::LoadPLY(inputPLY, splats, origin, &splatAttrs))
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
    for (const auto& s : splats)
    {
        minP.x = std::min(minP.x, s.position.x);
        minP.y = std::min(minP.y, s.position.y);
        minP.z = std::min(minP.z, s.position.z);
        maxP.x = std::max(maxP.x, s.position.x);
        maxP.y = std::max(maxP.y, s.position.y);
        maxP.z = std::max(maxP.z, s.position.z);
    }
    float maxDim = std::max(maxP.x - minP.x, std::max(maxP.y - minP.y, maxP.z - minP.z));

    // Same automatic defaults SplatPak applies when a .ply is opened (SplatPakApp::DefaultBakeParams)
    float chunkSize = std::max(0.10f, maxDim / 4.0f);              // ~4x4x4 chunks per model
    uint32_t maxLODs = splats.size() > 2000000 ? 5 : splats.size() > 500000 ? 4 : splats.size() > 100000 ? 3 : 2;
    float deadbandMeters = std::max(0.5f, maxDim * 1.5f) / 1000.0f; // Deadband scales with model size
    const float splatRadius = 1.0f;

    std::cout << "Loaded " << splats.size() << " points, extent " << maxDim << ". chunk=" << chunkSize
              << " lods=" << maxLODs << " deadband=" << deadbandMeters * 1000.0f << "mm" << std::endl;

    // Detail grid (v7+): an input to the streaming order, baked from the original cloud (libs/gstx/DetailHeatmap.h)
    Splat::DetailGrid detailGrid = Splat::DetailGrid::Build(splats, minP, maxP);
    std::cout << "Detail heatmap: detail grid " << detailGrid.nx << "x" << detailGrid.ny << "x" << detailGrid.nz
              << " cells (" << detailGrid.cellSize << " m), " << detailGrid.occupiedCells << " occupied" << std::endl;

    std::cout << "Baking occlusion volume (shave " << shave << ")..." << std::endl;
    Splat::OcclusionVolume::Grid grid;
    std::vector<Splat::OcclusionVoxelGPU> occlusion;
    Splat::OcclusionMipTable occlusionMips;
    {
        std::string trace;
        Splat::OcclusionVolume::BuildGrid(splats, minP, maxP, grid, trace);
        Splat::OcclusionVolume::Bake(grid, shave, Splat::OcclusionVolume::ColorGrade{}, occlusion, occlusionMips, trace);
        std::cout << trace;
    }

    // A plain point cloud: a Gaussian at every point, before the chunking so each point keeps its index
    const bool pointCloud = splatAttrs.empty();
    if (pointCloud)
    {
        const float sigma = Splat::PointCloudGaussians::Build(splats, splatAttrs);
        std::cout << "Point cloud: a Gaussian of sigma " << sigma << " at each point (half the median spacing)" << std::endl;
    }

    // Shadow mesh (v12): the smooth mesh following the visible surface, from the level 0 Gaussians (ShadowMesh.h)
    Splat::ShadowMesh shadowMesh;
    {
        std::string trace;
        const auto t0 = std::chrono::steady_clock::now();
        if (Splat::ShadowMeshBuilder::Build(splats, splatAttrs.empty() ? nullptr : &splatAttrs, minP, maxP, shadowMesh, trace))
            std::cout << trace << "Shadow mesh built in " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s" << std::endl;
        else
            std::cout << "No shadow mesh (this codec build has none)" << std::endl;
    }

    // Occluder mesh (v13): the volume as a smooth mesh, which then replaces the voxel array in the package
    Splat::ShadowMesh occluderMesh;
    {
        std::string trace;
        if (Splat::ShadowMeshBuilder::BuildOccluder(occlusion, occlusionMips, occluderMesh, trace))
        {
            std::cout << trace;
            occlusion.clear();
            occlusionMips = Splat::OcclusionMipTable{};
        }
    }

    auto chunks = Splat::SpatialOctree::PartitionIntoChunks(splats, chunkSize);
    std::cout << "Partitioned into " << chunks.size() << " chunks. Packaging to " << outputBase << ".gstx..." << std::endl;

    Splat::SplatBakeInput splatInput;
    splatInput.attributes = &splatAttrs;
    splatInput.shDegree = pointCloud ? 0u : (uint32_t)splatDegree;
    for (int b = 0; b < 3; b++) splatInput.shBits[b] = (uint8_t)std::max(1, std::min(8, shBits[b]));
    if (!colourTransform) transforms = 0;
    if (shBits[0] < 8 || shBits[1] < 8 || shBits[2] < 8)
        printf("SH requantised to %d / %d / %d bits (bands 1 / 2 / 3)\n", shBits[0], shBits[1], shBits[2]);
    std::cout << "Splat mode: 20-byte Gaussian records, spherical-harmonics degree " << splatInput.shDegree << std::endl;
    Splat::TransformStats transformStats;
    if (!Splat::StreamPackager::PackageDataset(outputBase, chunks, maxLODs, deadbandMeters, splatRadius, occlusion, sourceFileBytes, &occlusionMips, &detailGrid, &splatInput,
                                               Splat::PayloadCodec::kDefault, nullptr, transforms, &transformStats, &shadowMesh, &occluderMesh))
    {
        std::cerr << "Failed to package dataset" << std::endl;
        return 1;
    }

    printf("Colour transform %s: entropy records %.3f -> %.3f, SH %.3f -> %.3f bits/byte (%+.1f%% overall)\n", colourTransform ? "YCoCg" : "off",
           transformStats.recordEntropyBefore, transformStats.recordEntropyAfter, transformStats.shEntropyBefore, transformStats.shEntropyAfter,
           transformStats.TotalChangePercent());
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
