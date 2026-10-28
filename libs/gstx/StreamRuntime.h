// StreamRuntime.h
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
//
// The streaming runtime: which blocks of a model are resident, at which level, in what order they
// arrive, and how they hand over to one another. A host (SplatPak, the UE5 plugin) hands it the levels
// of a model, sets the settings and the camera, calls UpdateStreamingSimulation every frame, and draws
// the render list (the first FrameChunkCount() entries of m_rendererMeshletChunks) over the unified
// buffers. The implementation is part of the codec library; this header is its public API.
//
// The state fields are public so a host's debug views (residency graph, readouts) can
// read them. Hosts do not write them, except m_streamStateDirty to ask for a fresh render list.

#pragma once
#include <vector>
#include <cstdint>
#include <utility>
#include "StreamOrder.h"
#include "DetailHeatmap.h"
#include "SplatCodec.h"
#include "GstxTypes.h"

namespace Splat
{
    struct StreamChunk
    {
        int      lodLevel = 0;
        size_t   chunkIndex = 0;
        uint32_t orderHandle = 0;              // Its index in m_allStreamChunkPtrs: the streaming scheduler's handle for it
        XMFLOAT3 center = { 0, 0, 0 };
        float    radius = 0.0f;
        // The block's point spacing over its level's typical one (the level's median block): about 1 in a dense
        // region, larger where the level thins out (an environment's background). Scales the coarse levels' Gaussian
        // cap per block, so a sparse block's Gaussians are not shrunk to specks by a spacing measured in the core.
        float    spacingRatio = 1.0f;
        XMFLOAT3 avgNormal = { 0, 1, 0 };      // Representative surface normal for silhouette edge testing
        float    normalSpread = 0.0f;          // Normal angular variation
        float    detailScore = 0.0f;           // 0..1 from the package's detail grid over this block's bounds (DetailGrid::SampleBox); an input to the streaming order
        SplatPointArray   rawSplats;
        std::vector<PackedPointGPU> packedPoints;
        size_t   byteSize = 0;
        float    currentPriority = 0.0f;
        bool     isRequested = false;
        bool     isEdgeQueued = false;         // Waiting in one of the runtime's delivery queues
        uint32_t lastRequestFrame = 0;         // m_streamFrame of the traversal's most recent request for this block; an edge-queue entry not re-requested for a while is stale and dropped
        uint32_t deliveryCount = 0;            // Deliveries since the last streaming reset (diagnostic: > 1 means the block was evicted and streamed again)
        bool     isDelivered = false;
        bool     isResident = false;
        bool     isEvictionPending = false;    // Marked for eviction: waiting for parent demotion transition to complete
        bool     isLockedInTransition = false; // Locked against eviction while transition is running in either direction
        bool     hasRefined = false;           // Has refined into its children (CASE 2) since they last arrived. Only then does dropping back to this node (CASE 1) evict them; children that came in ahead of need and were never shown stay resident for when the view gets closer
        bool     refinedBySilhouette = false;  // The refinement into its children was edge-driven (this node sits at or below the target level). When the flag drops and it demotes, the children are kept resident: evicting them only made the load list re-stream them for the next flag, which flashed the visible faces while spinning
        bool     isSilhouette = false;         // Active in-view silhouette edge chunk (locked against eviction)
        float    transitionProgress = 0.0f;   // 0.0 (Parent Level N Solid) <-> 1.0 (Children Level N-1 Solid)
        float    streamWaveTimer = 0.0f;      // Arrival glow (Refinement Visualizer) countdown, m_chunkStreamDuration -> 0; started by the chunk's first draw after delivery (see streamWavePending)
        bool     streamWavePending = false;   // Delivered but not yet drawn: the arrival glow is armed and starts on the first draw, so a block that waits before it is shown still glows when it appears instead of expiring unseen
        float    demoteWaveTimer = 0.0f;      // Demotion glow (Refinement Visualizer, its own colour): a parent fading back in over its children, m_chunkStreamDuration -> 0
        uint32_t deliverySeq = 0;             // Order of delivery by the stream (1 = first delivered this session; 0 = resident at load)
        float    silhouetteHysteresisTimer = 0.0f; // Hysteresis hold time (seconds) to eliminate refinement/demotion thrashing
        bool     renderedLastFrame = false;    // Visited by the previous frame's traversal: rendered, or refined in place of by its children. Only such "active" chunks may keep an edge flag; anything the traversal never reached (a level outside the active range) is cleared before the next traversal. Clearing on "not rendered" alone made a refined edge parent lose its flag, re-render, get re-detected and refine again every other frame -- a whole-model flicker
        uint32_t globalSplatOffset = 0;      // Zero-copy offset into m_unifiedPackedSplats / m_unifiedRawSplats
        XMFLOAT3 aabbMin = { 0, 0, 0 };
        XMFLOAT3 aabbMax = { 0, 0, 0 };
        size_t   childBegin = 0;              // The block's children at the next finer level: m_lodStreamChunks[lodLevel - 1][childBegin, childEnd),
        size_t   childEnd = 0;                // covering the same surface as the block
        size_t   parentIndex = SIZE_MAX;      // The block's parent at the next coarser level (SIZE_MAX at the coarsest level, or when the levels are not nested)
        bool     appendedThisFrame = false;   // In this frame's render list, and in the last frame's
        bool     appendedLastFrame = false;
    };


