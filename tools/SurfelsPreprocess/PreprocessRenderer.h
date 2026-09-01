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
            uint32_t surfelCount = 0;
        };

        void OnCreate(CAULDRON_DX12::Device* pDevice, CAULDRON_DX12::SwapChain* pSwapChain);
        void OnDestroy();

        void OnCreateWindowSizeDependentResources(CAULDRON_DX12::SwapChain* pSwapChain, uint32_t width, uint32_t height);
        void OnDestroyWindowSizeDependentResources();
        void OnUpdateDisplayDependentResources(CAULDRON_DX12::SwapChain* pSwapChain);

        void OnRender(State* pState, CAULDRON_DX12::SwapChain* pSwapChain);

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
            float      pad0;
            XMFLOAT3   aabbMin;
            float      pad1;
            XMFLOAT3   aabbExtents;
            float      pad2;
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

        void UpdateSurfelBuffers(const PackedSurfelGPU* pSurfels, const SurfelVertex* pRawSurfels, uint32_t surfelCount, uint32_t renderMode, XMFLOAT3 eyePos, XMFLOAT3 forward);

        ID3D12Resource*            m_pSurfelBuffer = nullptr;
        uint8_t*                   m_pSurfelBufferMapped = nullptr;
        uint32_t                   m_surfelBufferCapacityBytes = 0;
        D3D12_GPU_VIRTUAL_ADDRESS  m_surfelBufferGPUAddress = 0;

        ID3D12Resource*            m_pRawSurfelBuffer = nullptr;
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
    };
}

