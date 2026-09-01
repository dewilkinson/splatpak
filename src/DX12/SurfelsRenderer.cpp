#include "stdafx.h"
#include "SurfelsRenderer.h"
#include "Misc/Error.h"

using namespace CAULDRON_DX12;

static const uint32_t BACK_BUFFER_COUNT = 2;

// Must match SURFELS_PER_GROUP in Surfels.hlsl -- how many surfels each mesh
// shader threadgroup builds (4 verts + 2 triangles each: 128 verts / 64 tris
// per group at 32, safely under D3D12's 256/256 mesh-shader output limits).
static const uint32_t SURFELS_PER_GROUP = 32;

// Cauldron's vendored d3dx12.h (libs/cauldron/libs/d3d12x/d3dx12.h) predates mesh
// shaders: it has the generic CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT template and
// every other stage's subobject typedef, but not MS/AS. The underlying Windows SDK
// d3d12.h already defines D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS/_AS and
// ID3D12Device2/ID3D12GraphicsCommandList6 fine, so just add the two missing
// typedefs here rather than patching the vendored submodule.
typedef CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> CD3DX12_PIPELINE_STATE_STREAM_MS;
typedef CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> CD3DX12_PIPELINE_STATE_STREAM_AS;

struct MeshShaderPipelineStateStream
{
    CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE        RootSignature;
    CD3DX12_PIPELINE_STATE_STREAM_AS                    AS;
    CD3DX12_PIPELINE_STATE_STREAM_MS                    MS;
    CD3DX12_PIPELINE_STATE_STREAM_PS                    PS;
    CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER            RasterizerState;
    CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC            BlendState;
    CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL         DepthStencilState;
    CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT  DSVFormat;
    CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
    CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC           SampleDesc;
};

