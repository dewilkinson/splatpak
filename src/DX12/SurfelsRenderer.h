#pragma once
#include "stdafx.h"
#include "base/Texture.h"
#include "Wavelet/WaveletTypes.h"

// GPU-side rendering for the sample: a bare Cauldron render loop (device/swapchain
// are owned by FrameworkWindows) that draws a procedural, instanced point-splat
// cloud or dynamic streamed wavelet surfels via DirectX 12 Mesh Shaders.
class SurfelsRenderer
{
public:
    struct State
    {
        uint32_t surfelCount  = 4096;
        float    splatRadius  = 0.035f;
        float    sphereRadius = 1.6f;
        bool     autoRotate   = true;
        float    time         = 0.0f;

        float    camYaw      = 0.6f;
        float    camPitch    = 0.35f;
        float    camDistance = 4.0f;
        float    aspectRatio = 16.0f / 9.0f;

        uint32_t renderMode  = 0; // 0 = Procedural Sphere, 1 = Streamed Wavelet
        uint32_t orientMode  = 0; // 0 = Normal-Oriented Discs, 1 = Camera-Facing Billboards
        XMFLOAT3 aabbMin     = { -40.0f, -2.0f, -80.0f };
        XMFLOAT3 aabbExtents = { 80.0f, 30.0f, 160.0f };
        const Surfels::PackedSurfelGPU* pStreamedSurfels = nullptr;
        uint32_t streamedSurfelCount = 0;
        bool     gpuRadixSort = true;
    };

    void OnCreate(CAULDRON_DX12::Device* pDevice, CAULDRON_DX12::SwapChain* pSwapChain);
    void OnDestroy();

    void OnCreateWindowSizeDependentResources(CAULDRON_DX12::SwapChain* pSwapChain, uint32_t width, uint32_t height);
    void OnDestroyWindowSizeDependentResources();
    void OnUpdateDisplayDependentResources(CAULDRON_DX12::SwapChain* pSwapChain);

    void OnRender(State* pState, CAULDRON_DX12::SwapChain* pSwapChain);

    const std::vector<TimeStamp>& GetGPUTimestamps() const { return m_gpuTimestamps; }

private:
    struct SurfelsCB
    {
        XMFLOAT4X4 viewProj;
        XMFLOAT3   camRight;
        float      radius;
        XMFLOAT3   camUp;
        float      time;
        XMFLOAT3   sphereCenter;
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
    CAULDRON_DX12::GPUTimestamps     m_gpuTimer;

    std::vector<TimeStamp> m_gpuTimestamps;

    ID3D12RootSignature* m_pRootSignature = nullptr;
    ID3D12PipelineState* m_pPipelineState = nullptr;

    ID3D12RootSignature* m_pComputeRootSignature = nullptr;
    ID3D12PipelineState* m_pProjectKeysPSO = nullptr;
    ID3D12PipelineState* m_pBitonicLocalSortPSO = nullptr;
    ID3D12PipelineState* m_pBitonicGlobalSortPSO = nullptr;
    ID3D12PipelineState* m_pBitonicLocalMergePSO = nullptr;
    ID3D12PipelineState* m_pGatherSurfelsPSO = nullptr;
    ID3D12PipelineState* m_pRadixSortPSO = nullptr;

    ID3D12Resource*      m_pSurfelBuffer = nullptr;
    ID3D12Resource*      m_pSurfelGpuBuffer = nullptr;
    ID3D12Resource*      m_pSurfelGpuOutBuffer = nullptr;
    ID3D12Resource*      m_pGPUSortPairBuffer = nullptr;
    uint32_t             m_sortPairBufferCapacityBytes = 0;
    uint8_t*             m_pSurfelBufferMapped = nullptr;
    uint32_t             m_surfelBufferCapacityBytes = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_surfelBufferGPUAddress = 0;

    const void*          m_lastSurfelsPtr = nullptr;
    uint32_t             m_lastSurfelCount = 0;
    bool                 m_needUploadToGpu = false;
    bool                 m_gpuSortNeedsRun = true;
    DirectX::XMFLOAT3    m_lastSortEye = { 0, 0, 0 };
    DirectX::XMFLOAT3    m_lastSortForward = { 0, 0, 0 };

    CAULDRON_DX12::Texture m_depthBuffer;
    CAULDRON_DX12::DSV     m_depthBufferDSV;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};
