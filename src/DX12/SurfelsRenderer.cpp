#include "stdafx.h"
#include "SurfelsRenderer.h"

using namespace CAULDRON_DX12;

static const uint32_t BACK_BUFFER_COUNT = 2;

void SurfelsRenderer::OnCreate(Device* pDevice, SwapChain* pSwapChain)
{
    m_pDevice = pDevice;

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
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);
    m_pDevice->GetDevice()->CreateRootSignature(0, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));
    SetName(m_pRootSignature, "Surfels::RootSignature");

    // Fully procedural quad-per-instance: no vertex/index buffers, geometry comes
    // from SV_VertexID (quad corner) and SV_InstanceID (surfel index) alone.
    D3D12_SHADER_BYTECODE vs = {};
    D3D12_SHADER_BYTECODE ps = {};
    CompileShaderFromFile("Surfels.hlsl", NULL, "mainVS", "-T vs_6_0", &vs);
    CompileShaderFromFile("Surfels.hlsl", NULL, "mainPS", "-T ps_6_0", &ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_pRootSignature;
    psoDesc.VS = vs;
    psoDesc.PS = ps;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = pSwapChain->GetFormat();
    psoDesc.SampleDesc.Count = 1;
    m_pDevice->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pPipelineState));
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
    pCmdLst->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pCmdLst->DrawInstanced(4, pState->surfelCount, 0, 0);

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