void SurfelsRenderer::OnCreate(Device* pDevice, SwapChain* pSwapChain)
{
    m_pDevice = pDevice;

    // Mesh shaders require D3D12 Mesh Shader Tier 1 (Shader Model 6.5+ hardware).
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
        HRESULT hr = pDevice->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
        if (FAILED(hr) || options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
        {
            MessageBoxA(NULL, "This GPU/driver does not support D3D12 mesh shaders (Mesh Shader Tier 1 / Shader Model 6.5).", "Surfels", MB_ICONERROR);
            exit(1);
        }
    }

    // Descriptor heaps, upload ring, per-frame constant buffer ring, command lists.
    m_resourceViewHeaps.OnCreate(pDevice, 10, 10, 10, 10, 10, 10);
    m_resourceViewHeaps.AllocDSVDescriptor(1, &m_depthBufferDSV);
    m_uploadHeap.OnCreate(pDevice, 32 * 1024 * 1024);
    m_constantBufferRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 32 * 1024 * 1024, &m_resourceViewHeaps);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    m_commandListRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 2, queueDesc);

    m_gpuTimer.OnCreate(pDevice, BACK_BUFFER_COUNT);

    m_imGui.OnCreate(pDevice, &m_uploadHeap, &m_resourceViewHeaps, &m_constantBufferRing, pSwapChain->GetFormat());

    // Root signature: CBV at b0, StructuredBuffer SRV at t0, Raw StructuredBuffer SRV at t1, ChunkBuffer at t2, SortedIndices at t3
    CD3DX12_ROOT_PARAMETER rootParams[5];
    rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // b0
    rootParams[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // t0
    rootParams[2].InitAsShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_ALL); // t1
    rootParams[3].InitAsShaderResourceView(2, 0, D3D12_SHADER_VISIBILITY_ALL); // t2
    rootParams[4].InitAsShaderResourceView(3, 0, D3D12_SHADER_VISIBILITY_ALL); // t3

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 5;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // no input assembler stage with mesh shaders

    Microsoft::WRL::ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);
    m_pDevice->GetDevice()->CreateRootSignature(0, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));
    SetName(m_pRootSignature, "Surfels::RootSignature");

    // Create Compute Root Signature & Compute Pipeline States (GPU LDS Key-Index Sorting)
    CD3DX12_ROOT_PARAMETER computeRootParams[8];
    computeRootParams[0].InitAsConstantBufferView(0, 0); // b0 - BitonicSortCB
    computeRootParams[1].InitAsShaderResourceView(0, 0); // t0 - InPackedSurfels
    computeRootParams[2].InitAsShaderResourceView(1, 0); // t1 - InRawSurfels
    computeRootParams[3].InitAsShaderResourceView(2, 0); // t2 - InChunks
    computeRootParams[4].InitAsUnorderedAccessView(0, 0); // u0 - SortPairs
    computeRootParams[5].InitAsUnorderedAccessView(1, 0); // u1 - OutPackedSurfels
    computeRootParams[6].InitAsUnorderedAccessView(2, 0); // u2 - OutRawSurfels
    computeRootParams[7].InitAsUnorderedAccessView(3, 0); // u3 - OutSortedChunkIndices

    CD3DX12_ROOT_SIGNATURE_DESC computeRsDesc = {};
    computeRsDesc.NumParameters = 8;
    computeRsDesc.pParameters = computeRootParams;
    computeRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> pCompOutBlob, pCompErrBlob;
    D3D12SerializeRootSignature(&computeRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pCompOutBlob, &pCompErrBlob);
    m_pDevice->GetDevice()->CreateRootSignature(0, pCompOutBlob->GetBufferPointer(), pCompOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pComputeRootSignature));
    SetName(m_pComputeRootSignature, "Surfels::ComputeRootSignature");

    auto CreateComputePSO = [&](const char* entryPoint, ID3D12PipelineState** ppPSO)
    {
        D3D12_SHADER_BYTECODE cs = {};
        if (CompileShaderFromFile("GPURadixSortCS.hlsl", NULL, entryPoint, "-T cs_6_0", &cs) && cs.pShaderBytecode != nullptr)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.pRootSignature = m_pComputeRootSignature;
            psoDesc.CS = cs;
            m_pDevice->GetDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(ppPSO));
        }
        else
        {
            Trace("ERROR: Failed to compile GPURadixSortCS.hlsl (%s)!\n", entryPoint);
        }
    };

    CreateComputePSO("ProjectKeysCS", &m_pProjectKeysPSO);
    CreateComputePSO("BitonicLocalSortCS", &m_pBitonicLocalSortPSO);
    CreateComputePSO("BitonicGlobalSortCS", &m_pBitonicGlobalSortPSO);
    CreateComputePSO("BitonicLocalMergeCS", &m_pBitonicLocalMergePSO);
    CreateComputePSO("GatherSurfelsCS", &m_pGatherSurfelsPSO);

    D3D12_SHADER_BYTECODE as = {};
    D3D12_SHADER_BYTECODE ms = {};
    D3D12_SHADER_BYTECODE ps = {};
    CompileShaderFromFile("Surfels.hlsl", NULL, "mainAS", "-T as_6_5", &as);
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
    stream.AS = as;
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

    // Mesh-shader PSOs need the pipeline-state-stream API, which requires ID3D12Device2.
    Microsoft::WRL::ComPtr<ID3D12Device2> device2;
    if (FAILED(m_pDevice->GetDevice()->QueryInterface(IID_PPV_ARGS(&device2))))
    {
        MessageBoxA(NULL, "ID3D12Device2 is unavailable (needed to create a mesh-shader pipeline state).", "Surfels", MB_ICONERROR);
        exit(1);
    }
    if (FAILED(device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&m_pPipelineState))))
    {
        MessageBoxA(NULL, "Failed to create the mesh-shader pipeline state.", "Surfels", MB_ICONERROR);
        exit(1);
    }
    SetName(m_pPipelineState, "Surfels::PSO");
}

