// TestSplatCodec.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Splat mode round trip: loads a 3D Gaussian Splatting .ply with its full attributes, encodes and
// decodes the records in memory, bakes a splat-mode .sflw (surfel format 1) through the packager,
// reloads it, and checks that every level-0 Gaussian survives within the format's quantisation.
//
//   TestSplatCodec <input.ply> <output-base> [max-points] [sh-degree]
//
// Exit code 0 on success, 1 on any failure. Prints the package size per splat.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "../src/SurfelsCore/PLYLoader.h"
#include "../src/SurfelsCore/SpatialOctree.h"
#include "../src/SurfelsCore/StreamPackager.h"
#include "../libs/bluesec-codec/SplatCodec.h"

using namespace Surfels;

static int g_failures = 0;
static void Check(bool ok, const char* what, double got, double limit)
{
    if (!ok)
    {
        std::cerr << "FAIL: " << what << " (" << got << " > " << limit << ")" << std::endl;
        g_failures++;
    }
}

static float QuatDot(const XMFLOAT4& a, const XMFLOAT4& b)
{
    return std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

int main(int argc, char** argv)
{
    const std::string inputPLY = (argc > 1) ? argv[1] : "assets/cthulu/cthulu.ply";
    const std::string outputBase = (argc > 2) ? argv[2] : "splat_test";
    const size_t maxPoints = (argc > 3) ? (size_t)atoll(argv[3]) : 200000;
    const uint32_t shDegree = (argc > 4) ? (uint32_t)atoi(argv[4]) : 1;

    std::vector<SurfelVertex> surfels;
    std::vector<SplatAttributes> attrs;
    double origin[3] = { 0, 0, 0 };
    if (!PLYLoader::LoadPLY(inputPLY, surfels, origin, &attrs))
    {
        std::cerr << "Failed to load " << inputPLY << std::endl;
        return 1;
    }
    if (attrs.empty())
    {
        std::cerr << inputPLY << " carries no Gaussian attributes" << std::endl;
        return 1;
    }
    if (surfels.size() > maxPoints)
    {
        // Keep an evenly spaced subset; the attributes stay whole (sourceIndex still refers into them)
        std::vector<SurfelVertex> sub;
        const size_t step = surfels.size() / maxPoints;
        for (size_t i = 0; i < surfels.size(); i += step) sub.push_back(surfels[i]);
        surfels.swap(sub);
    }
    std::cout << "Loaded " << surfels.size() << " points (" << attrs.size() << " Gaussians), SH degree " << shDegree << std::endl;

    XMFLOAT3 minP(1e9f, 1e9f, 1e9f), maxP(-1e9f, -1e9f, -1e9f);
    for (const auto& s : surfels)
    {
        minP.x = std::min(minP.x, s.position.x); minP.y = std::min(minP.y, s.position.y); minP.z = std::min(minP.z, s.position.z);
        maxP.x = std::max(maxP.x, s.position.x); maxP.y = std::max(maxP.y, s.position.y); maxP.z = std::max(maxP.z, s.position.z);
    }
    const float maxDim = std::max(maxP.x - minP.x, std::max(maxP.y - minP.y, maxP.z - minP.z));

    // 1. In-memory round trip of the codec
    SplatEncodeParams params;
    params.shDegree = shDegree;
    params.aabbMin = minP;
    params.aabbMax = maxP;
    SplatCodec::ComputeScaleRange(attrs, 4, params.scaleLog2Min, params.scaleLog2Max);
    {
        std::vector<PackedSplatGPU> records;
        std::vector<uint8_t> sh;
        SplatCodec::Encode(surfels, attrs, 0, params, records, sh);
        Check(records.size() == surfels.size(), "record count", (double)records.size(), (double)surfels.size());
        Check(sh.size() == surfels.size() * SplatCodec::SHRecordBytes(shDegree), "sh stream size", (double)sh.size(), (double)(surfels.size() * SplatCodec::SHRecordBytes(shDegree)));
        std::vector<SurfelVertex> back;
        std::vector<SplatAttributes> backAttrs;
        SplatCodec::Decode(records, sh.data(), sh.size(), params, 0, 0, back, backAttrs);

        const float posTol = maxDim / 65535.0f * 1.5f;
        const float log2Step = (params.scaleLog2Max - params.scaleLog2Min) / 255.0f;
        double maxPosErr = 0, maxScaleErr = 0, minQuatDot = 1, maxOpErr = 0, maxColErr = 0, maxShErr = 0;
        const uint32_t shCoeffs = SplatCodec::SHCoefficientsPerChannel(shDegree) * 3;
        for (size_t i = 0; i < surfels.size(); i++)
        {
            const SurfelVertex& a = surfels[i];
            const SurfelVertex& b = back[i];
            const SplatAttributes& sa = attrs[a.sourceIndex];
            const SplatAttributes& sb = backAttrs[i];
            maxPosErr = std::max(maxPosErr, (double)std::max(std::fabs(a.position.x - b.position.x), std::max(std::fabs(a.position.y - b.position.y), std::fabs(a.position.z - b.position.z))));
            const float sx[3] = { sa.scale.x, sa.scale.y, sa.scale.z }, sy[3] = { sb.scale.x, sb.scale.y, sb.scale.z };
            for (int k = 0; k < 3; k++)
                maxScaleErr = std::max(maxScaleErr, (double)std::fabs(std::log2(std::max(sx[k], 1e-9f)) - std::log2(std::max(sy[k], 1e-9f))));
            minQuatDot = std::min(minQuatDot, (double)QuatDot(sa.rotation, sb.rotation));
            maxOpErr = std::max(maxOpErr, (double)std::fabs(sa.opacity - sb.opacity));
            maxColErr = std::max(maxColErr, (double)std::max(std::fabs(a.color.x - b.color.x), std::max(std::fabs(a.color.y - b.color.y), std::fabs(a.color.z - b.color.z))));
            float shMax = 0.0f;
            for (uint32_t k = 0; k < shCoeffs; k++) shMax = std::max(shMax, std::fabs(sa.sh[k]));
            for (uint32_t k = 0; k < shCoeffs; k++)
                maxShErr = std::max(maxShErr, (double)(std::fabs(sa.sh[k] - sb.sh[k]) / std::max(shMax, 1e-6f)));
        }
        std::cout << "Round trip: position " << maxPosErr << " (limit " << posTol << "), scale log2 " << maxScaleErr << " (limit " << log2Step
                  << "), quaternion dot " << minQuatDot << ", opacity " << maxOpErr << ", colour " << maxColErr << ", SH " << maxShErr << " of max" << std::endl;
        Check(maxPosErr <= posTol, "position error", maxPosErr, posTol);
        Check(maxScaleErr <= log2Step * 0.51f + 1e-4f, "scale error", maxScaleErr, log2Step * 0.51f);
        Check(minQuatDot >= 0.999, "quaternion", minQuatDot, 0.999);
        Check(maxOpErr <= 0.5 / 255.0 + 1e-5, "opacity", maxOpErr, 0.5 / 255.0);
        Check(maxColErr <= 0.5 / 255.0 + 1e-5, "colour", maxColErr, 0.5 / 255.0);
        if (shCoeffs > 0) Check(maxShErr <= 1.0 / 127.0 + 2e-3, "spherical harmonics", maxShErr, 1.0 / 127.0);
    }

    // 2. Bake a splat-mode package and reload it
    const float chunkSize = std::max(0.10f, maxDim / 4.0f);
    const uint32_t maxLODs = 3;
    auto chunks = SpatialOctree::PartitionIntoChunks(surfels, chunkSize);
    SplatBakeInput bake;
    bake.attributes = &attrs;
    bake.shDegree = shDegree;
    if (!StreamPackager::PackageDataset(outputBase, chunks, maxLODs, 0.001f, 1.0f, {}, 0, nullptr, nullptr, &bake))
    {
        std::cerr << "PackageDataset failed" << std::endl;
        return 1;
    }
    StreamPackager::SFLWPackageData pkg;
    std::string reason;
    if (!StreamPackager::LoadPackage(outputBase + ".sflw", pkg, &reason))
    {
        std::cerr << "LoadPackage failed: " << reason << std::endl;
        return 1;
    }
    Check(pkg.IsSplatPackage(), "splat package flag", pkg.header.surfelFormat, 1);
    Check(pkg.header.version == SFLW_VERSION, "version", pkg.header.version, SFLW_VERSION);
    Check(pkg.header.shDegree == shDegree, "sh degree", pkg.header.shDegree, shDegree);
    Check(pkg.header.splatRecordBytes == sizeof(PackedSplatGPU), "record bytes", pkg.header.splatRecordBytes, sizeof(PackedSplatGPU));

    size_t total0 = 0, totalSH = 0, levels = 0;
    double maxPosErr = 0, maxPkgShErr = 0;
    for (size_t c = 0; c < pkg.chunkManifests.size(); c++)
    {
        levels = std::max(levels, pkg.chunkLODSplats[c].size());
        if (pkg.chunkLODSplats[c].empty()) continue;
        std::vector<SurfelVertex> back;
        std::vector<SplatAttributes> backAttrs;
        const std::vector<uint8_t>& sh = pkg.chunkLODSH[c][0];
        SplatCodec::Decode(pkg.chunkLODSplats[c][0], sh.empty() ? nullptr : sh.data(), sh.size(), pkg.splatParams, 0, 0, back, backAttrs);
        total0 += back.size();
        totalSH += sh.size();
        // The chunk's level-0 points are the bake input in order (the packager decomposes per chunk)
        const auto& src = chunks[c].surfels;
        Check(back.size() == src.size(), "chunk level-0 count", (double)back.size(), (double)src.size());
        const uint32_t shCoeffs = SplatCodec::SHCoefficientsPerChannel(shDegree) * 3;
        for (size_t i = 0; i < std::min(back.size(), src.size()); i++)
        {
            maxPosErr = std::max(maxPosErr, (double)std::max(std::fabs(src[i].position.x - back[i].position.x), std::max(std::fabs(src[i].position.y - back[i].position.y), std::fabs(src[i].position.z - back[i].position.z))));
            if (shCoeffs > 0)
            {
                const SplatAttributes& sa = attrs[src[i].sourceIndex];
                float shMax = 0.0f, err = 0.0f;
                for (uint32_t k = 0; k < shCoeffs; k++) shMax = std::max(shMax, std::fabs(sa.sh[k]));
                for (uint32_t k = 0; k < shCoeffs; k++) err = std::max(err, std::fabs(sa.sh[k] - backAttrs[i].sh[k]));
                maxPkgShErr = std::max(maxPkgShErr, (double)(err / std::max(shMax, 1e-6f)));
            }
        }
    }
    const float gExt = std::max(pkg.header.globalBoundsMax.x - pkg.header.globalBoundsMin.x, std::max(pkg.header.globalBoundsMax.y - pkg.header.globalBoundsMin.y, pkg.header.globalBoundsMax.z - pkg.header.globalBoundsMin.z));
    Check(maxPosErr <= gExt / 65535.0f * 1.5f, "package position error", maxPosErr, gExt / 65535.0f * 1.5f);
    Check(total0 == surfels.size(), "package level-0 count", (double)total0, (double)surfels.size());
    if (shDegree > 0) Check(maxPkgShErr <= 1.0 / 127.0 + 2e-3, "package spherical harmonics", maxPkgShErr, 1.0 / 127.0);
    std::cout << "Package round trip: position " << maxPosErr << ", SH " << maxPkgShErr << " of max, SH stream " << totalSH << " bytes" << std::endl;
    Check(levels == maxLODs + 1, "levels in package", (double)levels, (double)(maxLODs + 1));

    std::ifstream out(outputBase + ".sflw", std::ios::ate | std::ios::binary);
    const uint64_t packageBytes = out.is_open() ? (uint64_t)out.tellg() : 0;
    std::cout << "Package: " << packageBytes / (1024.0 * 1024.0) << " MB for " << surfels.size() << " splats over " << levels << " levels ("
              << (double)packageBytes / (double)std::max<size_t>(1, surfels.size()) << " bytes per level-0 splat, all levels included; "
              << pkg.header.splatRecordBytes << " + " << pkg.header.shRecordBytes << " bytes per record before compression)" << std::endl;

    if (g_failures == 0) std::cout << "TestSplatCodec: PASS" << std::endl;
    else std::cout << "TestSplatCodec: " << g_failures << " FAILURE(S)" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
