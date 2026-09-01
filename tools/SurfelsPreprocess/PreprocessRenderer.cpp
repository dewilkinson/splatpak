#include "PreprocessRenderer.h"
#include <d3dx12.h>
#include "Misc/Error.h"

namespace Surfels
{
    using namespace CAULDRON_DX12;

    static const uint32_t BACK_BUFFER_COUNT = 2;
    static const uint32_t SURFELS_PER_GROUP = 32;

    typedef CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> CD3DX12_PIPELINE_STATE_STREAM_MS;
    typedef CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> CD3DX12_PIPELINE_STATE_STREAM_AS;

    struct MeshShaderPipelineStateStream
    {
        CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE        RootSignature;
        CD3DX12_PIPELINE_STATE_STREAM_MS                    MS;
        CD3DX12_PIPELINE_STATE_STREAM_PS                    PS;
        CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER            RasterizerState;
        CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC            BlendState;
        CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL         DepthStencilState;
        CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT  DSVFormat;
        CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
        CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC           SampleDesc;
    };

    void PreprocessRenderer::OnCreate(Device* pDevice, SwapChain* pSwapChain)
    {
        m_pDevice = pDevice;

        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
            HRESULT hr = pDevice->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
            if (FAILED(hr) || options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
            {
                MessageBoxA(NULL, "Mesh Shaders are not supported on this GPU.", "SurfelsPreprocess", MB_ICONERROR);
                exit(1);
            }
        }

        m_resourceViewHeaps.OnCreate(pDevice, 10, 10, 10, 10, 10, 10);
        m_resourceViewHeaps.AllocDSVDescriptor(1, &m_depthBufferDSV);
        m_uploadHeap.OnCreate(pDevice, 32 * 1024 * 1024);
        m_constantBufferRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 32 * 1024 * 1024, &m_resourceViewHeaps);

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        m_commandListRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 2, queueDesc);

        m_imGui.OnCreate(pDevice, &m_uploadHeap, &m_resourceViewHeaps, &m_constantBufferRing, pSwapChain->GetFormat());

        CD3DX12_ROOT_PARAMETER rootParams[3];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // b0
        rootParams[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // t0 - g_SurfelBuffer
        rootParams[2].InitAsShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_ALL); // t1 - g_RawSurfelBuffer

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters = rootParams;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
        D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);
        m_pDevice->GetDevice()->CreateRootSignature(0, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));

        D3D12_SHADER_BYTECODE ms = {};
        D3D12_SHADER_BYTECODE ps = {};
        CompileShaderFromFile("Surfels.hlsl", NULL, "mainMS", "-T ms_6_5", &ms);
        CompileShaderFromFile("Surfels.hlsl", NULL, "mainPS", "-T ps_6_5", &ps);

        CD3DX12_RASTERIZER_DESC rasterizer(D3D12_DEFAULT);
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;

        // Back-to-front order-dependent blending handles depth accumulation naturally
        CD3DX12_DEPTH_STENCIL_DESC depthStencil(D3D12_DEFAULT);
        depthStencil.DepthEnable = FALSE;
        depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        depthStencil.StencilEnable = FALSE;

        D3D12_RT_FORMAT_ARRAY rtvFormats = {};
        rtvFormats.NumRenderTargets = 1;
        rtvFormats.RTFormats[0] = pSwapChain->GetFormat();

        // Premultiplied alpha blending: Output = SrcColor + DestColor * (1 - SrcAlpha)
        CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        MeshShaderPipelineStateStream stream = {};
        stream.RootSignature = m_pRootSignature;
        stream.MS = ms;
        stream.PS = ps;
        stream.RasterizerState = rasterizer;
        stream.BlendState = blendDesc;
        stream.DepthStencilState = depthStencil;
        stream.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        stream.RTVFormats = rtvFormats;
        stream.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {};
        streamDesc.SizeInBytes = sizeof(stream);
        streamDesc.pPipelineStateSubobjectStream = &stream;

        Microsoft::WRL::ComPtr<ID3D12Device2> device2;
        m_pDevice->GetDevice()->QueryInterface(IID_PPV_ARGS(&device2));
        device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&m_pPipelineState));
    }

    void PreprocessRenderer::OnDestroy()
    {
        if (m_pSurfelBuffer) { m_pSurfelBuffer->Unmap(0, nullptr); m_pSurfelBuffer->Release(); m_pSurfelBuffer = nullptr; }
        m_pSurfelBufferMapped = nullptr;
        m_surfelBufferCapacityBytes = 0;
        m_surfelBufferGPUAddress = 0;

        if (m_pRawSurfelBuffer) { m_pRawSurfelBuffer->Unmap(0, nullptr); m_pRawSurfelBuffer->Release(); m_pRawSurfelBuffer = nullptr; }
        m_pRawSurfelBufferMapped = nullptr;
        m_rawSurfelBufferCapacityBytes = 0;
        m_rawSurfelBufferGPUAddress = 0;

        m_lastSurfelsPtr = nullptr;
        m_lastSurfelCount = 0;
        m_lastRenderMode = 0;

        if (m_pPipelineState) { m_pPipelineState->Release(); m_pPipelineState = nullptr; }
        if (m_pRootSignature) { m_pRootSignature->Release(); m_pRootSignature = nullptr; }

        m_depthBuffer.OnDestroy();
        m_imGui.OnDestroy();
        m_commandListRing.OnDestroy();
        m_constantBufferRing.OnDestroy();
        m_uploadHeap.OnDestroy();
        m_resourceViewHeaps.OnDestroy();
    }

    void PreprocessRenderer::UpdateSurfelBuffers(
        const PackedSurfelGPU* pSurfels,
        const SurfelVertex* pRawSurfels,
        uint32_t surfelCount,
        uint32_t renderMode,
        XMFLOAT3 eyePos,
        XMFLOAT3 forward)
    {
        const void* activePtr = (renderMode == 2) ? (const void*)pRawSurfels : (const void*)pSurfels;

        if (activePtr == nullptr || surfelCount == 0)
        {
            m_surfelBufferGPUAddress = 0;
            m_rawSurfelBufferGPUAddress = 0;
            return;
        }

        float camMoved = std::abs(eyePos.x - m_lastSortEye.x) + std::abs(eyePos.y - m_lastSortEye.y) + std::abs(eyePos.z - m_lastSortEye.z);
        float forwardMoved = std::abs(forward.x - m_lastSortForward.x) + std::abs(forward.y - m_lastSortForward.y) + std::abs(forward.z - m_lastSortForward.z);
        bool needsSort = (m_lastSurfelsPtr != activePtr) || (m_lastSurfelCount != surfelCount) || (m_lastRenderMode != renderMode) || (camMoved > 0.005f) || (forwardMoved > 0.002f);

        m_lastSurfelsPtr = activePtr;
        m_lastSurfelCount = surfelCount;
        m_lastRenderMode = renderMode;

        if (renderMode == 2)
        {
            // Mode 2: Raw Float32 Points / SurfelVertex
            uint32_t requiredBytes = surfelCount * sizeof(SurfelVertex);

            if (requiredBytes > m_rawSurfelBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);

                if (m_pRawSurfelBuffer) { m_pRawSurfelBuffer->Unmap(0, nullptr); m_pRawSurfelBuffer->Release(); m_pRawSurfelBuffer = nullptr; }
                m_pRawSurfelBufferMapped = nullptr;

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                    D3D12_HEAP_FLAG_NONE,
                    &CD3DX12_RESOURCE_DESC::Buffer(requiredBytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&m_pRawSurfelBuffer)));
                SetName(m_pRawSurfelBuffer, "PreprocessRenderer::m_pRawSurfelBuffer");

                m_pRawSurfelBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pRawSurfelBufferMapped));
                m_rawSurfelBufferCapacityBytes = requiredBytes;
                needsSort = true;
            }

            if (surfelCount > 0 && pRawSurfels != nullptr && m_pRawSurfelBufferMapped != nullptr)
            {
                if (needsSort)
                {
                    m_lastSortEye = eyePos;
                    m_lastSortForward = forward;

                    if (m_sortIndicesA.size() < surfelCount)
                    {
                        m_sortIndicesA.resize(surfelCount);
                        m_sortIndicesB.resize(surfelCount);
                        m_sortKeys.resize(surfelCount);
                        m_sortDists.resize(surfelCount);
                    }

                    // 1. Calculate projected distance along camera forward vector
                    float minD = 1e9f, maxD = -1e9f;
                    for (uint32_t i = 0; i < surfelCount; i++)
                    {
                        const auto& pos = pRawSurfels[i].position;
                        float d = (pos.x - eyePos.x) * forward.x + (pos.y - eyePos.y) * forward.y + (pos.z - eyePos.z) * forward.z;
                        m_sortDists[i] = d;
                        if (d < minD) minD = d;
                        if (d > maxD) maxD = d;
                    }

                    float range = std::max(0.001f, maxD - minD);
                    float scale = 65535.0f / range;

                    // 2. Compute 16-bit keys (inverted for back-to-front descending order)
                    for (uint32_t i = 0; i < surfelCount; i++)
                    {
                        uint32_t k = (uint32_t)((m_sortDists[i] - minD) * scale);
                        if (k > 65535) k = 65535;
                        m_sortKeys[i] = 65535 - k;
                        m_sortIndicesA[i] = i;
                    }

                    // 3. Pass 1: Radix sort low 8 bits (Byte 0)
                    uint32_t count0[256] = {};
                    for (uint32_t i = 0; i < surfelCount; i++)
                        count0[m_sortKeys[i] & 0xFF]++;

                    uint32_t offset0[256] = {};
                    for (uint32_t i = 1; i < 256; i++)
                        offset0[i] = offset0[i - 1] + count0[i - 1];

                    for (uint32_t i = 0; i < surfelCount; i++)
                    {
                        uint32_t idx = m_sortIndicesA[i];
                        uint8_t byte0 = (uint8_t)(m_sortKeys[idx] & 0xFF);
                        m_sortIndicesB[offset0[byte0]++] = idx;
                    }

                    // 4. Pass 2: Radix sort high 8 bits (Byte 1)
                    uint32_t count1[256] = {};
                    for (uint32_t i = 0; i < surfelCount; i++)
                        count1[(m_sortKeys[m_sortIndicesB[i]] >> 8) & 0xFF]++;

                    uint32_t offset1[256] = {};
                    for (uint32_t i = 1; i < 256; i++)
                        offset1[i] = offset1[i - 1] + count1[i - 1];

                    for (uint32_t i = 0; i < surfelCount; i++)
                    {
                        uint32_t idx = m_sortIndicesB[i];
                        uint8_t byte1 = (uint8_t)((m_sortKeys[idx] >> 8) & 0xFF);
                        m_sortIndicesA[offset1[byte1]++] = idx;
                    }

                    // 5. Gather sorted surfels directly into mapped GPU upload buffer
                    SurfelVertex* pDst = reinterpret_cast<SurfelVertex*>(m_pRawSurfelBufferMapped);
                    for (uint32_t i = 0; i < surfelCount; i++)
                    {
                        pDst[i] = pRawSurfels[m_sortIndicesA[i]];
                    }
                }
            }
            m_rawSurfelBufferGPUAddress = m_pRawSurfelBuffer->GetGPUVirtualAddress();
        }
        else
        {
            // Mode 1: Quantized 8-Byte GPU Surfels (PackedSurfelGPU)
            uint32_t requiredBytes = surfelCount * sizeof(PackedSurfelGPU);

            if (requiredBytes > m_surfelBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);

                if (m_pSurfelBuffer) { m_pSurfelBuffer->Unmap(0, nullptr); m_pSurfelBuffer->Release(); m_pSurfelBuffer = nullptr; }
                m_pSurfelBufferMapped = nullptr;

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                    D3D12_HEAP_FLAG_NONE,
                    &CD3DX12_RESOURCE_DESC::Buffer(requiredBytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&m_pSurfelBuffer)));
                SetName(m_pSurfelBuffer, "PreprocessRenderer::m_pSurfelBuffer");

                m_pSurfelBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pSurfelBufferMapped));
                m_surfelBufferCapacityBytes = requiredBytes;
            }

            if (needsSort && m_pSurfelBufferMapped)
            {
                memcpy(m_pSurfelBufferMapped, pSurfels, requiredBytes);
            }

            m_surfelBufferGPUAddress = m_pSurfelBuffer->GetGPUVirtualAddress();
        }
    }

    void PreprocessRenderer::OnCreateWindowSizeDependentResources(SwapChain* /*pSwapChain*/, uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;

        m_depthBuffer.InitDepthStencil(m_pDevice, "PreprocessRenderer::m_depthBuffer", &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), 1.0f);
        m_depthBuffer.CreateDSV(0, &m_depthBufferDSV);
    }

    void PreprocessRenderer::OnDestroyWindowSizeDependentResources()
    {
        m_depthBuffer.OnDestroy();
    }

    void PreprocessRenderer::OnUpdateDisplayDependentResources(SwapChain* /*pSwapChain*/) {}

    void PreprocessRenderer::OnRender(State* pState, SwapChain* pSwapChain)
    {
        // Throttle CPU so it does not overwrite in-flight command allocator / dynamic buffers
        pSwapChain->WaitForSwapChain();

        m_constantBufferRing.OnBeginFrame();
        m_commandListRing.OnBeginFrame();

        ID3D12GraphicsCommandList* pCmdLst = m_commandListRing.GetNewCommandList();

        ID3D12DescriptorHeap* descriptorHeaps[] = { m_resourceViewHeaps.GetCBV_SRV_UAVHeap() };
        pCmdLst->SetDescriptorHeaps(1, descriptorHeaps);

        ID3D12Resource* pBackBuffer = pSwapChain->GetCurrentBackBufferResource();
        {
            D3D12_RESOURCE_BARRIER toRTV = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            pCmdLst->ResourceBarrier(1, &toRTV);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = *pSwapChain->GetCurrentBackBufferRTV();
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_depthBufferDSV.GetCPU();
        pCmdLst->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // Pure pitch black (SuperSplat reference)
        pCmdLst->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        pCmdLst->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        CD3DX12_VIEWPORT viewport(0.0f, 0.0f, (float)m_width, (float)m_height);
        CD3DX12_RECT scissor(0, 0, m_width, m_height);
        pCmdLst->RSSetViewports(1, &viewport);
        pCmdLst->RSSetScissorRects(1, &scissor);

        // Camera calculations
        const float cy = cosf(pState->camPitch), sy = sinf(pState->camPitch);
        const float sx = sinf(pState->camYaw), cx = cosf(pState->camYaw);
        XMFLOAT3 eyePos(
            pState->camTarget.x + pState->camDistance * cy * sx,
            pState->camTarget.y + pState->camDistance * sy,
            pState->camTarget.z + pState->camDistance * cy * cx
        );

        XMVECTOR eye = XMLoadFloat3(&eyePos);
        XMVECTOR at = XMLoadFloat3(&pState->camTarget);
        XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(at, eye));
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
        XMVECTOR camUp = XMVector3Cross(forward, right);

        XMMATRIX view = XMMatrixLookAtRH(eye, at, worldUp);
        XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, pState->aspectRatio, 0.1f, 500.0f);
        XMMATRIX viewProj = XMMatrixMultiply(view, proj);

        XMFLOAT3 forwardNorm;
        XMStoreFloat3(&forwardNorm, forward);
        UpdateSurfelBuffers(pState->pSurfels, pState->pRawSurfels, pState->surfelCount, pState->renderMode, eyePos, forwardNorm);
        uint32_t surfelCount = pState->surfelCount;

        SurfelsCB* pCB = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = 0;
        if (!m_constantBufferRing.AllocConstantBuffer(sizeof(SurfelsCB), (void**)&pCB, &cbAddress))
        {
            Trace("PreprocessRenderer: failed to allocate frame constant buffer, skipping draw\n");

            D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            pCmdLst->ResourceBarrier(1, &toPresent);
            pCmdLst->Close();

            ID3D12CommandList* pCmdLists[] = { pCmdLst };
            m_pDevice->GetGraphicsQueue()->ExecuteCommandLists(1, pCmdLists);
            return;
        }

        XMStoreFloat4x4(&pCB->viewProj, viewProj);
        XMStoreFloat3(&pCB->camRight, right);
        pCB->radius = pState->splatRadius;
        XMStoreFloat3(&pCB->camUp, camUp);
        pCB->time = pState->time;
        pCB->sphereCenter = pState->camTarget;
        pCB->sphereRadius = 1.0f;
        pCB->surfelCount = surfelCount;
        pCB->renderMode = pState->renderMode;
        pCB->orientMode = pState->orientMode;
        pCB->pad0 = 0.0f;
        pCB->aabbMin = pState->aabbMin;
        pCB->pad1 = 0.0f;
        pCB->aabbExtents = pState->aabbExtents;
        pCB->pad2 = 0.0f;

        pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
        pCmdLst->SetPipelineState(m_pPipelineState);
        pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);

        D3D12_GPU_VIRTUAL_ADDRESS srv0 = (m_surfelBufferGPUAddress != 0) ? m_surfelBufferGPUAddress : m_rawSurfelBufferGPUAddress;
        D3D12_GPU_VIRTUAL_ADDRESS srv1 = (m_rawSurfelBufferGPUAddress != 0) ? m_rawSurfelBufferGPUAddress : m_surfelBufferGPUAddress;
        pCmdLst->SetGraphicsRootShaderResourceView(1, srv0);
        pCmdLst->SetGraphicsRootShaderResourceView(2, srv1);

        if (surfelCount > 0 && (srv0 != 0 || srv1 != 0))
        {
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
            pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
            uint32_t groupCount = (surfelCount + SURFELS_PER_GROUP - 1) / SURFELS_PER_GROUP;
            cmdList6->DispatchMesh(groupCount, 1, 1);
        }

        // Draw ImGui UI on top
        m_imGui.Draw(pCmdLst);

        {
            D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            pCmdLst->ResourceBarrier(1, &toPresent);
        }

        pCmdLst->Close();

        ID3D12CommandList* pCmdLists[] = { pCmdLst };
        m_pDevice->GetGraphicsQueue()->ExecuteCommandLists(1, pCmdLists);
    }
}
