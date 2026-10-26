// TestStreamRuntime.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// GPU-free driver for the streaming runtime (libs/gstx/StreamRuntime.h): loads a splat package
// (the path given, or assets/cthulu/cthulu.gstx), hands its levels to the runtime the way SplatPak does,
// evicts, then streams at a fixed 1/60 s step and 50 MB/s while the camera closes in and orbits. Prints
// per-level residency and the render list size once a simulated second. The step is fixed, so two runs
// print the same lines; the exit code is 0 when level 0 ends fully resident.

#include <cstdio>
#include <cmath>
#include <numeric>
#include "../src/SplatCore/StreamPackager.h"
#include "../src/SplatCore/SpatialOctree.h"
#include "../libs/gstx/StreamRuntime.h"

using namespace Splat;

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : "assets/cthulu/cthulu.gstx";
    StreamPackager::SFLWPackageData pkg;
    std::string reason;
    if (!StreamPackager::LoadPackage(path, pkg, &reason) || !pkg.IsSplatPackage())
    {
        printf("Cannot load %s as a splat package: %s\n", path.c_str(), reason.c_str());
        return 2;
    }

    // Each level: every chunk's records, decoded for positions, then sorted in one spatial order over the
    // package bounds with the records and harmonics carried along (as SplatPakApp::PrecacheResidentLODs)
    const XMFLOAT3 bmin = pkg.header.globalBoundsMin, bmax = pkg.header.globalBoundsMax;
    const uint32_t shBytes = SplatCodec::SHRecordBytes(pkg.splatParams.shDegree);
    size_t numLevels = 0;
    for (const auto& c : pkg.chunkLODSplats) numLevels = std::max(numLevels, c.size());
    std::vector<ResidentLOD> levels(numLevels);
    for (size_t lvl = 0; lvl < numLevels; lvl++)
    {
        std::vector<PackedSplatGPU> records;
        std::vector<uint8_t> sh;
        for (size_t c = 0; c < pkg.chunkLODSplats.size(); c++)
        {
            if (lvl >= pkg.chunkLODSplats[c].size()) continue;
            records.insert(records.end(), pkg.chunkLODSplats[c][lvl].begin(), pkg.chunkLODSplats[c][lvl].end());
            if (lvl < pkg.chunkLODSH[c].size()) sh.insert(sh.end(), pkg.chunkLODSH[c][lvl].begin(), pkg.chunkLODSH[c][lvl].end());
        }
        std::vector<SplatPoint> points;
        std::vector<SplatAttributes> attributes;
        SplatCodec::Decode(records, sh.empty() ? nullptr : sh.data(), sh.size(), pkg.splatParams, (uint32_t)lvl, 0, points, attributes);
        ResidentLOD& out = levels[lvl];
        SpatialOctree::PartitionIntoMeshletChunks(points, out.meshletChunks, 64, true, &bmin, &bmax);
        out.splats.resize(points.size());
        out.splatSH.assign(points.size() * shBytes, 0);
        for (size_t i = 0; i < points.size(); i++)
        {
            const uint32_t src = points[i].sourceIndex;
            out.splats[i] = records[src];
            if (shBytes > 0 && sh.size() >= (size_t)(src + 1) * shBytes)
                std::copy(sh.begin() + (size_t)src * shBytes, sh.begin() + (size_t)(src + 1) * shBytes, out.splatSH.begin() + i * shBytes);
        }
        out.rawSplats = std::move(points);
    }

    StreamRuntime rt;
    rt.m_levels = &levels;
    rt.m_waveletLevels = (int)numLevels;
    rt.m_aabbMin = bmin; rt.m_aabbMax = bmax;
    rt.m_center = XMFLOAT3(0.5f * (bmin.x + bmax.x), 0.5f * (bmin.y + bmax.y), 0.5f * (bmin.z + bmax.z));
    rt.m_extents = XMFLOAT3(bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z);
    rt.m_lodSortFrameMin = bmin; rt.m_lodSortFrameMax = bmax;
    rt.m_detailGrid = pkg.detailGrid;
    rt.m_splatMode = true;
    rt.m_splatParams = pkg.splatParams;
    rt.m_bytesPerRenderPoint = sizeof(PackedSplatGPU) + shBytes;
    rt.camera.target = rt.m_center;
    rt.settings.unthrottledBandwidth = false;
    rt.settings.bandwidthThrottleMBps = 50.0f;
    rt.InitStreamingSimulation();
    printf("%s: %zu levels, %zu blocks, %.1f MB\n", path.c_str(), numLevels, rt.m_allStreamChunkPtrs.size(), rt.m_totalStreamBytes / (1024.0f * 1024.0f));
    rt.ClearResidentStream();

    // 20 s: closing in from four model sizes to one over the first 5 s (the level follows the distance),
    // then a quarter orbit over the next 5 s, then holding still
    const float maxDim = std::max(rt.m_extents.x, std::max(rt.m_extents.y, rt.m_extents.z));
    const double dt = 1.0 / 60.0;
    for (int frame = 1; frame <= 1200; frame++)
    {
        const float t = frame / 60.0f;
        rt.camera.distance = maxDim * (4.0f - 3.0f * std::min(1.0f, t / 5.0f));
        rt.camera.yaw = 0.6f + 1.5708f * std::max(0.0f, std::min(1.0f, (t - 5.0f) / 5.0f));
        rt.settings.selectedPreviewLOD = std::max(0, (int)floorf(rt.ContinuousLodAtDistance(rt.camera.distance, false) + 0.35f));
        rt.UpdateStreamingSimulation(dt);
        if (frame % 60 == 0)
        {
            printf("t=%2ds level %d  render list %6u blocks  resident", frame / 60, rt.settings.selectedPreviewLOD, rt.FrameChunkCount());
            for (size_t l = 0; l < numLevels; l++)
                printf("  L%zu %5.1f%%", l, rt.m_lodTotalSplats[l] ? 100.0 * rt.m_lodResidentSplats[l] / rt.m_lodTotalSplats[l] : 0.0);
            printf("\n");
        }
    }
    const bool full = rt.m_lodResidentSplats[0] == rt.m_lodTotalSplats[0];
    printf("Level 0 %s\n", full ? "fully resident" : "NOT fully resident");
    return full ? 0 : 1;
}