void SurfelsRenderer::OnDestroy()
{
    if (m_pSurfelBuffer) { m_pSurfelBuffer->Unmap(0, nullptr); m_pSurfelBuffer->Release(); m_pSurfelBuffer = nullptr; }
    if (m_pSurfelGpuBuffer) { m_pSurfelGpuBuffer->Release(); m_pSurfelGpuBuffer = nullptr; }
    if (m_pSurfelGpuOutBuffer) { m_pSurfelGpuOutBuffer->Release(); m_pSurfelGpuOutBuffer = nullptr; }
    if (m_pGPUSortPairBuffer) { m_pGPUSortPairBuffer->Release(); m_pGPUSortPairBuffer = nullptr; }
    m_sortPairBufferCapacityBytes = 0;
    m_pSurfelBufferMapped = nullptr;
    m_surfelBufferCapacityBytes = 0;
    m_surfelBufferGPUAddress = 0;

    if (m_pProjectKeysPSO) { m_pProjectKeysPSO->Release(); m_pProjectKeysPSO = nullptr; }
    if (m_pBitonicLocalSortPSO) { m_pBitonicLocalSortPSO->Release(); m_pBitonicLocalSortPSO = nullptr; }
    if (m_pBitonicGlobalSortPSO) { m_pBitonicGlobalSortPSO->Release(); m_pBitonicGlobalSortPSO = nullptr; }
    if (m_pBitonicLocalMergePSO) { m_pBitonicLocalMergePSO->Release(); m_pBitonicLocalMergePSO = nullptr; }
    if (m_pGatherSurfelsPSO) { m_pGatherSurfelsPSO->Release(); m_pGatherSurfelsPSO = nullptr; }
    if (m_pRadixSortPSO) { m_pRadixSortPSO->Release(); m_pRadixSortPSO = nullptr; }
    if (m_pComputeRootSignature) { m_pComputeRootSignature->Release(); m_pComputeRootSignature = nullptr; }
    if (m_pPipelineState) { m_pPipelineState->Release(); m_pPipelineState = nullptr; }
    if (m_pRootSignature) { m_pRootSignature->Release(); m_pRootSignature = nullptr; }

    m_gpuTimer.OnDestroy();
    m_imGui.OnDestroy();
    m_commandListRing.OnDestroy();
    m_constantBufferRing.OnDestroy();
    m_uploadHeap.OnDestroy();
    m_resourceViewHeaps.OnDestroy();
}

void SurfelsRenderer::OnCreateWindowSizeDependentResources(SwapChain* /*pSwapChain*/, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    m_depthBuffer.InitDepthStencil(m_pDevice, "SurfelsRenderer::m_depthBuffer", &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), 1.0f);
    m_depthBuffer.CreateDSV(0, &m_depthBufferDSV);
}

void SurfelsRenderer::OnDestroyWindowSizeDependentResources()
{
    m_depthBuffer.OnDestroy();
}

void SurfelsRenderer::OnUpdateDisplayDependentResources(SwapChain* pSwapChain)
{
    m_imGui.UpdatePipeline(pSwapChain->GetFormat());
}