    struct ChunkRequest
    {
        int    lodLevel = 0;
        size_t chunkIndex = 0;
        float  priority = 0.0f;
    };



    struct ResidentLOD
    {
        SplatPointArray rawSplats;
        std::vector<PackedPointGPU> packedPoints;
        std::vector<PackedSplatGPU> splats;   // Splat mode: the level's 20-byte Gaussian records
        std::vector<uint8_t>        splatSH;  // Splat mode: the level's spherical-harmonics stream
        std::vector<MeshletChunkGPU> meshletChunks;
    };


    // What the host controls; copied from its UI every frame
    struct StreamSettings
    {
        bool            autoLOD = true;                      // The level follows the camera distance (the host's auto LOD sets selectedPreviewLOD)
        int             selectedPreviewLOD = 0;              // The model-wide level the view renders
        bool            lodScreenSpace = true;               // Screen-space LOD rule
        float           lodPixelSpacing = 1.0f;              // Its pixel threshold
        bool            lodPerBlock = false;                 // Per-block levels
        bool            enableStreamingSimulation = true;
        bool            isStreamingPaused = false;
        bool            unthrottledBandwidth = true;
        float           bandwidthThrottleMBps = 10.0f;
        float           ringBufferCapacityMB = 512.0f;
        bool            enableStreamDecay = false;
        float           streamDecayRate = 5.0f;
        bool            demoteChunks = false;
        // Release Unused LOD Segments: a segment is evicted once nothing draws it any more. Zooming out demotes
        // (as demoteChunks) and a finished demotion evicts the finer segments below it; requests and the
        // scheduler's prefetch stop at the view's level, and the view's own requests cover only what is in view
        // plus a neighbour buffer (neighborBufferMargin), so detail the view leaves is evicted and nothing streams
        // it back. The two coarsest levels stay resident as always. On by default (as in SplatPak); off, everything
        // that arrives stays, and the scheduler keeps prefetching down to level 0 until the whole model is resident.
        bool            releaseUnusedSegments = true;
        // What streams besides the view's own requests. Conservative: only what the view needs -- the blocks in view plus
        // the neighbour buffer, each down to its own level -- and nothing in the background. Greedy (the default): the
        // stream carries on in the background regardless of the view; with releaseUnusedSegments it loads every block of
        // every level down to the finest level any visible block renders at (GreedyFloor), model-wide, and releases only
        // what lies finer than that, so nothing it loads is released again.
        enum class Policy : int { Conservative = 0, Greedy = 1 };
        Policy          streamingPolicy = Policy::Greedy;
        bool            enableDitheredTransitions = true;
        float           ditherTransitionDurationSec = 0.75f;
        bool            prioritizeFrustumAndProximity = true;
        float           neighborBufferMargin = 1.35f;      // Release Unused LOD Segments: frustum margin of the neighbour buffer
        bool            enableSilhouetteLOD0 = false;
        bool            highlightSilhouetteChunks = false;
        int             silhouetteLODBias = 2;
        float           silhouetteThreshold = 0.40f;
        float           dilationMorphAmount = 0.25f;
        bool            showChunkStream = true;              // Arrival glow on delivered blocks
        float           chunkStreamDuration = 2.5f;
        bool            splatDrawAsDiscs = false;
        // An environment scene (docs/design/environment-scenes.md): a capture of a place, viewed from inside. Requests rank
        // by screen-space error (a block's level spacing over its distance, weighted by whether it is in view) and are
        // delivered strictly in that order. "Needed" is in the frustum (plus the neighbour buffer) or inside the residency ring: every block
        // within residencyRingRadius of the eye (a flattened disc: half as far up and down) is requested at the level its
        // distance alone asks for, behind everything in view, so turning round finds it loaded.
        bool            environmentScene = false;
        float           residencyRingRadius = 0.0f;          // Metres; 0 = no ring
        // The view's requests ranked by screen-space error and delivered strictly in that order, for every model (an
        // environment always): measured 2026-10-26 at 5G, 2.4-2.6x as much level 0 on screen at each second on the bicycle
        // and about 2.6x the view's requests delivered per second on Cthulhu, against the octahedron's face-balanced
        // order (the retired octahedron scheduler's). Off: the view's requests get only a share of each frame ahead of
        // the scheduler's load list. Objects filter out their hidden side with FacesEye either way.
        bool            screenSpaceOrder = true;
        // What the scan is of (PackageView's subject: where the training cameras converge, out to where the density falls
        // away): blocks touching it rank kSubjectBoost times higher, for delivery and for the cross-fade admission alike,
        // so the subject reaches its level before the scenery around it. Radius 0 = none.
        XMFLOAT3        subjectCentre = { 0.0f, 0.0f, 0.0f };
        float           subjectRadius = 0.0f;
        bool            subjectBoost = true;             // Off: the subject is only measured (the render-list trace), not ranked higher
    };

