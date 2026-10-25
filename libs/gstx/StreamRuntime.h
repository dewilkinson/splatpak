// StreamRuntime.h
// Surfels Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
//
// The streaming runtime: which blocks of a model are resident, at which level, in what order they
// arrive, and how they hand over to one another. A host (SplatLab, the UE5 plugin) hands it the levels
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
#include "../../src/SurfelsCore/WaveletTypes.h"

namespace Surfels
{
    struct StreamChunk
    {
        int      lodLevel = 0;
        size_t   chunkIndex = 0;
        XMFLOAT3 center = { 0, 0, 0 };
        float    radius = 0.0f;
        XMFLOAT3 avgNormal = { 0, 1, 0 };      // Representative surface normal for silhouette edge testing
        float    normalSpread = 0.0f;          // Normal angular variation
        float    detailScore = 0.0f;           // 0..1 from the package's detail grid over this block's bounds (DetailGrid::SampleBox); an input to the streaming order
        uint8_t  octahedronFace = 0;           // Face (0..7) of the bounding octahedron this block was assigned by StreamOrder::Build
        std::vector<SurfelVertex>   rawSurfels;
        std::vector<PackedSurfelGPU> packedSurfels;
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
        bool     refinedBySilhouette = false;  // The refinement into its children was edge-driven (this node sits at or below the target level). When the flag drops and it demotes, the children are kept resident: evicting them only made the face lists re-stream them for the next flag, which flashed the visible faces while spinning
        bool     isSilhouette = false;         // Active in-view silhouette edge chunk (locked against eviction)
        float    transitionProgress = 0.0f;   // 0.0 (Parent Level N Solid) <-> 1.0 (Children Level N-1 Solid)
        float    streamWaveTimer = 0.0f;      // Arrival glow (Refinement Visualizer) countdown, m_chunkStreamDuration -> 0; started by the chunk's first draw after delivery (see streamWavePending)
        bool     streamWavePending = false;   // Delivered but not yet drawn: the arrival glow is armed and starts on the first draw, so a block that waits before it is shown still glows when it appears instead of expiring unseen
        uint32_t deliverySeq = 0;             // Order of delivery by the stream (1 = first delivered this session; 0 = resident at load)
        float    silhouetteHysteresisTimer = 0.0f; // Hysteresis hold time (seconds) to eliminate refinement/demotion thrashing
        bool     renderedLastFrame = false;    // Visited by the previous frame's traversal: rendered, or refined in place of by its children. Only such "active" chunks may keep an edge flag; anything the traversal never reached (a level outside the active range) is cleared before the next traversal. Clearing on "not rendered" alone made a refined edge parent lose its flag, re-render, get re-detected and refine again every other frame -- a whole-model flicker
        uint32_t globalSurfelOffset = 0;      // Zero-copy offset into m_unifiedPackedSurfels / m_unifiedRawSurfels
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


    enum class StreamingPolicy
    {
        Conservative = 0, // The view's own requests cover only visible chunks + a local neighbour buffer, and detail the view drops back from is evicted. The scheduler's face streams run in both policies (see StreamOrder.h)
        Greedy       = 1  // Refines visible chunks first, then continues pre-fetching remaining background chunks
    };


    struct ResidentLOD
    {
        std::vector<SurfelVertex> rawSurfels;
        std::vector<PackedSurfelGPU> packedSurfels;
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
        StreamingPolicy streamingPolicy = StreamingPolicy::Greedy;
        bool            enableStreamingSimulation = true;
        bool            isStreamingPaused = false;
        bool            unthrottledBandwidth = true;
        float           bandwidthThrottleMBps = 10.0f;
        float           ringBufferCapacityMB = 512.0f;
        bool            enableStreamDecay = false;
        float           streamDecayRate = 5.0f;
        bool            demoteChunks = false;
        bool            enableDitheredTransitions = true;
        float           ditherTransitionDurationSec = 0.75f;
        bool            prioritizeFrustumAndProximity = true;
        float           conservativeNeighborBufferMargin = 1.35f;
        bool            enableSilhouetteLOD0 = false;
        bool            highlightSilhouetteChunks = false;
        int             silhouetteLODBias = 2;
        float           silhouetteThreshold = 0.40f;
        float           dilationMorphAmount = 0.25f;
        bool            showChunkStream = true;              // Arrival glow on delivered blocks
        float           chunkStreamDuration = 2.5f;
        bool            splatDrawAsSurfels = false;
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
    };

