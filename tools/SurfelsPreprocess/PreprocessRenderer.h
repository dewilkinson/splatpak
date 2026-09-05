// PreprocessRenderer.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// GPU-side renderer for SplatLab: owns every root signature, PSO, and GPU
// buffer, and exposes a single State snapshot + OnRender() entry point that
// PreprocessApp drives once per frame. See PreprocessRenderer.cpp for the implementation.

#pragma once
#include "../../src/DX12/stdafx.h"
#include "base/Texture.h"
#include "../../src/DX12/Wavelet/WaveletTypes.h"

#include "PostProc/PostProcCS.h"

namespace Surfels
{
    void LogTransitionTrace(const char* fmt, ...);

    class PreprocessRenderer
    {
    public:
        struct State
        {
            float    splatRadius = 1.0f;
            float    camYaw      = 0.6f;
            float    camPitch    = 0.35f;
            float    camDistance = 15.0f;
            XMFLOAT3 camTarget   = { 0.0f, 0.0f, 0.0f };
            float    aspectRatio = 16.0f / 9.0f;
            bool     autoRotate  = false;
            float    time        = 0.0f;

            uint32_t renderMode  = 1; // 0 = Sphere, 1 = Quantized (8-byte), 2 = Raw Float32
            uint32_t orientMode  = 0; // 0 = Normal-Oriented Tangent Discs (standard default; 1 = Camera-Facing Billboards)
            XMFLOAT3 aabbMin     = { -40.0f, -2.0f, -80.0f };
            XMFLOAT3 aabbExtents = { 80.0f, 30.0f, 160.0f };

            const PackedSurfelGPU* pSurfels    = nullptr;
            const SurfelVertex*    pRawSurfels = nullptr;
            const MeshletChunkGPU* pChunks     = nullptr;
            uint32_t surfelCount      = 0;
            uint32_t chunkCount       = 0;
            uint32_t totalDatasetSurfels = 0; // Base unculled model point count
            uint32_t totalDatasetChunks  = 0; // Base total chunk count
            bool     gpuRadixSort     = true;
            bool     useChunkedPipeline = true;
            bool     detachCullCamera = false;
            float    cullYaw          = 0.6f;
            float    cullPitch        = 0.35f;
            float    cullDistance     = 15.0f;
            XMFLOAT3 cullTarget       = { 0.0f, 0.0f, 0.0f };
            bool     enableDithering  = true; // Stochastic screen-space Bayer dithering for smooth LOD transitions
            bool     highlightSilhouette = false; // Highlight silhouette chunks in lavender semi-transparent effect
            bool     showChunkStream  = true;  // Render orange wave sweep for newly streamed chunks
            bool     enableConeCulling = true; // Task Shader (mainAS) backface normal cone culling
            bool     useCopyQueue = true; // Dedicated DX12 Hardware DMA Copy Queue for asynchronous PCIe transfers
            bool     enableGpuSilhouetteInversion = true; // GPU Chunk-ID & Depth Discontinuity Edge Inversion
            float    silhouetteDepthThreshold = 0.05f; // Depth step threshold for interior occlusion edges
            bool     silhouetteExteriorOnly   = true;  // 1 = only outer perimeter against background, 0 = include interior occlusion
            bool     showOnlyLockedChunks = false; // Isolate and show ONLY locked chunks (transition or edge)
            bool     enableTemporalFiltering = true; // High-performance Temporal Accumulation & Dither Resolver (TAA)
            float    temporalBlendWeight      = 0.15f; // History blend weight (0.05 = maximum smoothness, 0.50 = responsive)
            bool     enableSubpixelJitter     = true;  // 8-phase Halton(2,3) sub-pixel camera jitter
            bool     enableVarianceClamping   = true;  // 3x3 YCoCg neighborhood variance color box clamping (anti-ghosting)

