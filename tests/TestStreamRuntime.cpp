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
//   TestStreamRuntime assets/bicycle/bicycle.gstx --environment [--env-distance <m>] [--env-pitch <radians>]
//
// --environment streams an environment as SplatPak does (Conservative, Release Unused, per-block levels, the residency
// ring) with the eye fixed and the view turning as Fly does, and checks each view after the turn draws about as much
// level 0 as a fresh load at that view, and that under 1% of the blocks in view sit coarser than their own level.
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
    bool conservative = false, release = false, far = false, timeRun = false, viz = false, envRun = false;
    // --environment: the bicycle's subject and ring as SplatPak measures them; the eye 4 m from the subject
    XMFLOAT3 envSubject(0.42f, -0.39f, -0.28f);
    float envSubjectRadius = 2.26f, envRing = 8.27f, envDistance = 4.0f, envPitch = 0.3f;
    for (int a = 1; a < argc; a++)
    {
        const std::string arg = argv[a];
        if (arg == "--environment") envRun = true;
        else if (arg == "--env-distance" && a + 1 < argc) envDistance = (float)atof(argv[++a]);
        else if (arg == "--env-pitch" && a + 1 < argc) envPitch = (float)atof(argv[++a]);
        else if (arg == "--conservative") conservative = true;
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

    if (envRun)
    {
        // An environment as SplatPak streams the bicycle: Conservative, Release Unused, per-block levels at 1 px, the
        // residency ring, unthrottled. The eye stays put and turns as Fly does. Each view is settled once after a turn
        // and once from a fresh start (Evict at that view); the two render lists must draw about the same level 0.
        auto setup = [&](StreamRuntime& r)
        {
            StreamSettings& st = r.settings;
            st.streamingPolicy = StreamSettings::Policy::Conservative;
            st.releaseUnusedSegments = true;
            st.unthrottledBandwidth = true;
            st.autoLOD = true; st.lodScreenSpace = true; st.lodPerBlock = true; st.lodPixelSpacing = 1.0f;
            st.environmentScene = true;
            st.residencyRingRadius = envRing;
            st.subjectCentre = envSubject; st.subjectRadius = envSubjectRadius;
            r.camera.width = 1456; r.camera.height = 939;
        };
        // The eye: envDistance from the subject at yaw 0, pitch envPitch; then the view turns about it (Fly: target 1 m ahead)
        const XMFLOAT3 eye(envSubject.x + envDistance * cosf(envPitch) * sinf(0.0f), envSubject.y + envDistance * sinf(envPitch),
                           envSubject.z + envDistance * cosf(envPitch) * cosf(0.0f));
        auto look = [&](StreamRuntime& r, float yaw)
        {
            r.camera.yaw = yaw; r.camera.pitch = envPitch; r.camera.distance = 1.0f;
            r.camera.target = XMFLOAT3(eye.x - cosf(envPitch) * sinf(yaw), eye.y - sinf(envPitch), eye.z - cosf(envPitch) * cosf(yaw));
        };
        auto drawn = [&](StreamRuntime& r, uint32_t* splats)
        {
            for (int l = 0; l < kMaxStreamLevels; l++) splats[l] = 0;
            for (uint32_t i = 0; i < r.FrameChunkCount() && i < r.m_rendererMeshletChunks.size(); i++)
            {
                const MeshletChunkGPU& c = r.m_rendererMeshletChunks[i];
                if (c.lodLevel < (uint32_t)kMaxStreamLevels) splats[c.lodLevel] += c.splatCount;
            }
        };
        auto print = [&](const char* name, const uint32_t* s)
        {
            printf("  %-8s", name);
            for (size_t l = 0; l < numLevels; l++) printf("  L%zu %8u", l, s[l]);
            printf("\n");
        };
        auto settle = [&](StreamRuntime& r, int frames) { for (int f = 0; f < frames; f++) r.UpdateStreamingSimulation(1.0 / 60.0); };
        setup(rt);
        rt.InitStreamingSimulation();
        rt.ClearResidentStream();
        look(rt, 0.0f);
        settle(rt, 600);
        int failures = 0;
        for (float turn : { 1.5708f, 3.1416f, 4.7124f, 0.0f })
        {
            // Turn over half a second, then settle 10 s
            const float from = rt.camera.yaw;
            float to = turn;
            for (int f = 1; f <= 30; f++) { look(rt, from + (to - from) * f / 30.0f); rt.UpdateStreamingSimulation(1.0 / 60.0); }
            settle(rt, 600);
            StreamRuntime fresh;
            fresh.m_levels = rt.m_levels; fresh.m_waveletLevels = rt.m_waveletLevels;
            fresh.m_aabbMin = rt.m_aabbMin; fresh.m_aabbMax = rt.m_aabbMax; fresh.m_center = rt.m_center; fresh.m_extents = rt.m_extents;
            fresh.m_lodSortFrameMin = rt.m_lodSortFrameMin; fresh.m_lodSortFrameMax = rt.m_lodSortFrameMax;
            fresh.m_detailGrid = rt.m_detailGrid; fresh.m_splatMode = true; fresh.m_splatParams = rt.m_splatParams; fresh.m_bytesPerRenderPoint = rt.m_bytesPerRenderPoint;
            setup(fresh);
            fresh.InitStreamingSimulation();
            fresh.ClearResidentStream();
            look(fresh, turn);
            settle(fresh, 600);
            uint32_t a[kMaxStreamLevels], b[kMaxStreamLevels];
            drawn(rt, a); drawn(fresh, b);
            printf("Yaw %.0f deg:\n", turn * 57.29578f);
            print("turned", a);
            print("fresh", b);
            // The drawn blocks in view (a cone round the view direction, the frame's diagonal), by how far they sit
            // from their own target level (BlockTargetLOD): coarser than it means the view is waiting on detail
            {
                // The runtime's own frustum (StreamRuntime::UpdateStreamingSimulation, the orbit camera)
                const XMMATRIX viewProj = XMMatrixMultiply(XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&rt.camera.target), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)),
                                                           XMMatrixPerspectiveFovRH(XM_PIDIV4, 1456.0f / 939.0f, 0.1f, 500.0f));
                XMFLOAT4X4 vp;
                XMStoreFloat4x4(&vp, viewProj);
                float planes[6][4] = {
                    { vp._14 + vp._11, vp._24 + vp._21, vp._34 + vp._31, vp._44 + vp._41 }, { vp._14 - vp._11, vp._24 - vp._21, vp._34 - vp._31, vp._44 - vp._41 },
                    { vp._14 + vp._12, vp._24 + vp._22, vp._34 + vp._32, vp._44 + vp._42 }, { vp._14 - vp._12, vp._24 - vp._22, vp._34 - vp._32, vp._44 - vp._42 },
                    { vp._13, vp._23, vp._33, vp._43 }, { vp._14 - vp._13, vp._24 - vp._23, vp._34 - vp._33, vp._44 - vp._43 } };
                for (auto& pl : planes) { const float len = sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]); for (float& v : pl) v /= len; }
                auto inFrustum = [&](const XMFLOAT3& c, float r)
                {
                    for (const auto& pl : planes) if (pl[0] * c.x + pl[1] * c.y + pl[2] * c.z + pl[3] < -r) return false;
                    return true;
                };
                uint32_t atTarget = 0, coarser = 0, coarserSplats = 0, finer = 0;
                uint32_t targets[kMaxStreamLevels] = {};
                uint32_t reasons[6] = {};
                for (uint32_t i = 0; i < rt.FrameChunkCount() && i < rt.m_rendererSourceChunks.size(); i++)
                {
                    const StreamChunk* c = rt.m_rendererSourceChunks[i];
                    if (!c) continue;
                    if (!inFrustum(c->center, c->radius)) continue;
                    const int t = rt.BlockTargetLOD(*c, eye);
                    targets[std::clamp(t, 0, kMaxStreamLevels - 1)]++;
                    if (c->lodLevel == t) atTarget++;
                    else if (c->lodLevel > t)
                    {
                        coarser++; coarserSplats += (uint32_t)c->rawSplats.size();
                        // Why it has not refined: its children's state
                        const int fl = (int)c->lodLevel - 1;
                        const auto& kids = rt.m_lodStreamChunks[fl];
                        int resident = 0, requested = 0, pending = 0, outOfView = 0, none = 0;
                        for (size_t ci = c->childBegin; ci < std::min(c->childEnd, kids.size()); ci++)
                        {
                            const StreamChunk& k = kids[ci];
                            const bool kIn = inFrustum(k.center, k.radius * rt.settings.neighborBufferMargin);
                            if (k.isResident) resident++;
                            else if (k.isEvictionPending) pending++;
                            else if (k.isRequested) requested++;
                            else if (!kIn) outOfView++;
                            else none++;
                        }
                        const int kidCount = (int)(std::min(c->childEnd, kids.size()) - c->childBegin);
                        const int why = kidCount <= 0 ? 0 : (resident == kidCount ? 1 : pending ? 2 : requested ? 3 : outOfView ? 4 : 5);
                        reasons[why]++;
                    }
                    else finer++;
                }
                printf("  in view: %u blocks at their target, %u coarser (%u splats), %u finer; targets", atTarget, coarser, coarserSplats, finer);
                for (size_t l = 0; l < numLevels; l++) printf(" L%zu %u", l, targets[l]);
                printf("\n  coarser because: no children %u, all children resident %u, a child eviction-pending %u, a child requested %u, a child outside the view %u, a child neither %u\n",
                    reasons[0], reasons[1], reasons[2], reasons[3], reasons[4], reasons[5]);
                if (coarser * 100 > atTarget) { printf("  FAIL: over 1%% of the blocks in view sit coarser than their level\n"); failures++; }
            }
            if (a[0] < b[0] * 0.8) { printf("  FAIL: level 0 after the turn is %.0f%% of a fresh load's\n", b[0] ? 100.0 * a[0] / b[0] : 0.0); failures++; }
        }
        printf("TestStreamRuntime environment: %s\n", failures ? "FAIL" : "ok");
        return failures ? 1 : 0;
    }

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
