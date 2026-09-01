#include "stdafx.h"
#include "SurfelsRenderer.h"

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
    CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE       RootSignature;
    CD3DX12_PIPELINE_STATE_STREAM_MS                   MS;
    CD3DX12_PIPELINE_STATE_STREAM_PS                   PS;
    CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER           RasterizerState;
    CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC           BlendState;
    CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL        DepthStencilState;
    CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
    CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC          SampleDesc;
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
    m_resourceViewHeaps.OnCreate(pDevice, 10, 10, 10, 0, 10, 10);
    m_uploadHeap.OnCreate(pDevice, 32 * 1024 * 1024);
    m_constantBufferRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 4 * 1024 * 1024, &m_resourceViewHeaps);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    // CommandListRing::GetNewCommandList() increments its used-count then asserts
    // used < commandListsPerBackBuffer, so passing 1 here fails on the very first
    // GetNewCommandList() call of every frame. We only ever call it once per frame,
    // so 2 gives exactly the headroom that check requires.
    m_commandListRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 2, queueDesc);

    m_imGui.OnCreate(pDevice, &m_uploadHeap, &m_resourceViewHeaps, &m_constantBufferRing, pSwapChain->GetFormat());

    // Root signature: a single CBV (view-proj, camera basis, surfel params).
    CD3DX12_ROOT_PARAMETER rootParams[1];
    rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // no input assembler stage with mesh shaders

    Microsoft::WRL::ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);
    m_pDevice->GetDevice()->CreateRootSignature(0, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));
    SetName(m_pRootSignature, "Surfels::RootSignature");

    // Fully procedural: no vertex/index buffers and no draw-level instancing either.
    // The mesh shader builds SURFELS_PER_GROUP surfels' worth of geometry per
    // threadgroup from SV_GroupID/SV_GroupThreadID alone; DispatchMesh() in OnRender
    // replaces DrawInstanced().
    D3D12_SHADER_BYTECODE ms = {};
    D3D12_SHADER_BYTECODE ps = {};
    CompileShaderFromFile("Surfels.hlsl", NULL, "mainMS", "-T ms_6_5", &ms);
    CompileShaderFromFile("Surfels.hlsl", NULL, "mainPS", "-T ps_6_5", &ps);

    CD3DX12_RASTERIZER_DESC rasterizer(D3D12_DEFAULT);
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;

    CD3DX12_DEPTH_STENCIL_DESC depthStencil(D3D12_DEFAULT);
    depthStencil.DepthEnable = FALSE;
    depthStencil.StencilEnable = FALSE;

    D3D12_RT_FORMAT_ARRAY rtvFormats = {};
    rtvFormats.NumRenderTargets = 1;
    rtvFormats.RTFormats[0] = pSwapChain->GetFormat();

    MeshShaderPipelineStateStream stream = {};
    stream.RootSignature = m_pRootSignature;
    stream.MS = ms;
    stream.PS = ps;
    stream.RasterizerState = rasterizer;
    stream.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    stream.DepthStencilState = depthStencil;
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
    if (m_pPipelineState) { m_pPipelineState->Release(); m_pPipelineState = nullptr; }
    if (m_pRootSignature) { m_pRootSignature->Release(); m_pRootSignature = nullptr; }

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
}

void SurfelsRenderer::OnDestroyWindowSizeDependentResources()
{
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

    m_commandListRing.OnBeginFrame();
    m_constantBufferRing.OnBeginFrame();

    ID3D12GraphicsCommandList2* pCmdLst = m_commandListRing.GetNewCommandList();

    ID3D12Resource* pBackBuffer = pSwapChain->GetCurrentBackBufferResource();
    D3D12_CPU_DESCRIPTOR_HANDLE* pRTV = pSwapChain->GetCurrentBackBufferRTV();

    {
        D3D12_RESOURCE_BARRIER toRT = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pCmdLst->ResourceBarrier(1, &toRT);
    }

    const float clearColor[4] = { 0.02f, 0.02f, 0.05f, 1.0f };
    pCmdLst->OMSetRenderTargets(1, pRTV, TRUE, nullptr);
    pCmdLst->ClearRenderTargetView(*pRTV, clearColor, 0, nullptr);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)m_width, (LONG)m_height };
    pCmdLst->RSSetViewports(1, &viewport);
    pCmdLst->RSSetScissorRects(1, &scissor);

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

    SurfelsCB* pCB = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS cbAddress = 0;
    m_constantBufferRing.AllocConstantBuffer(sizeof(SurfelsCB), (void**)&pCB, &cbAddress);

    XMStoreFloat4x4(&pCB->viewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat3(&pCB->camRight, right);
    pCB->radius = pState->splatRadius;
    XMStoreFloat3(&pCB->camUp, camUp);
    pCB->time = pState->time;
    pCB->sphereCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);
    pCB->sphereRadius = pState->sphereRadius;
    pCB->surfelCount = pState->surfelCount;
    pCB->pad = XMFLOAT3(0.0f, 0.0f, 0.0f);

    pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
    pCmdLst->SetPipelineState(m_pPipelineState);
    pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);

    // DispatchMesh needs the newer command list interface; GetNewCommandList() only
    // returns ID3D12GraphicsCommandList2.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
    pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
    uint32_t groupCount = (pState->surfelCount + SURFELS_PER_GROUP - 1) / SURFELS_PER_GROUP;
    cmdList6->DispatchMesh(groupCount, 1, 1);

    // ImGui draws on top of whatever is currently bound (still our backbuffer RTV).
    m_imGui.Draw(pCmdLst);

    {
        D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        pCmdLst->ResourceBarrier(1, &toPresent);
    }

    pCmdLst->Close();

    ID3D12CommandList* pCmdLists[] = { pCmdLst };
    m_pDevice->GetGraphicsQueue()->ExecuteCommandLists(1, pCmdLists);
}