            // Interior Occlusion Volume: solid depth-writing cubes baked at preprocessing time so far-side
            // surfels don't show through gaps in the near side. Disabled by default.
            const OcclusionVoxelGPU* pOcclusionVoxels  = nullptr;
            uint32_t                 occlusionVoxelCount = 0;
            uint32_t                 occlusionVoxelVersion = 0; // Bumped on every rebuild so a same-size rebuild (e.g. colour-only) still re-uploads
            bool                     enableOcclusionCulling  = false; // Disabled by default
            bool                     showOcclusionVolumeOnly = false; // Debug view: render only the occluder geometry
            OcclusionMipTable        occlusionMips;                   // Mip layout of pOcclusionVoxels (see OcclusionMipTable); mipCount 0 = the array is one mip
            int32_t                  occlusionMipOverride = -1;       // -1 = pick the mip automatically from projected cell size; 0..mipCount-1 = force that mip (debug/compare)
            float                    occlusionMipMinCellPixels = 2.5f; // Auto rule: finest mip whose cell still covers at least this many pixels at the model's nearest point
        };

        struct FrameTimingMetrics
        {
            float frameRate = 0.0f;
            float totalFrameTimeMs = 0.0f;
            float cpuSortTimeMs = 0.0f;
            float gpuSortTimeMs = 0.0f;
            float gpuDispatchTimeMs = 0.0f; // Whole draw-submission section (uploads + sort + prepass + occluder + main dispatch combined)
            float uploadTimeMs = 0.0f;      // Surfel/chunk buffer upload submission (copy queue or direct queue fallback)
            float silhouettePrepassTimeMs = 0.0f; // Item ID/depth prepass + GPU edge-extraction compute (only re-runs when the edge cache invalidates)
            float occluderPassTimeMs = 0.0f;      // Interior occlusion volume draw (0 when disabled/no volume loaded)
            float mainDispatchTimeMs = 0.0f;      // Final splat mesh shader dispatch only
            float taaResolveTimeMs = 0.0f;        // Temporal filter compute dispatch + resolve blit (0 when TAA disabled)
            float uiDrawTimeMs = 0.0f;
            bool  wasSortedThisFrame = false;
            bool  isGPUSortActive = true;
        };

        void OnCreate(CAULDRON_DX12::Device* pDevice, CAULDRON_DX12::SwapChain* pSwapChain);
        void OnDestroy();

        void OnCreateWindowSizeDependentResources(CAULDRON_DX12::SwapChain* pSwapChain, uint32_t width, uint32_t height);
        void OnDestroyWindowSizeDependentResources();
        void OnUpdateDisplayDependentResources(CAULDRON_DX12::SwapChain* pSwapChain);

        void OnRender(State* pState, CAULDRON_DX12::SwapChain* pSwapChain);

        void FlushGPU() { if (m_pDevice) m_pDevice->GPUFlush(); }

        const FrameTimingMetrics& GetTimingMetrics() const { return m_metrics; }
        const GeometryCullStats&  GetCullStats() const { return m_cullStats; }
        float GetSmoothGpuSortMs() const { return m_smoothGpuSortMs; }
        float GetSmoothDispatchMs() const { return m_smoothDispatchMs; }
        float GetSmoothUiMs() const { return m_smoothUiMs; }
        float GetSmoothUploadMs() const { return m_smoothUploadMs; }
        float GetSmoothSilhouettePrepassMs() const { return m_smoothSilhouettePrepassMs; }
        float GetSmoothOccluderMs() const { return m_smoothOccluderMs; }
        uint32_t GetActiveOcclusionMip() const { return m_activeOcclusionMip; }             // Mip of the occlusion volume drawn last frame (see SelectOcclusionMip)
        float    GetOcclusionMipCellPixels() const { return m_activeOcclusionMipCellPixels; } // Screen-space size of that mip's cell at the model's nearest point
        float GetSmoothMainDispatchMs() const { return m_smoothMainDispatchMs; }
        float GetSmoothTaaMs() const { return m_smoothTaaMs; }
        const std::vector<uint32_t>& GetSilhouetteBitmask() const { return m_silhouetteBitmaskCPU; }
        uint32_t GetSilhouetteBitmaskChunkCount() const { return m_lastEdgeChunkCount; }


    private:
        // Blocks until any in-flight m_pCopyQueue work has completed. m_pCopyQueue is a raw D3D12 queue
        // created directly by this class -- it is NOT one of Cauldron's own tracked queues, so
        // Device::GPUFlush() (direct/compute only) never waits on it. Buffer-resize code paths that
        // Unmap()/Release() an upload buffer the copy queue reads from (via CopyResource) must call this
        // first, or a copy queued on a prior frame can still be reading from that memory the moment it is
        // freed -- a GPU-side use-after-free that can corrupt state or hang the device (DXGI_ERROR_DEVICE_HUNG)
        // with no CPU-visible symptom.
        void FlushCopyQueue();