void SurfelsRenderer::OnRender(State* pState, SwapChain* pSwapChain)
{
    // Throttle the CPU so it doesn't get more than (BackBufferCount - 1) frames
    // ahead of the GPU before reusing this frame's command allocator.
    pSwapChain->WaitForSwapChain();

    // Read back previous frame's GPU timings
    UINT64 gpuTicksPerSecond = 0;
    m_pDevice->GetGraphicsQueue()->GetTimestampFrequency(&gpuTicksPerSecond);
    m_gpuTimer.OnBeginFrame(gpuTicksPerSecond, &m_gpuTimestamps);

    m_commandListRing.OnBeginFrame();
    m_constantBufferRing.OnBeginFrame();

    ID3D12GraphicsCommandList2* pCmdLst = m_commandListRing.GetNewCommandList();

    m_gpuTimer.GetTimeStamp(pCmdLst, "Frame Begin");

    ID3D12Resource* pBackBuffer = pSwapChain->GetCurrentBackBufferResource();
    D3D12_CPU_DESCRIPTOR_HANDLE* pRTV = pSwapChain->GetCurrentBackBufferRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_depthBufferDSV.GetCPU();

    {
        D3D12_RESOURCE_BARRIER toRT = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pCmdLst->ResourceBarrier(1, &toRT);
    }

    const float clearColor[4] = { 0.02f, 0.02f, 0.05f, 1.0f };
    pCmdLst->OMSetRenderTargets(1, pRTV, TRUE, &dsvHandle);
    pCmdLst->ClearRenderTargetView(*pRTV, clearColor, 0, nullptr);
    pCmdLst->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)m_width, (LONG)m_height };
    pCmdLst->RSSetViewports(1, &viewport);
    pCmdLst->RSSetScissorRects(1, &scissor);

    m_gpuTimer.GetTimeStamp(pCmdLst, "Clear & Setup");

    // ---- Orbit camera ----
    const float cy = cosf(pState->camPitch), sy = sinf(pState->camPitch);
    const float sx = sinf(pState->camYaw), cx = cosf(pState->camYaw);
    XMVECTOR eye = XMVectorSet(pState->camDistance * cy * sx, pState->camDistance * sy, pState->camDistance * cy * cx, 0.0f);
    XMVECTOR at = XMVectorZero();
    XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(at, eye));
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
    XMVECTOR camUp = XMVector3Cross(forward, right);

    XMMATRIX view = XMMatrixLookAtRH(eye, at, worldUp);
    XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, pState->aspectRatio, 0.1f, 100.0f);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    uint32_t surfelCount = pState->surfelCount;

    if (pState->renderMode == 1 && pState->pStreamedSurfels != nullptr && pState->streamedSurfelCount > 0)
    {
        surfelCount = pState->streamedSurfelCount;

        // Calculate power of 2 size for GPU Bitonic sorting network
        uint32_t numElements = 1;
        while (numElements < surfelCount) numElements <<= 1;
        uint32_t requiredBytes = numElements * sizeof(Surfels::PackedSurfelGPU);

        if (requiredBytes > m_surfelBufferCapacityBytes)
        {
            m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);

            if (m_pSurfelBuffer) { m_pSurfelBuffer->Unmap(0, nullptr); m_pSurfelBuffer->Release(); m_pSurfelBuffer = nullptr; }
            if (m_pSurfelGpuBuffer) { m_pSurfelGpuBuffer->Release(); m_pSurfelGpuBuffer = nullptr; }
            if (m_pSurfelGpuOutBuffer) { m_pSurfelGpuOutBuffer->Release(); m_pSurfelGpuOutBuffer = nullptr; }
            m_pSurfelBufferMapped = nullptr;

            ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(requiredBytes),
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&m_pSurfelBuffer)));
            SetName(m_pSurfelBuffer, "SurfelsRenderer::m_pSurfelBuffer");

            CD3DX12_RESOURCE_DESC gpuBufDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredBytes);
            gpuBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                D3D12_HEAP_FLAG_NONE,
                &gpuBufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_pSurfelGpuBuffer)));
            SetName(m_pSurfelGpuBuffer, "SurfelsRenderer::m_pSurfelGpuBuffer");

            ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                D3D12_HEAP_FLAG_NONE,
                &gpuBufDesc,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                nullptr,
                IID_PPV_ARGS(&m_pSurfelGpuOutBuffer)));
            SetName(m_pSurfelGpuOutBuffer, "SurfelsRenderer::m_pSurfelGpuOutBuffer");

            uint32_t pairBytes = numElements * sizeof(uint32_t) * 2;
            if (pairBytes > m_sortPairBufferCapacityBytes)
            {
                if (m_pGPUSortPairBuffer) { m_pGPUSortPairBuffer->Release(); m_pGPUSortPairBuffer = nullptr; }
                CD3DX12_RESOURCE_DESC pairBufDesc = CD3DX12_RESOURCE_DESC::Buffer(pairBytes);
                pairBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &pairBufDesc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    nullptr,
                    IID_PPV_ARGS(&m_pGPUSortPairBuffer)));
                SetName(m_pGPUSortPairBuffer, "SurfelsRenderer::m_pGPUSortPairBuffer");
                m_sortPairBufferCapacityBytes = pairBytes;
            }

            m_pSurfelBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pSurfelBufferMapped));
            m_surfelBufferCapacityBytes = requiredBytes;
            m_lastSurfelsPtr = nullptr;
        }

        bool modelChanged = (m_lastSurfelsPtr != pState->pStreamedSurfels) || (m_lastSurfelCount != surfelCount);
        if (modelChanged && m_pSurfelBufferMapped != nullptr)
        {
            m_lastSurfelsPtr = pState->pStreamedSurfels;
            m_lastSurfelCount = surfelCount;

            memcpy(m_pSurfelBufferMapped, pState->pStreamedSurfels, surfelCount * sizeof(Surfels::PackedSurfelGPU));
            for (uint32_t i = surfelCount; i < numElements; i++)
            {
                Surfels::PackedSurfelGPU* pDst = (Surfels::PackedSurfelGPU*)m_pSurfelBufferMapped;
                pDst[i].packedPosRadius = 0xFFFFFFFF;
                pDst[i].packedNormal = 0xFFFF;
                pDst[i].packedColor = 0xFFFF;
            }
            m_needUploadToGpu = true;
            m_gpuSortNeedsRun = true;
        }
    }
    else
    {
        void* pDummy = nullptr;
        m_constantBufferRing.AllocConstantBuffer(256, &pDummy, &m_surfelBufferGPUAddress);
    }

    XMFLOAT3 eyePos;
    XMStoreFloat3(&eyePos, eye);
    XMFLOAT3 forwardNorm;
    XMStoreFloat3(&forwardNorm, forward);

    float camMoved = std::abs(eyePos.x - m_lastSortEye.x) + std::abs(eyePos.y - m_lastSortEye.y) + std::abs(eyePos.z - m_lastSortEye.z);
    float forwardMoved = std::abs(forwardNorm.x - m_lastSortForward.x) + std::abs(forwardNorm.y - m_lastSortForward.y) + std::abs(forwardNorm.z - m_lastSortForward.z);
    if (camMoved > 0.05f || forwardMoved > 0.02f)
    {
        m_gpuSortNeedsRun = true;
        m_lastSortEye = eyePos;
        m_lastSortForward = forwardNorm;
    }

    // Copy from Upload to Default VRAM and execute Multi-Pass GPU Bitonic Depth Sort
    if (surfelCount > 0 && m_pSurfelGpuBuffer != nullptr && m_pSurfelBuffer != nullptr)
    {
        if (m_needUploadToGpu)
        {
            pCmdLst->CopyResource(m_pSurfelGpuBuffer, m_pSurfelBuffer);
            m_needUploadToGpu = false;
        }

        if (pState->gpuRadixSort && m_pProjectKeysPSO && m_pBitonicLocalSortPSO && m_pBitonicGlobalSortPSO && m_pBitonicLocalMergePSO && m_pGatherSurfelsPSO && m_pComputeRootSignature && m_pSurfelGpuOutBuffer != nullptr && m_pGPUSortPairBuffer != nullptr)
        {
            if (m_gpuSortNeedsRun)
            {
                // Transition input buffer to SRV, and output buffer to UAV
                D3D12_RESOURCE_BARRIER preBarriers[2] = {};
                preBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_pSurfelGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                preBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_pSurfelGpuOutBuffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                pCmdLst->ResourceBarrier(2, preBarriers);

            uint32_t numElements = 1;
            while (numElements < surfelCount) numElements <<= 1;

            struct BitonicCB
            {
                XMFLOAT3 camPos;
                float    pad0;
                XMFLOAT3 camForward;
                uint32_t totalSurfels;
                uint32_t level;
                uint32_t levelMask;
                uint32_t numElements;
                uint32_t renderMode;
                XMFLOAT3 aabbMin;
                float    pad1;
                XMFLOAT3 aabbExtents;
                float    pad2;
            };

            BitonicCB baseCB = {};
            baseCB.camPos = eyePos;
            baseCB.camForward = forwardNorm;
            baseCB.totalSurfels = surfelCount;
            baseCB.numElements = numElements;
            baseCB.renderMode = pState->renderMode;
            baseCB.aabbMin = pState->aabbMin;
            baseCB.aabbExtents = pState->aabbExtents;

            pCmdLst->SetComputeRootSignature(m_pComputeRootSignature);
            pCmdLst->SetComputeRootShaderResourceView(1, m_pSurfelGpuBuffer->GetGPUVirtualAddress());
            pCmdLst->SetComputeRootShaderResourceView(2, m_pSurfelGpuBuffer->GetGPUVirtualAddress());
            pCmdLst->SetComputeRootUnorderedAccessView(3, m_pGPUSortPairBuffer->GetGPUVirtualAddress());
            pCmdLst->SetComputeRootUnorderedAccessView(4, m_pSurfelGpuOutBuffer->GetGPUVirtualAddress());
            pCmdLst->SetComputeRootUnorderedAccessView(5, m_pSurfelGpuOutBuffer->GetGPUVirtualAddress());

            uint32_t localGroups = (numElements + 1023) / 1024;
            uint32_t globalGroups = (numElements + 255) / 256;

            D3D12_RESOURCE_BARRIER uavPairBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_pGPUSortPairBuffer);
            D3D12_RESOURCE_BARRIER uavOutBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_pSurfelGpuOutBuffer);

            // Stage 1: Project depth keys ONCE into key-index pairs
            {
                BitonicCB* pSortCB = nullptr;
                D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                {
                    *pSortCB = baseCB;
                    pCmdLst->SetPipelineState(m_pProjectKeysPSO);
                    pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                    pCmdLst->Dispatch(globalGroups, 1, 1);
                    pCmdLst->ResourceBarrier(1, &uavPairBarrier);
                }
            }

            // Stage 2: Local Block Sort in LDS (all stages level 2 to 1024 in 1 single dispatch!)
            {
                BitonicCB* pSortCB = nullptr;
                D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                {
                    *pSortCB = baseCB;
                    pSortCB->level = std::min(numElements, 1024u);
                    pSortCB->levelMask = pSortCB->level >> 1;

                    pCmdLst->SetPipelineState(m_pBitonicLocalSortPSO);
                    pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                    pCmdLst->Dispatch(localGroups, 1, 1);
                    pCmdLst->ResourceBarrier(1, &uavPairBarrier);
                }
            }

            // Stage 3: Outer Levels (level = 2048 up to numElements)
            for (uint32_t level = 2048; level <= numElements; level <<= 1)
            {
                // Global passes for levelMask >= 1024
                for (uint32_t levelMask = level >> 1; levelMask >= 1024; levelMask >>= 1)
                {
                    BitonicCB* pSortCB = nullptr;
                    D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                    if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                    {
                        *pSortCB = baseCB;
                        pSortCB->level = level;
                        pSortCB->levelMask = levelMask;

                        pCmdLst->SetPipelineState(m_pBitonicGlobalSortPSO);
                        pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                        pCmdLst->Dispatch(globalGroups, 1, 1);
                        pCmdLst->ResourceBarrier(1, &uavPairBarrier);
                    }
                }

                // Local LDS Merge pass for all remaining levels (512 down to 1 in 1 single dispatch!)
                {
                    BitonicCB* pSortCB = nullptr;
                    D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                    if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                    {
                        *pSortCB = baseCB;
                        pSortCB->level = level;
                        pSortCB->levelMask = 512;

                        pCmdLst->SetPipelineState(m_pBitonicLocalMergePSO);
                        pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                        pCmdLst->Dispatch(localGroups, 1, 1);
                        pCmdLst->ResourceBarrier(1, &uavPairBarrier);
                    }
                }
            }

            // Stage 4: 1-Pass Gather sorted elements into output buffer
            {
                BitonicCB* pSortCB = nullptr;
                D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                {
                    *pSortCB = baseCB;
                    pCmdLst->SetPipelineState(m_pGatherSurfelsPSO);
                    pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                    pCmdLst->Dispatch(globalGroups, 1, 1);
                    pCmdLst->ResourceBarrier(1, &uavOutBarrier);
                }
            }

            // Transition output buffer to SRV for Mesh Shader rendering
            D3D12_RESOURCE_BARRIER postBarriers[2] = {};
            postBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_pSurfelGpuOutBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_pSurfelGpuBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            pCmdLst->ResourceBarrier(2, postBarriers);

            m_gpuSortNeedsRun = false;
            }

            m_surfelBufferGPUAddress = m_pSurfelGpuOutBuffer->GetGPUVirtualAddress();
        }
        else
        {
            // Transition COPY_DEST -> ALL_SHADER_RESOURCE
            D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                m_pSurfelGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            pCmdLst->ResourceBarrier(1, &toSrv);
            m_surfelBufferGPUAddress = m_pSurfelGpuBuffer->GetGPUVirtualAddress();
        }
    }

    SurfelsCB* pCB = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS cbAddress = 0;
    m_constantBufferRing.AllocConstantBuffer(sizeof(SurfelsCB), (void**)&pCB, &cbAddress);

    // DirectXMath matrices are row-major, meant to be used as v * M. HLSL's default
    // float4x4 packing is column-major, which for an UNtransposed upload already
    // reinterprets the bytes correctly for mul(M, v) in the shader.
    XMStoreFloat4x4(&pCB->viewProj, viewProj);
    XMStoreFloat3(&pCB->camRight, right);
    pCB->radius = pState->splatRadius;
    XMStoreFloat3(&pCB->camUp, camUp);
    pCB->time = pState->time;
    pCB->sphereCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);
    pCB->sphereRadius = pState->sphereRadius;
    pCB->surfelCount = surfelCount;
    pCB->renderMode = pState->renderMode;
    pCB->orientMode = pState->orientMode;
    pCB->totalChunks = 0;
    pCB->aabbMin = pState->aabbMin;
    pCB->useChunkedPipeline = 0;
    pCB->aabbExtents = pState->aabbExtents;
    pCB->useDetachedCullCam = 0;
    XMStoreFloat4x4(&pCB->cullViewProj, viewProj);
    pCB->cullEyePos = eyePos;
    pCB->pad3 = 0.0f;

    pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
    pCmdLst->SetPipelineState(m_pPipelineState);
    pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
    pCmdLst->SetGraphicsRootShaderResourceView(1, m_surfelBufferGPUAddress);
    pCmdLst->SetGraphicsRootShaderResourceView(2, m_surfelBufferGPUAddress);
    pCmdLst->SetGraphicsRootShaderResourceView(3, m_surfelBufferGPUAddress);
    pCmdLst->SetGraphicsRootShaderResourceView(4, m_surfelBufferGPUAddress);

    // DispatchMesh needs the newer command list interface; GetNewCommandList() only
    // returns ID3D12GraphicsCommandList2.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
    pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
    uint32_t groupCount = (surfelCount + SURFELS_PER_GROUP - 1) / SURFELS_PER_GROUP;
    if (groupCount > 0)
    {
        cmdList6->DispatchMesh(groupCount, 1, 1);
    }

    if (m_pSurfelGpuBuffer != nullptr)
    {
        D3D12_RESOURCE_BARRIER toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
            m_pSurfelGpuBuffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        pCmdLst->ResourceBarrier(1, &toCopyDest);
    }

    m_gpuTimer.GetTimeStamp(pCmdLst, "Surfels Mesh Shader");

    // ImGui draws on top of whatever is currently bound (still our backbuffer RTV).
    m_imGui.Draw(pCmdLst);

    m_gpuTimer.GetTimeStamp(pCmdLst, "ImGui UI");

    // Resolve timestamp queries into readback buffer
    m_gpuTimer.CollectTimings(pCmdLst);

    {
        D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        pCmdLst->ResourceBarrier(1, &toPresent);
    }

    pCmdLst->Close();

    ID3D12CommandList* pCmdLists[] = { pCmdLst };
    m_pDevice->GetGraphicsQueue()->ExecuteCommandLists(1, pCmdLists);

    m_gpuTimer.OnEndFrame();
}
