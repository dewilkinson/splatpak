// SurfelsRenderer.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// GPU-side renderer shared by SplatLab and the standalone viewer: owns every root
// signature, PSO, and GPU buffer, and exposes a single State snapshot + OnRender()
// entry point that SurfelsApp drives once per frame. See SurfelsRenderer.cpp for the
// implementation.

#pragma once
#include "stdafx.h"
#include "base/Texture.h"
#include "WaveletTypes.h"

#include "PostProc/PostProcCS.h"

namespace Surfels
{
    void LogTransitionTrace(const char* fmt, ...);

    class SurfelsRenderer
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

            uint32_t renderMode  = 1; // 0 = Sphere, 1 = Quantized (8-byte), 2 = Raw Float32, 3 = Splat mode (20-byte Gaussians, see pSplats)
            uint32_t orientMode  = 0; // 0 = Normal-Oriented Tangent Discs (standard default; 1 = Camera-Facing Billboards)
            XMFLOAT3 aabbMin     = { -40.0f, -2.0f, -80.0f };
            XMFLOAT3 aabbExtents = { 80.0f, 30.0f, 160.0f };

            const PackedSurfelGPU* pSurfels    = nullptr;
            const SurfelVertex*    pRawSurfels = nullptr;
            // Splat mode (renderMode 3): the 20-byte Gaussian records and their spherical-harmonics stream
            // (surfelCount * shRecordBytes bytes, may be null), plus the decode parameters from the package.
            const PackedSplatGPU*  pSplats     = nullptr;
            const uint8_t*         pSplatSH    = nullptr;
            uint32_t shDegree          = 0;
            uint32_t shRecordBytes     = 0;
            float    splatScaleLog2Min = -14.0f;
            float    splatScaleLog2Max = 2.0f;
            uint32_t splatBlendSpace   = 0; // 0 = display space (as the reference viewer), 1 = linear (sRGB target)
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
            bool     showChunkStream  = true;  // Refinement visualizer: tint newly streamed chunks (arrival glow)
            float    arrivalGlowIntensity = 0.4f; // Strength of that tint and bloom (0 = invisible, 0.4 = default, 2 = strong)
            float    arrivalGlowHue = 0.0f;       // Hue rotation in degrees applied to the glow colours (0 = orange)
            bool     autoSplatSize = true;        // Disc radius from each level's point spacing (lodRadius) instead of the size classes
            float    lodRadius[8] = {};           // Auto splat size: disc radius per LOD level, world units (0 = unknown level)
            float    splatDiscRadius[8] = {};     // Splat mode: camera-facing disc radius per level for level 2 and up, world units (0 = draw Gaussians)
            float    splatDiscOpacityFloor = 0.0f; // Splat mode: opacity floor for the discs of level 1 and up (Match Level 0 Softness; 0 = the record's opacity as is)
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

        // Render path, decided in OnCreate from the GPU's capabilities: mesh shaders where the hardware
        // has D3D12 Mesh Shader Tier 1, otherwise the instanced vertex-shader fallback (Surfels.hlsl,
        // mainVS/itemVS/occluderVS) compiled for Shader Model 6.0, or through the legacy compiler for
        // Shader Model 5.1 when the driver has no DXIL support. SetRenderPathOverride (config
        // "render_path") forces a fallback on capable hardware for testing; a path the hardware cannot
        // run is never forced.
        enum class RenderPath { MeshShaders = 0, VertexShadersSM6 = 1, VertexShadersSM5 = 2 };
        struct GpuCapabilities
        {
            bool     meshShaders = false;        // D3D12 Mesh Shader Tier 1: amplification + mesh stages
            bool     shaderModel6 = false;       // DXIL, Shader Model 6.0 or later
            uint32_t highestShaderModel = 0;     // D3D_SHADER_MODEL value, e.g. 0x65 = 6.5, 0x51 = 5.1
            bool     meshPipelineFailed = false; // Mesh shaders were reported but their pipeline states could not be created
        };
        void SetRenderPathOverride(int path) { m_renderPathOverride = path; } // Before OnCreate: -1 = auto, else a RenderPath value
        RenderPath GetRenderPath() const { return m_renderPath; }
        bool IsRenderPathForced() const { return m_renderPathForced; }
        const GpuCapabilities& GetGpuCapabilities() const { return m_gpuCaps; }
        const char* GetRenderPathDescription() const;