        struct SurfelsCB
        {
            XMFLOAT4X4 viewProj;
            XMFLOAT3   camRight;
            float      radius;
            XMFLOAT3   camUp;
            float      time;
            XMFLOAT3   viewerEyePos;
            float      sphereRadius;
            uint32_t   surfelCount;
            uint32_t   renderMode;
            uint32_t   orientMode;
            uint32_t   totalChunks;
            XMFLOAT3   aabbMin;
            uint32_t   useChunkedPipeline;
            XMFLOAT3   aabbExtents;
            uint32_t   useDetachedCullCam;
            XMFLOAT4X4 cullViewProj;
            XMFLOAT3   cullEyePos;
            uint32_t   enableDithering;
            uint32_t   highlightSilhouette;
            uint32_t   enableConeCulling;
            uint32_t   showOnlyLocked;
            uint32_t   showChunkStream;          // Matches a pre-existing HLSL-only g_ShowChunkStream cbuffer
                                                   // field that was never previously mirrored here -- keeping
                                                   // it in this exact slot keeps everything below it correctly
                                                   // byte-aligned with the shader's cbuffer layout.
            uint32_t   enableOcclusionCulling;
            uint32_t   showOcclusionVolumeOnly;
            uint32_t   occlusionVoxelCount;   // Blocks in the occlusion volume mip drawn this frame
            uint32_t   occlusionVoxelFirst;   // Index of that mip's first block in the voxel buffer
        };

        CAULDRON_DX12::Device* m_pDevice = nullptr;

        CAULDRON_DX12::ResourceViewHeaps m_resourceViewHeaps;
        CAULDRON_DX12::UploadHeap        m_uploadHeap;
        CAULDRON_DX12::DynamicBufferRing m_constantBufferRing;
        CAULDRON_DX12::CommandListRing   m_commandListRing;
        CAULDRON_DX12::ImGUI             m_imGui;

        ID3D12RootSignature* m_pRootSignature = nullptr;
        ID3D12PipelineState* m_pPipelineState = nullptr;
        ID3D12PipelineState* m_pPipelineStateOcclusionTest = nullptr; // Same as m_pPipelineState but with depth-test-only (no write) enabled, used when occlusion culling is active
        ID3D12PipelineState* m_pOccluderPSO = nullptr; // Solid depth-writing interior occlusion volume cubes (occluderMS/occluderPS)

        void UpdateOcclusionVoxelBuffer(const State* pState);
        uint32_t SelectOcclusionMip(const State* pState, const OcclusionMipTable& mips, const XMFLOAT3& eyePos);
        uint32_t m_activeOcclusionMip = 0;             // Mip chosen last frame; the hysteresis in SelectOcclusionMip works from it
        float    m_activeOcclusionMipCellPixels = 0.0f;
        ID3D12Resource*      m_pOcclusionVoxelBuffer = nullptr; // Upload-heap resource read directly as an SRV -- small & infrequently updated, doesn't need the default-heap/copy-queue machinery used for chunks/surfels
        uint8_t*              m_pOcclusionVoxelBufferMapped = nullptr;
        uint32_t               m_occlusionVoxelBufferCapacityBytes = 0;
        const void*             m_lastOcclusionVoxelsPtr = nullptr;
        uint32_t                m_lastOcclusionVoxelCount = 0;
        uint32_t                m_lastOcclusionVoxelVersion = 0;

        uint32_t m_width  = 0;
        uint32_t m_height = 0;

        void UpdateSurfelBuffers(const State* pState, XMFLOAT3 eyePos, XMFLOAT3 forward);

        ID3D12Resource*            m_pSurfelBuffer = nullptr;
        ID3D12Resource*            m_pSurfelGpuBuffer = nullptr;
        uint8_t*                   m_pSurfelBufferMapped = nullptr;
        uint32_t                   m_surfelBufferCapacityBytes = 0;
        D3D12_GPU_VIRTUAL_ADDRESS  m_surfelBufferGPUAddress = 0;