    // The camera the stream follows (the frozen one, when detached), and the viewport
    struct StreamCamera
    {
        float    pitch = 0.35f, yaw = 0.6f, distance = 25.0f;
        XMFLOAT3 target = { 0.0f, 0.0f, 0.0f };
        bool     detachCamera = false;
        float    detachedPitch = 0.35f, detachedYaw = 0.6f, detachedDistance = 25.0f;
        XMFLOAT3 detachedTarget = { 0.0f, 0.0f, 0.0f };
        uint32_t width = 1440, height = 900;
        // A host with its own camera (the UE5 plugin) sets explicitView and gives the view directly, in model space:
        // the eye, the view direction, the view-projection (row vectors, clip = p * viewProj, clip z over [0, w] in
        // either direction, so reversed and infinite depth work) and the vertical field of view. The orbit fields
        // above are then ignored.
        bool       explicitView = false;
        XMFLOAT3   eye = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3   forward = { 0.0f, 0.0f, -1.0f };
        XMFLOAT4X4 viewProj = {};
        float      fovY = 0.78539816f;
    };

    class StreamRuntime
    {
    public:
        static constexpr size_t kMaxEdgeQueue = 65536;
        static constexpr bool   kSplatGaussianLevel0 = true;  // Splat packages: level 0 drawn as Gaussians
        static constexpr float  kMaxDecayRate = 10.0f;                   // Decay slider maximum
        static constexpr float  kDecayFullDrainSecondsAtMaxRate = 10.0f; // Full-drain time at rate 10, independent of bandwidth

        StreamSettings settings;
        StreamCamera   camera;

        // Trace output: one formatted line per call; the host adds timestamps (null = no trace)
        using TraceFn = void (*)(const char* line);
        static void SetTrace(TraceFn fn);

