// StreamRuntime.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Open-build streaming runtime: a simple stand-in for the private one, with the same header and the
// same signatures, so the app builds and streams without the prebuilt library. Each level is cut into
// blocks of 64 points in file order; blocks arrive coarsest level first, in chunk order, within the
// bandwidth budget; the view draws one model-wide level, the finest one at or above the target that is
// fully resident, and swaps to the next one the moment it is complete (no cross-fades). By default it keeps
// fetching finer levels once the target is complete; Release Unused LOD Segments stops there and drops finer levels.
// There are no per-block levels, no pacing, no re-ordering after a camera move and no decay.

#include "StreamRuntime.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace Splat
{
    static StreamRuntime::TraceFn s_traceFn = nullptr;
    void StreamRuntime::SetTrace(TraceFn fn) { s_traceFn = fn; }
    void StreamRuntime::Trace(const char* fmt, ...)
    {
        if (!s_traceFn) return;
        char line[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        s_traceFn(line);
    }

    float StreamRuntime::MaxRingBufferMB() const
    {
        return std::max(512.0f, std::ceil(m_totalStreamBytes / (1024.0f * 1024.0f) * 2.0f));
    }

    void StreamRuntime::InitStreamingSimulation()
    {
        m_scratchLoadList.clear();
        m_finishBatch.clear();
        m_lodStreamChunks.clear();
        m_allStreamChunkPtrs.clear();
        m_lodTotalSplats.clear();
        m_lodResidentSplats.clear();
        m_streamOrder.Clear();
        m_rendererMeshletChunks.clear();
        m_rendererSourceChunks.clear();
        m_demandRequestQueue.clear(); ClearEdgeQueue();
        m_demandRequestHead = 0;
        m_smoothedLodResidentPct.clear();
        m_smoothedLodResidentBlocks.clear();
        m_totalStreamBytes = 0.0f;
        m_unifiedPackedSplats.clear();
        m_unifiedRawSplats.clear();
        m_unifiedSplats.clear();
        m_unifiedSH.clear();

        const int numLODs = m_levels ? (int)m_levels->size() : 0;
        if (numLODs == 0) return;
        m_lodStreamChunks.resize(numLODs);
        m_lodTotalSplats.assign(numLODs, 0);
        m_lodResidentSplats.assign(numLODs, 0);
        const uint32_t shBytesPerPoint = m_splatMode ? SplatCodec::SHRecordBytes(m_splatParams.shDegree) : 0;

        for (int lvl = numLODs - 1; lvl >= 0; lvl--)
        {
            const ResidentLOD& lod = (*m_levels)[lvl];
            const size_t numPoints = lod.rawSplats.size();
            m_lodTotalSplats[lvl] = numPoints;
            for (size_t start = 0, c = 0; start < numPoints; start += 64, c++)
            {
                const size_t count = std::min<size_t>(64, numPoints - start);
                StreamChunk sc;
                sc.lodLevel = lvl;
                sc.chunkIndex = c;
                sc.rawSplats.assign(lod.rawSplats.begin() + start, lod.rawSplats.begin() + start + count);
                if (lod.packedPoints.size() >= start + count)
                {
                    sc.packedPoints.assign(lod.packedPoints.begin() + start, lod.packedPoints.begin() + start + count);
                    m_unifiedPackedSplats.insert(m_unifiedPackedSplats.end(), lod.packedPoints.begin() + start, lod.packedPoints.begin() + start + count);
                }
                sc.globalSplatOffset = (uint32_t)m_unifiedRawSplats.size();
                m_unifiedRawSplats.insert(m_unifiedRawSplats.end(), lod.rawSplats.begin() + start, lod.rawSplats.begin() + start + count);
                if (m_splatMode && lod.splats.size() >= start + count)
                {
                    m_unifiedSplats.insert(m_unifiedSplats.end(), lod.splats.begin() + start, lod.splats.begin() + start + count);
                    if (shBytesPerPoint > 0 && lod.splatSH.size() >= (start + count) * shBytesPerPoint)
                        m_unifiedSH.insert(m_unifiedSH.end(), lod.splatSH.begin() + start * shBytesPerPoint, lod.splatSH.begin() + (start + count) * shBytesPerPoint);
                    else if (shBytesPerPoint > 0)
                        m_unifiedSH.resize(m_unifiedSH.size() + count * shBytesPerPoint, 0);
                }
                sc.byteSize = count * m_bytesPerRenderPoint;
                sc.isRequested = sc.isDelivered = sc.isResident = true;

                // Bounds: centroid, enclosing radius, box, mean normal
                XMFLOAT3 center = { 0, 0, 0 }, n = { 0, 0, 0 };
                XMFLOAT3 mn = { 1e9f, 1e9f, 1e9f }, mx = { -1e9f, -1e9f, -1e9f };
                for (const auto& s : sc.rawSplats)
                {
                    center.x += s.position.x; center.y += s.position.y; center.z += s.position.z;
                    n.x += s.normal.x; n.y += s.normal.y; n.z += s.normal.z;
                    mn.x = std::min(mn.x, s.position.x); mn.y = std::min(mn.y, s.position.y); mn.z = std::min(mn.z, s.position.z);
                    mx.x = std::max(mx.x, s.position.x); mx.y = std::max(mx.y, s.position.y); mx.z = std::max(mx.z, s.position.z);
                }
                center.x /= (float)count; center.y /= (float)count; center.z /= (float)count;
                float radius = 0.0f;
                for (const auto& s : sc.rawSplats)
                {
                    const float dx = s.position.x - center.x, dy = s.position.y - center.y, dz = s.position.z - center.z;
                    radius = std::max(radius, sqrtf(dx * dx + dy * dy + dz * dz));
                }
                const float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
                sc.avgNormal = nl > 1e-4f ? XMFLOAT3(n.x / nl, n.y / nl, n.z / nl) : XMFLOAT3(0.0f, 1.0f, 0.0f);
                sc.center = center;
                sc.radius = radius;
                sc.aabbMin = mn;
                sc.aabbMax = mx;
                sc.detailScore = m_detailGrid.Valid() ? m_detailGrid.SampleBox(mn, mx) : 0.0f;
                m_totalStreamBytes += (float)sc.byteSize;
                m_lodStreamChunks[lvl].push_back(std::move(sc));
            }
            for (auto& chunk : m_lodStreamChunks[lvl]) m_allStreamChunkPtrs.push_back(&chunk);
            m_lodResidentSplats[lvl] = numPoints;
        }

        // The open StreamOrder's per-level lists
        std::vector<StreamOrder::Block> blocks;
        blocks.reserve(m_allStreamChunkPtrs.size());
        for (uint32_t h = 0; h < (uint32_t)m_allStreamChunkPtrs.size(); h++)
        {
            const StreamChunk* pc = m_allStreamChunkPtrs[h];
            StreamOrder::Block b;
            b.handle = h; b.lodLevel = pc->lodLevel; b.chunkIndex = (uint32_t)pc->chunkIndex;
            b.center = pc->center; b.byteSize = (uint32_t)pc->byteSize; b.detailScore = pc->detailScore;
            blocks.push_back(b);
        }
        m_streamOrder.Build(blocks);

        ComputeLodSpacing();
        CalibrateDecay();
        settings.ringBufferCapacityMB = MaxRingBufferMB();
        m_simulatedBytesDelivered = m_totalStreamBytes;
        m_streamRefinementProgress = 1.0f;
        m_streamStateDirty = true;
        Trace("StreamRuntime (open stand-in): %d levels, %zu blocks, %.1f MB", numLODs, m_allStreamChunkPtrs.size(), m_totalStreamBytes / (1024.0f * 1024.0f));
    }

    // Drops every level below the two coarsest, which stay resident (the model's envelope)
    void StreamRuntime::ClearResidentStream()
    {
        const int numLODs = (int)m_lodStreamChunks.size();
        const int minProtectedLvl = std::max(0, numLODs - 2);
        m_evictedSplatCount = 0;
        for (int lvl = 0; lvl < numLODs; lvl++)
        {
            const bool keep = lvl >= minProtectedLvl;
            for (auto& sc : m_lodStreamChunks[lvl])
            {
                sc.isRequested = sc.isDelivered = sc.isResident = keep;
                sc.isEvictionPending = sc.isLockedInTransition = sc.isSilhouette = false;
                sc.transitionProgress = 0.0f;
                sc.deliveryCount = 0;
            }
        }
        m_residencyEpoch++;
        m_demandRequestQueue.clear(); ClearEdgeQueue();
        m_demandRequestHead = 0;
        m_streamStateDirty = true;
        UpdateStreamingSimulation(0.0);
    }

    void StreamRuntime::ResetStreamingSimulation()
    {
        ClearResidentStream();
    }

    void StreamRuntime::RequestChunk(int lodLevel, size_t chunkIndex, float priority)
    {
        if (lodLevel < 0 || lodLevel >= (int)m_lodStreamChunks.size() || chunkIndex >= m_lodStreamChunks[lodLevel].size()) return;
        StreamChunk& c = m_lodStreamChunks[lodLevel][chunkIndex];
        if (c.isResident || c.isRequested) return;
        c.isRequested = true;
        m_demandRequestQueue.push_back({ lodLevel, chunkIndex, priority });
    }

    void StreamRuntime::TriggerSilhouetteEdgeMorphTest()
    {
    }

    void StreamRuntime::UpdateStreamingSimulation(double dtSeconds)
    {
        m_streamFrame++;
        m_frameProduced = false;
        if (!settings.enableStreamingSimulation || m_allStreamChunkPtrs.empty()) return;
        const int numLODs = (int)m_lodStreamChunks.size();
        const int targetLOD = std::max(0, std::min(numLODs - 1, settings.selectedPreviewLOD));
        const bool release = settings.releaseUnusedSegments;

        const float camPitch = camera.detachCamera ? camera.detachedPitch : camera.pitch;
        const float camYaw   = camera.detachCamera ? camera.detachedYaw : camera.yaw;
        const float camDist  = camera.detachCamera ? camera.detachedDistance : camera.distance;
        const XMFLOAT3 camTarget = camera.detachCamera ? camera.detachedTarget : camera.target;
        const XMFLOAT3 eyePos = camera.explicitView ? camera.eye // A host with its own camera gives the eye directly
            : XMFLOAT3(camTarget.x + camDist * cosf(camPitch) * sinf(camYaw), camTarget.y + camDist * sinf(camPitch), camTarget.z + camDist * cosf(camPitch) * cosf(camYaw));
        m_streamOrder.UpdateVisibility(eyePos, m_center);
        m_lastStreamCamPos = eyePos;

        auto deliver = [&](StreamChunk& c)
        {
            c.isRequested = c.isDelivered = c.isResident = true;
            c.deliveryCount++;
            c.deliverySeq = ++m_deliverySeqCounter;
            c.streamWavePending = true;
            m_streamStateDirty = true;
        };

        // Delivery: host requests first, then every level from the coarsest down to the target (to level 0
        // unless Release Unused LOD Segments is on) in chunk order, until the frame's bandwidth runs out
        if (!settings.isStreamingPaused)
        {
            float budget = settings.unthrottledBandwidth ? 1e12f : (float)(dtSeconds * settings.bandwidthThrottleMBps * 1024.0 * 1024.0);
            for (; m_demandRequestHead < m_demandRequestQueue.size() && budget > 0.0f; m_demandRequestHead++)
            {
                const ChunkRequest& r = m_demandRequestQueue[m_demandRequestHead];
                StreamChunk& c = m_lodStreamChunks[r.lodLevel][r.chunkIndex];
                if (c.isResident) continue;
                deliver(c);
                budget -= (float)c.byteSize;
            }
            if (m_demandRequestHead >= m_demandRequestQueue.size()) { m_demandRequestQueue.clear(); m_demandRequestHead = 0; }
            const int finest = (release || !Greedy()) ? targetLOD : 0; // Conservative: nothing past the view's level
            for (int lvl = numLODs - 1; lvl >= finest && budget > 0.0f; lvl--)
            {
                for (auto& c : m_lodStreamChunks[lvl])
                {
                    if (c.isResident) continue;
                    if (budget <= 0.0f) break;
                    deliver(c);
                    budget -= (float)c.byteSize;
                }
            }
        }

        // Release Unused LOD Segments: detail finer than the target goes
        if (release)
        {
            for (int lvl = 0; lvl < targetLOD && lvl < numLODs - 2; lvl++)
            {
                for (auto& c : m_lodStreamChunks[lvl])
                {
                    if (!c.isResident) continue;
                    c.isRequested = c.isDelivered = c.isResident = false;
                    m_evictedSplatCount += c.rawSplats.size();
                    m_residencyEpoch++;
                    m_streamStateDirty = true;
                }
            }
        }

        // Residency readouts
        m_simulatedBytesDelivered = 0.0f;
        for (int lvl = 0; lvl < numLODs; lvl++)
        {
            m_lodResidentSplats[lvl] = 0;
            for (const auto& c : m_lodStreamChunks[lvl])
            {
                if (!c.isResident) continue;
                m_lodResidentSplats[lvl] += c.rawSplats.size();
                m_simulatedBytesDelivered += (float)c.byteSize;
            }
        }
        m_streamRefinementProgress = (m_totalStreamBytes > 0.0f) ? std::min(1.0f, m_simulatedBytesDelivered / m_totalStreamBytes) : 1.0f;

        // Render list: the finest level at or above the target that is fully resident, drawn whole
        const auto tStart = std::chrono::high_resolution_clock::now();
        int drawLevel = numLODs - 1;
        for (int lvl = targetLOD; lvl < numLODs; lvl++)
        {
            if (m_lodResidentSplats[lvl] == m_lodTotalSplats[lvl]) { drawLevel = lvl; break; }
        }
        for (auto* pc : m_allStreamChunkPtrs)
        {
            pc->streamWaveTimer = std::max(0.0f, pc->streamWaveTimer - (float)dtSeconds);
            pc->appendedLastFrame = pc->appendedThisFrame;
            pc->appendedThisFrame = pc->renderedLastFrame = false;
        }
        m_rendererMeshletChunks.clear();
        m_rendererSourceChunks.clear();
        for (auto& c : m_lodStreamChunks[drawLevel])
        {
            if (c.rawSplats.empty()) continue;
            if (c.streamWavePending) { c.streamWavePending = false; c.streamWaveTimer = settings.chunkStreamDuration; }
            const bool glow = settings.showChunkStream && settings.chunkStreamDuration > 0.0f && c.streamWaveTimer > 0.0f;
            MeshletChunkGPU g = {};
            g.center = c.center;
            g.boundingRadius = c.radius;
            g.aabbMin = XMFLOAT3(c.center.x - c.radius, c.center.y - c.radius, c.center.z - c.radius);
            g.aabbExtents = XMFLOAT3(c.radius * 2.0f, c.radius * 2.0f, c.radius * 2.0f);
            g.splatOffset = c.globalSplatOffset;
            g.splatCount = (uint32_t)c.rawSplats.size();
            g.blendWeight = 1.0f;
            g.lodLevel = (uint32_t)c.lodLevel;
            g.dilationMorph = (m_splatMode && !settings.splatDrawAsDiscs) ? 1.0f : 0.0f; // Splat mode: full opacity
            g.isSilhouette = glow ? std::min(0.95f, c.streamWaveTimer / settings.chunkStreamDuration) : 0.0f;
            m_rendererMeshletChunks.push_back(g);
            m_rendererSourceChunks.push_back(&c);
            c.renderedLastFrame = c.appendedThisFrame = true;
        }
        m_frameTraversalMs = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
        m_lastActiveTransitions = 0;
        m_frameChunkCount = (uint32_t)std::min<size_t>(m_rendererMeshletChunks.size(), 150000);
        m_frameProduced = true;
        m_streamStateDirty = false;
    }

    float StreamRuntime::LodEffectiveDistance(float cameraDistance) const
    {
        return cameraDistance;
    }

    float StreamRuntime::LodFocalPixels() const
    {
        return 0.5f * (float)std::max(1, (int)camera.height) / tanf(XM_PIDIV4 * 0.5f);
    }

    // One rule for both modes: level 0 within one model size of the target, the coarsest at four
    float StreamRuntime::ContinuousLodAtDistance(float cameraDistance, bool) const
    {
        const int maxLODIndex = m_waveletLevels - 1;
        if (maxLODIndex <= 0) return 0.0f;
        const float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
        const float factor = std::max(0.0f, std::min(1.0f, (cameraDistance / std::max(0.1f, maxDim) - 1.0f) / 3.0f));
        return factor * (float)maxLODIndex;
    }

    float StreamRuntime::ContinuousLodAtDepth(float viewDepth) const
    {
        return ContinuousLodAtDistance(viewDepth, true);
    }

    bool StreamRuntime::PerBlockLodActive() const
    {
        return false;
    }

    int StreamRuntime::BlockTargetLOD(const StreamChunk&, const XMFLOAT3&) const
    {
        return std::max(0, settings.selectedPreviewLOD);
    }

    float StreamRuntime::LodSwitchDistance(int level, bool) const
    {
        const int maxLODIndex = m_waveletLevels - 1;
        const float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
        const float factor = ((float)level - 0.35f) / (float)std::max(1, maxLODIndex);
        return (1.0f + 3.0f * factor) * std::max(0.1f, maxDim);
    }

    float StreamRuntime::EvictableStreamBytes() const
    {
        return 0.0f;
    }

    void StreamRuntime::CalibrateDecay()
    {
        m_decayBytesPerSec = 0.0f;
        m_decayReferenceBandwidthBps = 0.0f;
    }

    float StreamRuntime::DecayFullDrainSeconds() const
    {
        return 0.0f;
    }

    // Typical point spacing per level (Auto Splat Size): the median distance to the nearest neighbour over
    // a sample of up to 4000 points, found by a sweep along x
    void StreamRuntime::ComputeLodSpacing()
    {
        const int numLODs = (int)m_lodStreamChunks.size();
        m_lodSpacing.assign(numLODs, 0.0f);
        for (int lvl = 0; lvl < numLODs; lvl++)
        {
            std::vector<XMFLOAT3> pts;
            for (const auto& c : m_lodStreamChunks[lvl]) for (const auto& v : c.rawSplats) pts.push_back(v.position);
            if (pts.size() < 8) continue;
            std::sort(pts.begin(), pts.end(), [](const XMFLOAT3& a, const XMFLOAT3& b) { return a.x < b.x; });
            const size_t stride = std::max<size_t>(1, pts.size() / 4000);
            std::vector<float> dists;
            for (size_t i = 0; i < pts.size(); i += stride)
            {
                float best = 1e30f;
                for (int dir = -1; dir <= 1; dir += 2)
                {
                    for (ptrdiff_t j = (ptrdiff_t)i + dir; j >= 0 && j < (ptrdiff_t)pts.size(); j += dir)
                    {
                        const float dx = pts[j].x - pts[i].x;
                        if (dx * dx >= best) break;
                        const float dy = pts[j].y - pts[i].y, dz = pts[j].z - pts[i].z;
                        best = std::min(best, dx * dx + dy * dy + dz * dz);
                    }
                }
                if (best < 1e30f) dists.push_back(sqrtf(best));
            }
            std::nth_element(dists.begin(), dists.begin() + dists.size() / 2, dists.end());
            m_lodSpacing[lvl] = dists[dists.size() / 2];
        }
    }

    void StreamRuntime::HealStaleRequestFlags()
    {
    }

    void StreamRuntime::ClearEdgeQueue()
    {
        m_extraDeliveries = 0;
        m_redeliveredBlocks = 0;
        m_edgeQueue.clear();
        m_edgeHead = m_edgeSortedEnd = 0;
    }
}