        ID3D12Resource*            m_pRawSurfelBuffer = nullptr;
        ID3D12Resource*            m_pRawSurfelGpuBuffer = nullptr;
        uint8_t*                   m_pRawSurfelBufferMapped = nullptr;
        uint32_t                   m_rawSurfelBufferCapacityBytes = 0;
        D3D12_GPU_VIRTUAL_ADDRESS  m_rawSurfelBufferGPUAddress = 0;

        CAULDRON_DX12::Texture     m_depthBuffer;
        CAULDRON_DX12::DSV         m_depthBufferDSV;

        const void*                m_lastSurfelsPtr = nullptr;
        uint32_t                   m_lastSurfelCount = 0;
        uint32_t                   m_lastRenderMode = 0;

        XMFLOAT3                   m_lastSortEye = { 0, 0, 0 };
        XMFLOAT3                   m_lastSortForward = { 0, 0, 0 };
        std::vector<uint32_t>      m_sortIndicesA;
        std::vector<uint32_t>      m_sortIndicesB;
        std::vector<uint32_t>      m_sortKeys;
        std::vector<float>         m_sortDists;
        FrameTimingMetrics         m_metrics;
        GeometryCullStats          m_cullStats;

        float                      m_fpsAccumTimeMs = 0.0f;
        uint32_t                   m_fpsAccumFrames = 0;
        float                      m_smoothGpuSortMs = 0.0f;
        float                      m_smoothDispatchMs = 0.0f;
        float                      m_smoothUiMs = 0.0f;
        float                      m_smoothUploadMs = 0.0f;
        float                      m_smoothSilhouettePrepassMs = 0.0f;
        float                      m_smoothOccluderMs = 0.0f;
        float                      m_smoothMainDispatchMs = 0.0f;
        float                      m_smoothTaaMs = 0.0f;
        std::chrono::high_resolution_clock::time_point m_lastWallClockTime;

        ID3D12Resource*            m_pSurfelGpuOutBuffer = nullptr;
        ID3D12Resource*            m_pRawSurfelGpuOutBuffer = nullptr;
        ID3D12Resource*            m_pGPUSortPairBuffer = nullptr;
        uint32_t                   m_sortPairBufferCapacityBytes = 0;
        bool                       m_needUploadToGpu = false;
        bool                       m_needUploadChunksToGpu = false;
        bool                       m_gpuSortNeedsRun = true;

        std::vector<MeshletChunkGPU> m_meshletChunks;
        ID3D12Resource*            m_pChunkUploadBuffer = nullptr;
        uint8_t*                   m_pChunkUploadBufferMapped = nullptr;
        ID3D12Resource*            m_pChunkGpuBuffer = nullptr;
        D3D12_RESOURCE_STATES      m_chunkGpuBufferState = D3D12_RESOURCE_STATE_COPY_DEST;
        ID3D12Resource*            m_pSortedChunkIndicesGpuBuffer = nullptr;
        D3D12_RESOURCE_STATES      m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_COPY_DEST;
        ID3D12Resource*            m_pSortedChunkIndicesUploadBuffer = nullptr;
        uint32_t*                  m_pSortedChunkIndicesUploadBufferMapped = nullptr;
        uint32_t                   m_chunkBufferCapacityBytes = 0;
        uint32_t                   m_sortedChunkIndicesCapacityBytes = 0;
        bool                       m_lastGpuRadixSort = false;
        bool                       m_lastUseChunkedPipeline = false;
        bool                       m_needUploadChunkIndicesToGpu = false;
        std::vector<std::pair<float, uint32_t>> m_chunkDists;

        // GPU-Driven Pipeline & Bitonic LDS Sorting PSOs
        ID3D12CommandSignature*    m_pCommandSignature = nullptr;
        ID3D12RootSignature*       m_pComputeRootSignature = nullptr;
        ID3D12PipelineState*       m_pProjectKeysPSO = nullptr;
        ID3D12PipelineState*       m_pProjectChunkKeysPSO = nullptr;
        ID3D12PipelineState*       m_pBitonicLocalSortPSO = nullptr;
        ID3D12PipelineState*       m_pBitonicGlobalSortPSO = nullptr;
        ID3D12PipelineState*       m_pBitonicLocalMergePSO = nullptr;
        ID3D12PipelineState*       m_pGatherSurfelsPSO = nullptr;
        ID3D12PipelineState*       m_pGatherChunkIndicesPSO = nullptr;
        ID3D12PipelineState*       m_pCullPSO = nullptr;
        ID3D12PipelineState*       m_pRadixSortPSO = nullptr;
        ID3D12PipelineState*       m_pBuildArgsPSO = nullptr;