        // Inputs, set before InitStreamingSimulation (the host keeps m_levels alive while the runtime is in use)
        const std::vector<ResidentLOD>* m_levels = nullptr;  // Every level of the model, finest first, each in the same spatial order over the model bounds
        int               m_greedyFloor = 0;
        int               m_waveletLevels = 0;               // Number of levels the model has (the LOD rule's range)
        XMFLOAT3          m_aabbMin = { 0, 0, 0 }, m_aabbMax = { 0, 0, 0 }, m_center = { 0, 0, 0 }, m_extents = { 0, 0, 0 };
        XMFLOAT3          m_lodSortFrameMin = { 0, 0, 0 }, m_lodSortFrameMax = { 0, 0, 0 };
        DetailGrid        m_detailGrid;
        bool              m_enableMortonOrder = true;
        bool              m_splatMode = true;
        SplatEncodeParams m_splatParams;
        size_t            m_bytesPerRenderPoint = 20;

        // Levels locked against eviction (the residency panel's padlocks): their resident blocks stay resident through
        // view-driven release, decay and the Evict button; only a Re-Stream reset clears them
        bool              m_lodLocked[kMaxStreamLevels] = {};
        bool LevelLocked(int lvl) const { return lvl >= 0 && lvl < kMaxStreamLevels && m_lodLocked[lvl]; }

        float MaxRingBufferMB() const;
        void InitStreamingSimulation();
        void ResetStreamingSimulation();
        void ClearResidentStream();
        void RequestChunk(int lodLevel, size_t chunkIndex, float priority);
        void TriggerSilhouetteEdgeMorphTest();
        void UpdateStreamingSimulation(double dtSeconds);
        float LodEffectiveDistance(float cameraDistance) const;
        float LodFocalPixels() const;
        float ContinuousLodAtDistance(float cameraDistance, bool screenSpace) const;
        float ContinuousLodAtDepth(float viewDepth) const;
        bool PerBlockLodActive() const;
        int BlockTargetLOD(const StreamChunk& c, const XMFLOAT3& eye) const;
        // Environment scenes: whether a block lies in the residency ring around the eye (settings.residencyRingRadius;
        // a flattened disc, its height half its radius)
        bool InResidencyRing(const StreamChunk& c, const XMFLOAT3& eye) const
        {
            const float r = settings.residencyRingRadius;
            if (!settings.environmentScene || r <= 0.0f) return false;
            const float dx = c.center.x - eye.x, dy = (c.center.y - eye.y) * 2.0f, dz = c.center.z - eye.z;
            const float reach = r + c.radius;
            return dx * dx + dy * dy + dz * dz <= reach * reach;
        }
        // Objects: whether a block can face the eye (the far side of the model is hidden behind its near side, so it is
        // neither refined nor, under Conservative, loaded). See the definition.
        bool FacesEye(const StreamChunk& c, const XMFLOAT3& eye) const;
        float m_ringRequestTimer = 0.0f;           // The residency ring's requests are refreshed four times a second
        bool Greedy() const { return settings.streamingPolicy == StreamSettings::Policy::Greedy; }
        int  GreedyFloor() const { return m_greedyFloor; } // Greedy with releaseUnusedSegments: the finest level a visible block renders at (this frame)
        float LodSwitchDistance(int level, bool screenSpace) const;
        float EvictableStreamBytes() const;
        void CalibrateDecay();
        float DecayFullDrainSeconds() const;
        void ComputeLodSpacing();
        void HealStaleRequestFlags();
        void ClearEdgeQueue();
        uint32_t FrameChunkCount() const { return m_frameChunkCount; }