        // Product name used in the renderer's own dialogs (mesh-shader fallback notice, pipeline
        // failures). Set by SurfelsApp before OnCreate; defaults to "SplatLab".
        static void SetAppTitle(const char* title);
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
        float GetLastGpuWaitMs() const { return m_lastGpuWaitMs; }                 // Raw, this frame (the app windows these itself)
        float GetLastCommandRecordMs() const { return m_lastCommandRecordMs; }
        float GetSmoothGpuWaitMs() const { return m_smoothGpuWaitMs; }             // CPU blocked in WaitForSwapChain (the GPU, or VSync)
        float GetSmoothCommandRecordMs() const { return m_smoothCommandRecordMs; } // From that wait to ExecuteCommandLists: uploads, sort, every pass
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
            float      arrivalGlowIntensity;  // Refinement visualizer strength (see State)
            float      arrivalGlowHue;        // Refinement visualizer hue rotation, degrees
            uint32_t   autoSplatSize;         // Auto splat size on/off (see State)
            uint32_t   culledPass;            // Detach Camera: 0 = draw the splats the frozen camera sees, 1 = draw only the ones it culled (the red volume)
            float      autoSplatPad1[2];
            float      lodRadiusAlign[2];     // Pads to the next 16-byte row: the shader's float4 g_LodRadius[2] starts on one, so without this every field from here on was read 8 bytes off
            float      lodRadius[8];          // Disc radius per LOD level, world units (two float4 rows in the shader)
            // Splat mode
            XMFLOAT4X4 view;
            float      viewportW;
            float      viewportH;
            float      proj00;
            float      splatScaleLog2Min;
            float      splatScaleLog2Max;
            uint32_t   shDegree;
            uint32_t   shRecordBytes;
            uint32_t   splatBlendSpace;
            XMFLOAT3   camForward;
            uint32_t   splatSlotCount;
            float      splatDiscRadius[8];    // Splat mode: camera-facing disc radius per level for level 2 and up, world units (two float4 rows in the shader; splatSlotCount ends a 16-byte row, so no padding)
            float      splatDiscOpacityFloor; // Splat mode: opacity floor for the discs of level 1 and up (see State)
            float      splatDiscPad[3];       // Pads to a whole 16-byte row (float3 g_SplatDiscPad in the shader)
        };
        static_assert(offsetof(SurfelsCB, splatDiscRadius) % 16 == 0, "SurfelsCB::splatDiscRadius must start a 16-byte row: the shader reads it as float4 g_SplatDiscRadius[2]");
        static_assert(sizeof(SurfelsCB) % 16 == 0, "SurfelsCB must be a whole number of 16-byte rows");

        CAULDRON_DX12::Device* m_pDevice = nullptr;

        CAULDRON_DX12::ResourceViewHeaps m_resourceViewHeaps;
        CAULDRON_DX12::UploadHeap        m_uploadHeap;
        CAULDRON_DX12::DynamicBufferRing m_constantBufferRing;
        CAULDRON_DX12::CommandListRing   m_commandListRing;
        CAULDRON_DX12::ImGUI             m_imGui;

        ID3D12RootSignature* m_pRootSignature = nullptr;
        ID3D12PipelineState* m_pPipelineState = nullptr;
        ID3D12PipelineState* m_pPipelineStateOcclusionTest = nullptr; // Same as m_pPipelineState but with depth-test-only (no write) enabled, used when occlusion culling is active
        // Detach Camera's three-pass sequence (all against the viewer's depth buffer): the splats the frozen
        // camera sees are drawn as usual but also record depth; the culled splats then run a depth-only
        // prepass so only their nearest layer survives, and that layer alone is blended at 10% with an
        // EQUAL depth test. Stacked culled splats therefore cannot add up, and red behind the visible shell
        // is rejected. Same shaders as m_pPipelineState; only depth/blend state differs.
        ID3D12PipelineState* m_pPipelineStateDetachedMain = nullptr; // Blend, depth LESS_EQUAL test + write
        ID3D12PipelineState* m_pPipelineStateCulledDepth = nullptr;  // No colour writes, depth LESS test + write
        ID3D12PipelineState* m_pPipelineStateCulledColor = nullptr;  // Blend, depth EQUAL test, no write
        ID3D12PipelineState* m_pOccluderPSO = nullptr; // Solid depth-writing interior occlusion volume cubes (occluderMS/occluderPS)
        // Splat mode: same shaders (mainAS/MS/PS branch on g_RenderMode == 3), premultiplied blend, no depth. Two
        // render-target formats: a plain UNORM view for display-space blending (the reference viewer's
        // behaviour), the swapchain's sRGB view for linear-space blending.
        ID3D12PipelineState* m_pSplatPSODisplay = nullptr;
        ID3D12PipelineState* m_pSplatPSOLinear = nullptr;
        ID3D12PipelineState* m_pSplatPSODisplayDepth = nullptr; // Same, depth-tested (no write) against the occlusion volume
        ID3D12PipelineState* m_pSplatPSOLinearDepth = nullptr;
        ID3D12PipelineState* m_pOccluderPSOUnorm = nullptr;     // The occluder on the UNORM target of display-space splat blending
        DXGI_FORMAT          m_unormFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT          m_srgbFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

        RenderPath      m_renderPath = RenderPath::MeshShaders;
        int             m_renderPathOverride = -1;
        bool            m_renderPathForced = false;
        GpuCapabilities m_gpuCaps;
        void CreateVertexShaderPipelines(CAULDRON_DX12::SwapChain* pSwapChain); // The fallback's graphics PSOs (main, occlusion-test, item prepass, occluder)

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

        // Splat mode buffers: the records, the SH stream, and the per-frame depth sort (SplatSortCS.hlsl).
        ID3D12Resource*            m_pSplatBuffer = nullptr;       // Upload
        ID3D12Resource*            m_pSplatGpuBuffer = nullptr;
        uint8_t*                   m_pSplatBufferMapped = nullptr;
        uint32_t                   m_splatBufferCapacityBytes = 0;
        ID3D12Resource*            m_pSHBuffer = nullptr;          // Upload
        ID3D12Resource*            m_pSHGpuBuffer = nullptr;
        uint8_t*                   m_pSHBufferMapped = nullptr;
        uint32_t                   m_shBufferCapacityBytes = 0;
        const void*                m_lastSplatSHPtr = nullptr;
        ID3D12Resource*            m_pChunkSlotBaseUpload = nullptr; // First slot of every chunk (exclusive prefix of surfelCount)
        uint32_t*                  m_pChunkSlotBaseMapped = nullptr;
        ID3D12Resource*            m_pChunkSlotBaseGpu = nullptr;
        D3D12_RESOURCE_STATES      m_chunkSlotBaseState = D3D12_RESOURCE_STATE_COPY_DEST;
        uint32_t                   m_splatSlotCount = 0;             // Sum of the render list's surfel counts
        bool                       m_splatSortNeedsRun = true;
        ID3D12RootSignature*       m_pSplatSortRootSig = nullptr;
        ID3D12PipelineState*       m_pSplatExpandPSO = nullptr;
        ID3D12PipelineState*       m_pSplatHistPSO = nullptr;
        ID3D12PipelineState*       m_pSplatScanPSO = nullptr;
        ID3D12PipelineState*       m_pSplatScatterPSO = nullptr;
        ID3D12Resource*            m_pSlotInfoBuffer = nullptr;      // uint2 per padded slot
        ID3D12Resource*            m_pSplatPairsA = nullptr;         // uint2 (key, slot) per padded slot; the sorted result after four passes
        ID3D12Resource*            m_pSplatPairsB = nullptr;
        ID3D12Resource*            m_pSplatHistBuffer = nullptr;     // 256 x blocks
        uint32_t                   m_splatSortCapacitySlots = 0;
        D3D12_RESOURCE_STATES      m_slotInfoState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES      m_splatPairsAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12DescriptorHeap*      m_pBackBufferUnormRtvHeap = nullptr; // One UNORM view of the current back buffer (display-space blending without TAA)
        void EnsureSplatSortBuffers(uint32_t paddedSlots);
        void RunSplatSort(ID3D12GraphicsCommandList* pCmdLst, const State* pState, XMFLOAT3 eyePos, XMFLOAT3 forward, const XMFLOAT4X4& cullViewProj);

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
        float                      m_smoothGpuWaitMs = 0.0f;
        float                      m_lastGpuWaitMs = 0.0f;
        float                      m_lastCommandRecordMs = 0.0f;
        float                      m_smoothCommandRecordMs = 0.0f;
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
        CAULDRON_DX12::RTV         m_sceneColorRTVUnorm; // Same texture, plain UNORM view: splat mode's display-space blending target
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