        ID3D12Resource*            m_pGPUAllSurfelsBuffer = nullptr;
        ID3D12Resource*            m_pGPUVisibleIndexBuffer = nullptr;
        ID3D12Resource*            m_pGPUSortedIndexBuffer = nullptr;
        ID3D12Resource*            m_pGPUSortKeyBuffer = nullptr;
        ID3D12Resource*            m_pGPUIndirectArgsBuffer = nullptr;
        ID3D12Resource*            m_pGPUCounterBuffer = nullptr;
        bool                       m_enableGPUPipeline = true;

        // Dedicated Hardware DMA Copy Queue
        ID3D12CommandQueue*        m_pCopyQueue = nullptr;
        ID3D12CommandAllocator*    m_pCopyAllocator = nullptr;
        ID3D12GraphicsCommandList* m_pCopyCmdList = nullptr;
        ID3D12Fence*               m_pCopyFence = nullptr;
        uint64_t                   m_copyFenceValue = 0;
        HANDLE                     m_copyFenceEvent = nullptr;

        // GPU Chunk-ID & Depth Discontinuity Edge Inversion
        CAULDRON_DX12::Texture     m_itemBuffer;
        CAULDRON_DX12::Texture     m_itemDepthBuffer;
        CAULDRON_DX12::RTV         m_itemRTV;
        CAULDRON_DX12::DSV         m_itemDepthDSV;
        CAULDRON_DX12::CBV_SRV_UAV m_itemSRV;
        CAULDRON_DX12::CBV_SRV_UAV m_itemDepthSRV;
        CAULDRON_DX12::CBV_SRV_UAV m_itemTableSRVs;
        CAULDRON_DX12::CBV_SRV_UAV m_silhouetteBitmaskUAV;
        CAULDRON_DX12::PostProcCS  m_clearBitmaskCS;
        CAULDRON_DX12::PostProcCS  m_silhouetteEdgeExtractCS;
        ID3D12Resource*            m_pSilhouetteBitmaskGpuBuffer = nullptr;
        ID3D12Resource*            m_pSilhouetteReadbackBuffer = nullptr;
        ID3D12PipelineState*       m_pItemPrepassPSO = nullptr;
        std::vector<uint32_t>      m_silhouetteBitmaskCPU;
        uint32_t                   m_itemWidth = 0;
        uint32_t                   m_itemHeight = 0;
        uint32_t                   m_bitmaskCapacityBytes = 0;
        XMFLOAT4X4                 m_lastEdgeViewProj = {};
        uint32_t                   m_lastEdgeChunkCount = 0;
        float                      m_lastEdgeDepthThreshold = -1.0f;
        bool                       m_lastEdgeExteriorOnly = true;
        bool                       m_edgeBitmaskValid = false;

        // Temporal Anti-Aliasing & Dither Transition Resolver
        CAULDRON_DX12::Texture     m_sceneColorBuffer;
        CAULDRON_DX12::RTV         m_sceneColorRTV;
        CAULDRON_DX12::CBV_SRV_UAV m_sceneColorSRV;

        CAULDRON_DX12::Texture     m_historyColorBuffer;
        CAULDRON_DX12::CBV_SRV_UAV m_historyColorSRV;
        CAULDRON_DX12::CBV_SRV_UAV m_historyColorUAV;

        CAULDRON_DX12::Texture     m_resolvedColorBuffer;
        CAULDRON_DX12::CBV_SRV_UAV m_resolvedColorSRV;
        CAULDRON_DX12::CBV_SRV_UAV m_resolvedColorUAV;

        CAULDRON_DX12::CBV_SRV_UAV m_depthBufferSRV;
        CAULDRON_DX12::CBV_SRV_UAV m_temporalTableSRVs;
        CAULDRON_DX12::CBV_SRV_UAV m_temporalTableUAVs;

        ID3D12RootSignature*       m_pTemporalRootSig = nullptr;
        ID3D12PipelineState*       m_pTemporalPSO = nullptr;

        XMFLOAT4X4                 m_prevViewProj = {};
        bool                       m_temporalFirstFrame = true;
        uint32_t                   m_jitterPhase = 0;
    };
}