        // State (read-only for hosts, see the file comment)
        std::vector<std::vector<StreamChunk>> m_lodStreamChunks;
        std::vector<StreamChunk*>             m_allStreamChunkPtrs;
        std::vector<size_t>                   m_lodTotalSplats, m_lodResidentSplats;
        std::vector<float>                    m_lodSpacing;
        std::vector<float>                    m_smoothedLodResidentPct;
        std::vector<uint32_t>                 m_smoothedLodResidentBlocks;
        std::vector<MeshletChunkGPU>          m_rendererMeshletChunks;
        std::vector<StreamChunk*>             m_rendererSourceChunks;
        std::vector<PackedPointGPU>          m_unifiedPackedSplats;
        SplatPointArray             m_unifiedRawSplats;
        std::vector<PackedSplatGPU>           m_unifiedSplats;
        std::vector<uint8_t>                  m_unifiedSH;
        StreamOrder               m_streamOrder;
        std::vector<ChunkRequest> m_demandRequestQueue;
        size_t                    m_demandRequestHead = 0;
        std::vector<ChunkRequest> m_edgeQueue;                 // TIER 1 (silhouette edges, bootstrap envelope), delivered ahead of the demand queue
        size_t                    m_edgeHead = 0;
        size_t                    m_edgeSortedEnd = 0;         // m_edgeQueue is ranked up to here (new entries are ranked as they are delivered)
        std::vector<uint32_t>     m_scratchLoadHandles;
        std::vector<StreamChunk*> m_scratchLoadList;
        std::vector<StreamChunk*> m_finishBatch;
        std::vector<std::pair<uint32_t, uint32_t>> m_refineDeferred;
        float    m_simulatedBytesDelivered = 0.0f, m_totalStreamBytes = 0.0f, m_streamRefinementProgress = 1.0f;
        float    m_decayBytesPerSec = 0.0f, m_decayReferenceBandwidthBps = 0.0f;
        size_t   m_evictedSplatCount = 0;
        uint32_t m_deliverySeqCounter = 0, m_residencyEpoch = 0, m_streamFrame = 0;
        uint32_t m_redeliveredBlocks = 0, m_extraDeliveries = 0;
        uint32_t m_lastActiveTransitions = 0, m_edgeRefineStartsThisFrame = 0;
        uint32_t m_refineStartsThisFrame = 0, m_refineStartBudget = 0, m_refineStartsDeferred = 0, m_refineAdmitSeqLimit = 0;
        uint32_t m_frameChunkCount = 0;
        // Bumped wherever a block's residency changes (the idle skip's test). m_lodResidentSplats is kept exact as single blocks land and leave (CountResidency); a bulk change (load,
        // reset, clear) marks them invalid and the next update recounts once. Recounting every frame cost two passes
        // over every block, about 1.5 ms on Cthulhu (137,104 blocks).
        uint32_t m_residencySerial = 0;
        bool     m_residencyCountsValid = false;
        bool     m_waveTimersRunning = false; // Some block's arrival glow is counting down (the countdown pass is skipped otherwise)
        // Idle skip: full updates in a row that changed nothing, and the camera and settings they saw. Once two have,
        // an update with the same inputs and the stream not marked dirty returns at once and the host keeps the last
        // render list (m_frameProduced false); m_skippedUpdates counts those for the host's statistics
        uint32_t m_quietUpdates = 0, m_skippedUpdates = 0;
        uint64_t m_quietPendingHash = 0; // The requests left pending by the last full update
        StreamSettings m_quietSettings;
        StreamCamera   m_quietCamera;
        bool     m_frameProduced = false;   // The last UpdateStreamingSimulation built a render list (it returns early while streaming is off)
        float    m_refinePacingTraceTimer = 0.0f, m_equalizerUpdateTimer = 0.0f, m_priorityUpdateTimer = 0.0f, m_morphTestDebounceTimer = 0.0f;
        float    m_streamSettleTimer = 0.0f, m_finishBatchTimer = 0.0f, m_frameTraversalMs = 0.0f;
        bool     m_streamReorderPending = false, m_finishBatchPending = false, m_orderEyeValid = false, m_streamStateDirty = true;
        XMFLOAT3 m_orderEye = { 0, 0, 0 }, m_pendingOrderEye = { 0, 0, 0 }, m_lastStreamCamPos = { 1e9f, 1e9f, 1e9f };
        float    m_lastStreamPitch = 1e9f, m_lastStreamYaw = 1e9f;
        XMFLOAT3 m_lastStreamForward = { 0, 0, 0 };   // Explicit view: the direction at the last camera move

    private:
        static void Trace(const char* fmt, ...);
        void CountResidency(const StreamChunk& c, int delta); // One block landed (+1) or left (-1): the level counts follow
        void RecountResidency();                              // The level counts from scratch
    };
}
