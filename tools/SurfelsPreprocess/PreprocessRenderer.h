#pragma once
#include "../../src/DX12/stdafx.h"
#include "base/Texture.h"
#include "../../src/DX12/Wavelet/WaveletTypes.h"

namespace Surfels
{
    class PreprocessRenderer
    {
    public:
        struct State
        {
            float    splatRadius = 0.05f;
            float    camYaw      = 0.6f;
            float    camPitch    = 0.35f;
            float    camDistance = 15.0f;
            XMFLOAT3 camTarget   = { 0.0f, 0.0f, 0.0f };
            float    aspectRatio = 16.0f / 9.0f;
            bool     autoRotate  = false;
            float    time        = 0.0f;

            uint32_t renderMode  = 1; // 0 = Sphere, 1 = Quantized (8-byte), 2 = Raw Float32
            uint32_t orientMode  = 1; // 1 = Camera-Facing Billboards (standard)
            XMFLOAT3 aabbMin     = { -40.0f, -2.0f, -80.0f };
            XMFLOAT3 aabbExtents = { 80.0f, 30.0f, 160.0f };

            const PackedSurfelGPU* pSurfels    = nullptr;
            const SurfelVertex*    pRawSurfels = nullptr;
            const MeshletChunkGPU* pChunks     = nullptr;
            uint32_t surfelCount      = 0;
            uint32_t chunkCount       = 0;
            bool     gpuRadixSort     = true;
            bool     useChunkedPipeline = true;
            bool     detachCullCamera = false;
            float    cullYaw          = 0.6f;
            float    cullPitch        = 0.35f;
            float    cullDistance     = 15.0f;
            XMFLOAT3 cullTarget       = { 0.0f, 0.0f, 0.0f };
        };

        struct FrameTimingMetrics
        {
            float frameRate = 0.0f;
            float totalFrameTimeMs = 0.0f;
            float cpuSortTimeMs = 0.0f;
            float gpuSortTimeMs = 0.0f;
            float gpuDispatchTimeMs = 0.0f;
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
        float GetSmoothGpuSortMs() const { return m_smoothGpuSortMs; }
        float GetSmoothDispatchMs() const { return m_smoothDispatchMs; }
        float GetSmoothUiMs() const { return m_smoothUiMs; }

    private:
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
            float      pad3;
        };

        CAULDRON_DX12::Device* m_pDevice = nullptr;

        CAULDRON_DX12::ResourceViewHeaps m_resourceViewHeaps;
        CAULDRON_DX12::UploadHeap        m_uploadHeap;
        CAULDRON_DX12::DynamicBufferRing m_constantBufferRing;
        CAULDRON_DX12::CommandListRing   m_commandListRing;
        CAULDRON_DX12::ImGUI             m_imGui;

        ID3D12RootSignature* m_pRootSignature = nullptr;
        ID3D12PipelineState* m_pPipelineState = nullptr;

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

        float                      m_fpsAccumTimeMs = 0.0f;
        uint32_t                   m_fpsAccumFrames = 0;
        float                      m_smoothGpuSortMs = 0.0f;
        float                      m_smoothDispatchMs = 0.0f;
        float                      m_smoothUiMs = 0.0f;
        std::chrono::high_resolution_clock::time_point m_lastWallClockTime;

        ID3D12Resource*            m_pSurfelGpuOutBuffer = nullptr;
        ID3D12Resource*            m_pRawSurfelGpuOutBuffer = nullptr;
        ID3D12Resource*            m_pGPUSortPairBuffer = nullptr;
        uint32_t                   m_sortPairBufferCapacityBytes = 0;
        bool                       m_needUploadToGpu = false;
        bool                       m_gpuSortNeedsRun = true;

        std::vector<MeshletChunkGPU> m_meshletChunks;
        ID3D12Resource*            m_pChunkUploadBuffer = nullptr;
        uint8_t*                   m_pChunkUploadBufferMapped = nullptr;
        ID3D12Resource*            m_pChunkGpuBuffer = nullptr;
        ID3D12Resource*            m_pSortedChunkIndicesGpuBuffer = nullptr;
        D3D12_RESOURCE_STATES      m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_COPY_DEST;
        ID3D12Resource*            m_pSortedChunkIndicesUploadBuffer = nullptr;
        uint32_t*                  m_pSortedChunkIndicesUploadBufferMapped = nullptr;
        uint32_t                   m_chunkBufferCapacityBytes = 0;
        uint32_t                   m_sortedChunkIndicesCapacityBytes = 0;
        bool                       m_lastGpuRadixSort = false;
        bool                       m_lastUseChunkedPipeline = false;
        bool                       m_needUploadChunkIndicesToGpu = false;

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
    };
}

