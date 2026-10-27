// TestStreamRuntime.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// GPU-free driver for the streaming runtime (libs/gstx/StreamRuntime.h): loads a splat package
// (the path given, or assets/cthulu/cthulu.gstx), hands its levels to the runtime the way SplatPak does,
// evicts, then streams at a fixed 1/60 s step and 50 MB/s while the camera closes in and orbits. Prints
// per-level residency and the render list size once a simulated second. The step is fixed, so two runs
// print the same lines; the exit code is 0 when level 0 ends fully resident.
//
//   TestStreamRuntime [package] [--conservative] [--release] [--far]
//   TestStreamRuntime [package] --time [--viz]
//
// --conservative / --release choose the streaming policy and Release Unused LOD Segments; --far stops the approach at
// two model sizes, so the view renders a coarser level and the policies differ. With any of them the run checks the
// policy instead: Greedy streams everything (Release: every level down to its floor, nothing finer resident out of
// view), Conservative leaves part of the view's level unloaded; and in every case few blocks are delivered twice.
//
// --time measures the runtime's CPU cost per frame as SplatPak runs it on an object (Conservative, Release Unused, the
// screen-space per-block levels at 1 px, a 1440 x 900 view, unthrottled): 20 s still at a close view to settle, then
// 10 s orbiting at 36 degrees a second. Prints the mean and 95th-percentile UpdateStreamingSimulation time of each
// phase and the render list. --viz turns on the Refinement Visualizer (showChunkStream). Once still and settled the
// runtime should skip its updates (nothing invalidated the render list); the run then forces one full update and
// fails (exit 1) unless the list it builds is byte for byte the one kept through the skipped updates.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <numeric>
#include "../src/SplatCore/StreamPackager.h"
#include "../src/SplatCore/SpatialOctree.h"
#include "../libs/gstx/StreamRuntime.h"

using namespace Splat;