    class StreamRuntime
    {
    public:
        static constexpr size_t kMaxEdgeQueuePerFace = 8192;
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
        int               m_waveletLevels = 0;               // Number of levels the model has (the LOD rule's range)
        XMFLOAT3          m_aabbMin = { 0, 0, 0 }, m_aabbMax = { 0, 0, 0 }, m_center = { 0, 0, 0 }, m_extents = { 0, 0, 0 };
        XMFLOAT3          m_lodSortFrameMin = { 0, 0, 0 }, m_lodSortFrameMax = { 0, 0, 0 };
        DetailGrid        m_detailGrid;
        bool              m_enableMortonOrder = true;
        bool              m_splatMode = true;
        SplatEncodeParams m_splatParams;
        size_t            m_bytesPerRenderPoint = 20;

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
        float LodSwitchDistance(int level, bool screenSpace) const;
        float EvictableStreamBytes() const;
        void CalibrateDecay();
        float DecayFullDrainSeconds() const;
        void ComputeLodSpacing();
        void HealStaleRequestFlags();
        void ClearFaceEdgeQueues();
        uint32_t FrameChunkCount() const { return m_frameChunkCount; }

        // State (read-only for hosts, see the file comment)
        std::vector<std::vector<StreamChunk>> m_lodStreamChunks;
        std::vector<StreamChunk*>             m_allStreamChunkPtrs;
        std::vector<size_t>                   m_lodTotalSurfels, m_lodResidentSurfels;
        std::vector<float>                    m_lodSpacing;
        std::vector<float>                    m_smoothedLodResidentPct;
        std::vector<uint32_t>                 m_smoothedLodResidentBlocks;
        std::vector<MeshletChunkGPU>          m_rendererMeshletChunks;
        std::vector<StreamChunk*>             m_rendererSourceChunks;
        std::vector<PackedSurfelGPU>          m_unifiedPackedSurfels;
        std::vector<SurfelVertex>             m_unifiedRawSurfels;
        std::vector<PackedSplatGPU>           m_unifiedSplats;
        std::vector<uint8_t>                  m_unifiedSH;
        StreamOrder               m_streamOrder;
        std::vector<ChunkRequest> m_demandRequestQueue;
        size_t                    m_demandRequestHead = 0;
        std::vector<ChunkRequest> m_faceEdgeQueue[kOctahedronFaces];
        size_t                    m_faceEdgeHead[kOctahedronFaces] = {};
        std::vector<size_t>       m_faceDrainBuckets[kOctahedronFaces];
        std::vector<uint32_t>     m_scratchLoadHandles;
        std::vector<StreamChunk*> m_scratchLoadList;
        std::vector<StreamChunk*> m_finishBatch;
        std::vector<std::pair<uint32_t, uint32_t>> m_refineDeferred;
        float    m_simulatedBytesDelivered = 0.0f, m_totalStreamBytes = 0.0f, m_streamRefinementProgress = 1.0f;
        float    m_decayBytesPerSec = 0.0f, m_decayReferenceBandwidthBps = 0.0f;
        size_t   m_evictedSurfelCount = 0;
        uint32_t m_deliverySeqCounter = 0, m_residencyEpoch = 0, m_streamFrame = 0;
        uint32_t m_redeliveredBlocks = 0, m_extraDeliveries = 0;
        uint32_t m_lastActiveTransitions = 0, m_edgeRefineStartsThisFrame = 0;
        uint32_t m_refineStartsThisFrame = 0, m_refineStartBudget = 0, m_refineStartsDeferred = 0, m_refineAdmitSeqLimit = 0;
        uint32_t m_frameChunkCount = 0;
        bool     m_frameProduced = false;   // The last UpdateStreamingSimulation built a render list (it returns early while streaming is off)
        float    m_refinePacingTraceTimer = 0.0f, m_equalizerUpdateTimer = 0.0f, m_priorityUpdateTimer = 0.0f, m_morphTestDebounceTimer = 0.0f;
        float    m_streamSettleTimer = 0.0f, m_finishBatchTimer = 0.0f, m_frameTraversalMs = 0.0f;
        bool     m_streamReorderPending = false, m_finishBatchPending = false, m_orderEyeValid = false, m_streamStateDirty = true;
        XMFLOAT3 m_orderEye = { 0, 0, 0 }, m_pendingOrderEye = { 0, 0, 0 }, m_lastStreamCamPos = { 1e9f, 1e9f, 1e9f };
        float    m_lastStreamPitch = 1e9f, m_lastStreamYaw = 1e9f;

    private:
        static void Trace(const char* fmt, ...);
    };
}