int main(int argc, char** argv)
{
    std::string path = "assets/cthulu/cthulu.gstx";
    bool conservative = false, release = false, far = false, timeRun = false, viz = false;
    for (int a = 1; a < argc; a++)
    {
        const std::string arg = argv[a];
        if (arg == "--conservative") conservative = true;
        else if (arg == "--release") release = true;
        else if (arg == "--far") far = true;
        else if (arg == "--time") timeRun = true;
        else if (arg == "--viz") viz = true;
        else path = arg;
    }
    const bool policyRun = conservative || release || far;
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
        SplatPointArray points;
        SplatAttributeArray attributes;
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
    rt.settings.streamingPolicy = conservative ? StreamSettings::Policy::Conservative : StreamSettings::Policy::Greedy;
    rt.settings.releaseUnusedSegments = release;
    rt.InitStreamingSimulation();
    printf("%s: %zu levels, %zu blocks, %.1f MB\n", path.c_str(), numLevels, rt.m_allStreamChunkPtrs.size(), rt.m_totalStreamBytes / (1024.0f * 1024.0f));
    rt.ClearResidentStream();

    if (timeRun)
    {
        StreamSettings& st = rt.settings;
        st.streamingPolicy = StreamSettings::Policy::Conservative;
        st.releaseUnusedSegments = true;
        st.unthrottledBandwidth = true;
        st.autoLOD = true; st.lodScreenSpace = true; st.lodPerBlock = true; st.lodPixelSpacing = 1.0f;
        st.showChunkStream = viz;
        rt.camera.width = 1440; rt.camera.height = 900;
        rt.camera.pitch = 0.25f;
        const float dim = std::max(rt.m_extents.x, std::max(rt.m_extents.y, rt.m_extents.z));
        rt.camera.distance = dim * 1.6f;
        auto phase = [&](const char* name, int frames, float yawPerSecond)
        {
            std::vector<double> ms;
            double chunks = 0.0;
            for (int f = 0; f < frames; f++)
            {
                rt.camera.yaw += yawPerSecond / 60.0f;
                const auto t0 = std::chrono::steady_clock::now();
                rt.UpdateStreamingSimulation(1.0 / 60.0);
                ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                chunks += rt.FrameChunkCount();
            }
            std::vector<double> sorted = ms;
            std::sort(sorted.begin(), sorted.end());
            const double mean = std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
            printf("%-22s update %6.2f ms mean, %6.2f ms p95, %6.2f ms max   render list %7.0f blocks   L0 %5.1f%%\n", name, mean,
                sorted[sorted.size() * 95 / 100], sorted.back(), chunks / frames,
                rt.m_lodTotalSplats[0] ? 100.0 * rt.m_lodResidentSplats[0] / rt.m_lodTotalSplats[0] : 0.0);
        };
        printf("Timing (Conservative, Release Unused, per-block 1 px, 1440x900%s)\n", viz ? ", Refinement Visualizer" : "");
        phase("settling, still", 1200, 0.0f);
        phase("still, settled", 300, 0.0f);
        const std::vector<MeshletChunkGPU> kept = rt.m_rendererMeshletChunks;
        const uint32_t skipped = rt.m_skippedUpdates;
        rt.m_streamStateDirty = true; // Forces a full update with the same inputs
        rt.UpdateStreamingSimulation(1.0 / 60.0);
        const bool same = kept.size() == rt.m_rendererMeshletChunks.size() &&
                          memcmp(kept.data(), rt.m_rendererMeshletChunks.data(), kept.size() * sizeof(MeshletChunkGPU)) == 0;
        printf("Idle skip: %u updates skipped; a forced full update rebuilds %s render list\n", skipped, same ? "the same" : "A DIFFERENT");
        phase("orbiting 36 deg/s", 600, 0.6283f);
        phase("still again", 300, 0.0f);
        return (same && skipped > 0) ? 0 : 1;
    }

    // 20 s: closing in from four model sizes to one over the first 5 s (the level follows the distance),
    // then a quarter orbit over the next 5 s, then holding still
    const float maxDim = std::max(rt.m_extents.x, std::max(rt.m_extents.y, rt.m_extents.z));
    const double dt = 1.0 / 60.0;
    for (int frame = 1; frame <= 1200; frame++)
    {
        const float t = frame / 60.0f;
        rt.camera.distance = maxDim * (4.0f - (far ? 2.0f : 3.0f) * std::min(1.0f, t / 5.0f));
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
    if (!policyRun)
    {
        printf("Level 0 %s\n", full ? "fully resident" : "NOT fully resident");
        // The residency panel's locks: every level but 0 locked, then Evict. Level 1 must stay resident and be drawn on the
        // very next update (its blocks show themselves), not fade back up from the coarsest level.
        for (int l = 1; l < (int)numLevels && l < kMaxStreamLevels; l++) rt.m_lodLocked[l] = true;
        rt.ClearResidentStream();
        rt.UpdateStreamingSimulation(dt);
        uint32_t byLevel[kMaxStreamLevels] = {};
        for (const auto& c : rt.m_rendererMeshletChunks) if (c.lodLevel < (uint32_t)kMaxStreamLevels) byLevel[c.lodLevel]++;
        printf("Locked 1..%zu, Evict: L0 %.1f%%  L1 %.1f%% resident; render list by level:", numLevels - 1,
            rt.m_lodTotalSplats[0] ? 100.0 * rt.m_lodResidentSplats[0] / rt.m_lodTotalSplats[0] : 0.0,
            rt.m_lodTotalSplats[1] ? 100.0 * rt.m_lodResidentSplats[1] / rt.m_lodTotalSplats[1] : 0.0);
        for (size_t l = 0; l < numLevels; l++) printf(" L%zu %u", l, byLevel[l]);
        printf("\n");
        uint32_t listTotal = 0;
        for (size_t l = 0; l < numLevels; l++) listTotal += byLevel[l];
        const bool lockHeld = rt.m_lodResidentSplats[1] == rt.m_lodTotalSplats[1] && byLevel[1] * 2 > listTotal;
        printf("Locks %s\n", lockHeld ? "held: level 1 drawn at once" : "FAILED: level 1 not resident or not drawn");
        return (full && lockHeld) ? 0 : 1;
    }
    // The policy checks, at the final (still) view
    auto pct = [&](int l) { return rt.m_lodTotalSplats[l] ? 100.0 * rt.m_lodResidentSplats[l] / rt.m_lodTotalSplats[l] : 0.0; };
    const int view = rt.settings.selectedPreviewLOD;
    int failures = 0;
    printf("%s%s: view level %d, greedy floor %d, %u blocks delivered more than once (%u extra deliveries)\n",
        conservative ? "Conservative" : "Greedy", release ? " + Release Unused LOD Segments" : "", view, rt.GreedyFloor(), rt.m_redeliveredBlocks, rt.m_extraDeliveries);
    if (!conservative && !release)
        for (size_t l = 0; l < numLevels; l++) if (pct((int)l) < 99.9) { printf("FAIL: Greedy left level %zu at %.1f%%\n", l, pct((int)l)); failures++; }
    if (!conservative && release)
        for (int l = rt.GreedyFloor(); l < (int)numLevels; l++) if (pct(l) < 99.9) { printf("FAIL: Greedy + Release left level %d (at or above the floor) at %.1f%%\n", l, pct(l)); failures++; }
    if (conservative && pct(view) > 99.9 && view < (int)numLevels - 2)
    { printf("FAIL: Conservative loaded all of level %d, the far side included\n", view); failures++; }
    if (rt.m_extraDeliveries > rt.m_allStreamChunkPtrs.size() / 50)
    { printf("FAIL: %u extra deliveries, over 2%% of the blocks\n", rt.m_extraDeliveries); failures++; }
    printf("TestStreamRuntime policy: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
