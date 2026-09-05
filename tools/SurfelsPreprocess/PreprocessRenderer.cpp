// PreprocessRenderer.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The GPU heart of SurfelsPreprocess: root signatures, PSOs, and per-frame command
// recording for the main mesh-shader splat pass, the GPU silhouette item-prepass, the
// interior occlusion volume pass, the GPU bitonic depth sort, and TAA resolve.

#include "PreprocessRenderer.h"
#include <d3dx12.h>
#include "Misc/Error.h"

namespace Surfels
{
    using namespace CAULDRON_DX12;

    static const uint32_t BACK_BUFFER_COUNT = 2;
    static const uint32_t SURFELS_PER_GROUP = 64;
    static const uint32_t AS_GROUP_SIZE = 32;

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

    // One-time setup: every root signature, PSO (splat/item-prepass/occluder/GPU-sort compute/TAA),
    // and the dedicated copy queue used for async surfel/chunk uploads.
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

        m_resourceViewHeaps.OnCreate(pDevice, 256, 256, 256, 64, 64, 64);
        m_resourceViewHeaps.AllocDSVDescriptor(1, &m_depthBufferDSV);
        m_resourceViewHeaps.AllocDSVDescriptor(1, &m_itemDepthDSV);
        m_resourceViewHeaps.AllocRTVDescriptor(1, &m_itemRTV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_itemSRV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_itemDepthSRV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(2, &m_itemTableSRVs);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_silhouetteBitmaskUAV);

        // Temporal Anti-Aliasing (TAA) Descriptors
        m_resourceViewHeaps.AllocRTVDescriptor(1, &m_sceneColorRTV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_sceneColorSRV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_historyColorSRV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_historyColorUAV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_resolvedColorSRV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_resolvedColorUAV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(1, &m_depthBufferSRV);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(3, &m_temporalTableSRVs);
        m_resourceViewHeaps.AllocCBV_SRV_UAVDescriptor(2, &m_temporalTableUAVs);

        m_uploadHeap.OnCreate(pDevice, 32 * 1024 * 1024);
        m_constantBufferRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 32 * 1024 * 1024, &m_resourceViewHeaps);

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        m_commandListRing.OnCreate(pDevice, BACK_BUFFER_COUNT, 2, queueDesc);

        m_imGui.OnCreate(pDevice, &m_uploadHeap, &m_resourceViewHeaps, &m_constantBufferRing, pSwapChain->GetFormat());

        CD3DX12_ROOT_PARAMETER rootParams[6];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // b0 - SurfelsCB
        rootParams[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // t0 - g_SurfelBuffer
        rootParams[2].InitAsShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_ALL); // t1 - g_RawSurfelBuffer
        rootParams[3].InitAsShaderResourceView(2, 0, D3D12_SHADER_VISIBILITY_ALL); // t2 - g_ChunkBuffer
        rootParams[4].InitAsShaderResourceView(3, 0, D3D12_SHADER_VISIBILITY_ALL); // t3 - g_SortedChunkIndices
        rootParams[5].InitAsShaderResourceView(4, 0, D3D12_SHADER_VISIBILITY_ALL); // t4 - g_OcclusionVoxelBuffer

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 6;
        rsDesc.pParameters = rootParams;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
        D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);
        m_pDevice->GetDevice()->CreateRootSignature(0, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));

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
        CreateComputePSO("ProjectChunkKeysCS", &m_pProjectChunkKeysPSO);
        CreateComputePSO("BitonicLocalSortCS", &m_pBitonicLocalSortPSO);
        CreateComputePSO("BitonicGlobalSortCS", &m_pBitonicGlobalSortPSO);
        CreateComputePSO("BitonicLocalMergeCS", &m_pBitonicLocalMergePSO);
        CreateComputePSO("GatherSurfelsCS", &m_pGatherSurfelsPSO);
        CreateComputePSO("GatherChunkIndicesCS", &m_pGatherChunkIndicesPSO);

        D3D12_SHADER_BYTECODE as = {};
        D3D12_SHADER_BYTECODE ms = {};
        D3D12_SHADER_BYTECODE ps = {};
        CompileShaderFromFile("Surfels.hlsl", NULL, "mainAS", "-T as_6_5", &as);
        CompileShaderFromFile("Surfels.hlsl", NULL, "mainMS", "-T ms_6_5", &ms);
        CompileShaderFromFile("Surfels.hlsl", NULL, "mainPS", "-T ps_6_5", &ps);

        CD3DX12_RASTERIZER_DESC rasterizer(D3D12_DEFAULT);
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;

        // 1. Fast Surfel Blending Pipeline State (Depth Testing Disabled for maximum >200 FPS fillrate)
        CD3DX12_DEPTH_STENCIL_DESC depthStencilFast(D3D12_DEFAULT);
        depthStencilFast.DepthEnable = FALSE;
        depthStencilFast.StencilEnable = FALSE;

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
        stream.DepthStencilState = depthStencilFast;
        stream.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        stream.RTVFormats = rtvFormats;
        stream.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {};
        streamDesc.SizeInBytes = sizeof(stream);
        streamDesc.pPipelineStateSubobjectStream = &stream;

        Microsoft::WRL::ComPtr<ID3D12Device2> device2;
        m_pDevice->GetDevice()->QueryInterface(IID_PPV_ARGS(&device2));
        device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&m_pPipelineState));

        // Create Item Prepass Pipeline State Object (itemMS & itemPS with DepthWrite ON, RTV: R32_UINT, DSV: D32_FLOAT)
        D3D12_SHADER_BYTECODE itemMs = {};
        D3D12_SHADER_BYTECODE itemPs = {};
        CompileShaderFromFile("Surfels.hlsl", NULL, "itemMS", "-T ms_6_5", &itemMs);
        CompileShaderFromFile("Surfels.hlsl", NULL, "itemPS", "-T ps_6_5", &itemPs);

        CD3DX12_DEPTH_STENCIL_DESC depthStencilItem(D3D12_DEFAULT);
        depthStencilItem.DepthEnable = TRUE;
        depthStencilItem.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthStencilItem.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        D3D12_RT_FORMAT_ARRAY itemRtvFormats = {};
        itemRtvFormats.NumRenderTargets = 1;
        itemRtvFormats.RTFormats[0] = DXGI_FORMAT_R32_UINT;

        CD3DX12_BLEND_DESC itemBlendDesc(D3D12_DEFAULT); // Blend disabled, overwrite chunk ID

        MeshShaderPipelineStateStream itemStream = {};
        itemStream.RootSignature = m_pRootSignature;
        itemStream.AS = {}; // Direct 1:1 chunk to mesh threadgroup dispatch
        itemStream.MS = itemMs;
        itemStream.PS = itemPs;
        itemStream.RasterizerState = rasterizer;
        itemStream.BlendState = itemBlendDesc;
        itemStream.DepthStencilState = depthStencilItem;
        itemStream.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        itemStream.RTVFormats = itemRtvFormats;
        itemStream.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };

        D3D12_PIPELINE_STATE_STREAM_DESC itemStreamDesc = {};
        itemStreamDesc.SizeInBytes = sizeof(itemStream);
        itemStreamDesc.pPipelineStateSubobjectStream = &itemStream;
        device2->CreatePipelineState(&itemStreamDesc, IID_PPV_ARGS(&m_pItemPrepassPSO));

        // Create Occluder Pipeline State Object (occluderMS & occluderPS): solid depth-writing interior
        // occlusion volume cubes, drawn into the main color+depth target before the splat pass. Same
        // direct-dispatch (no AS stage) shape as the item prepass above.
        D3D12_SHADER_BYTECODE occluderMs = {};
        D3D12_SHADER_BYTECODE occluderPs = {};
        bool occluderShadersOk = CompileShaderFromFile("Surfels.hlsl", NULL, "occluderMS", "-T ms_6_5", &occluderMs) && occluderMs.pShaderBytecode != nullptr;
        occluderShadersOk = (CompileShaderFromFile("Surfels.hlsl", NULL, "occluderPS", "-T ps_6_5", &occluderPs) && occluderPs.pShaderBytecode != nullptr) && occluderShadersOk;
        if (!occluderShadersOk)
        {
            LogTransitionTrace("PreprocessRenderer::OnCreate ERROR: Failed to compile occluderMS/occluderPS -- occlusion volume feature will be a silent no-op.");
        }

        MeshShaderPipelineStateStream occluderStream = {};
        occluderStream.RootSignature = m_pRootSignature;
        occluderStream.AS = {}; // Direct 1:1 voxel to mesh threadgroup dispatch
        occluderStream.MS = occluderMs;
        occluderStream.PS = occluderPs;
        occluderStream.RasterizerState = rasterizer;
        occluderStream.BlendState = itemBlendDesc; // Opaque, no blending
        occluderStream.DepthStencilState = depthStencilItem; // DepthEnable, WriteMask=ALL, Func=LESS
        occluderStream.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        occluderStream.RTVFormats = rtvFormats; // Draws straight into the main color target
        occluderStream.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };

        D3D12_PIPELINE_STATE_STREAM_DESC occluderStreamDesc = {};
        occluderStreamDesc.SizeInBytes = sizeof(occluderStream);
        occluderStreamDesc.pPipelineStateSubobjectStream = &occluderStream;
        HRESULT hrOccluderPSO = device2->CreatePipelineState(&occluderStreamDesc, IID_PPV_ARGS(&m_pOccluderPSO));
        if (FAILED(hrOccluderPSO))
        {
            LogTransitionTrace("PreprocessRenderer::OnCreate ERROR: CreatePipelineState(occluder) failed hr=0x%08X -- occlusion volume feature will be a silent no-op.", (unsigned int)hrOccluderPSO);
        }

        // Depth-test-only variant of the main splat pipeline: same mainAS/mainMS/mainPS as m_pPipelineState,
        // but tests (does not write) depth so splats behind the occluder volume above get discarded, while
        // the existing CPU/GPU back-to-front sort is still what keeps surviving overlapping splats blended
        // in the correct order (see the "Known gaps" note in README.md on why the fast path disables depth
        // test/write entirely).
        CD3DX12_DEPTH_STENCIL_DESC depthStencilOcclusionTest(D3D12_DEFAULT);
        depthStencilOcclusionTest.DepthEnable = TRUE;
        depthStencilOcclusionTest.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depthStencilOcclusionTest.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        depthStencilOcclusionTest.StencilEnable = FALSE;

        MeshShaderPipelineStateStream occlusionTestStream = stream; // Copy: same AS/MS/PS/blend/rasterizer as the main pass
        occlusionTestStream.DepthStencilState = depthStencilOcclusionTest;

        D3D12_PIPELINE_STATE_STREAM_DESC occlusionTestStreamDesc = {};
        occlusionTestStreamDesc.SizeInBytes = sizeof(occlusionTestStream);
        occlusionTestStreamDesc.pPipelineStateSubobjectStream = &occlusionTestStream;
        HRESULT hrOcclusionTestPSO = device2->CreatePipelineState(&occlusionTestStreamDesc, IID_PPV_ARGS(&m_pPipelineStateOcclusionTest));
        if (FAILED(hrOcclusionTestPSO))
        {
            LogTransitionTrace("PreprocessRenderer::OnCreate ERROR: CreatePipelineState(occlusionTest) failed hr=0x%08X -- occlusion culling will silently do nothing on the main splat pass.", (unsigned int)hrOcclusionTestPSO);
        }

        // Create Silhouette Edge Extraction and Clear Compute Shaders
        m_clearBitmaskCS.OnCreate(pDevice, &m_resourceViewHeaps, "SilhouetteEdgeExtractCS.hlsl", "clearBitmaskCS", 1, 0, 0, 0, 0);
        m_silhouetteEdgeExtractCS.OnCreate(pDevice, &m_resourceViewHeaps, "SilhouetteEdgeExtractCS.hlsl", "mainCS", 1, 2, 0, 0, 0);

        // Create ID3D12CommandSignature for ExecuteIndirect Mesh Shader Dispatches
        D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;

        D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
        sigDesc.ByteStride = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS); // 12 bytes
        sigDesc.NumArgumentDescs = 1;
        sigDesc.pArgumentDescs = &argDesc;

        m_pDevice->GetDevice()->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&m_pCommandSignature));

        // Create Dedicated DX12 Hardware DMA Copy Queue for asynchronous PCIe transfers
        D3D12_COMMAND_QUEUE_DESC copyQueueDesc = {};
        copyQueueDesc.Type     = D3D12_COMMAND_LIST_TYPE_COPY;
        copyQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        copyQueueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

        if (SUCCEEDED(m_pDevice->GetDevice()->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&m_pCopyQueue))))
        {
            SetName(m_pCopyQueue, "PreprocessRenderer::m_pCopyQueue");
            m_pDevice->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_pCopyAllocator));
            SetName(m_pCopyAllocator, "PreprocessRenderer::m_pCopyAllocator");
            m_pDevice->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_pCopyAllocator, nullptr, IID_PPV_ARGS(&m_pCopyCmdList));
            SetName(m_pCopyCmdList, "PreprocessRenderer::m_pCopyCmdList");
            m_pCopyCmdList->Close();

            m_pDevice->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pCopyFence));
            SetName(m_pCopyFence, "PreprocessRenderer::m_pCopyFence");
            m_copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            m_copyFenceValue = 0;
        }

        // Create Temporal Anti-Aliasing (TAA) Root Signature & Compute Pipeline State
        {
            CD3DX12_DESCRIPTOR_RANGE srvTableRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0); // t0: curColor, t1: curDepth, t2: historyColor
            CD3DX12_DESCRIPTOR_RANGE uavTableRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0); // u0: outResolved, u1: outHistory

            CD3DX12_ROOT_PARAMETER temporalRootParams[3];
            temporalRootParams[0].InitAsConstantBufferView(0, 0); // b0 - TemporalCB
            temporalRootParams[1].InitAsDescriptorTable(1, &srvTableRange);
            temporalRootParams[2].InitAsDescriptorTable(1, &uavTableRange);

            D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
            // s0: Linear Clamp
            samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[0].ShaderRegister = 0;
            samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            // s1: Point Clamp
            samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[1].ShaderRegister = 1;
            samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            CD3DX12_ROOT_SIGNATURE_DESC tempRsDesc = {};
            tempRsDesc.NumParameters = 3;
            tempRsDesc.pParameters = temporalRootParams;
            tempRsDesc.NumStaticSamplers = 2;
            tempRsDesc.pStaticSamplers = samplers;
            tempRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            Microsoft::WRL::ComPtr<ID3DBlob> pTempOutBlob, pTempErrBlob;
            D3D12SerializeRootSignature(&tempRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pTempOutBlob, &pTempErrBlob);
            m_pDevice->GetDevice()->CreateRootSignature(0, pTempOutBlob->GetBufferPointer(), pTempOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pTemporalRootSig));

            D3D12_SHADER_BYTECODE tempCs = {};
            if (CompileShaderFromFile("TemporalFilterCS.hlsl", NULL, "main", "-T cs_6_0", &tempCs) && tempCs.pShaderBytecode != nullptr)
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc = {};
                computeDesc.pRootSignature = m_pTemporalRootSig;
                computeDesc.CS = tempCs;
                m_pDevice->GetDevice()->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(&m_pTemporalPSO));
            }
        }
    }

    void PreprocessRenderer::FlushCopyQueue()
    {
        if (!m_pCopyQueue || !m_pCopyFence) return;

        uint64_t fv = ++m_copyFenceValue;
        m_pCopyQueue->Signal(m_pCopyFence, fv);
        if (m_pCopyFence->GetCompletedValue() < fv)
        {
            m_pCopyFence->SetEventOnCompletion(fv, m_copyFenceEvent);
            WaitForSingleObject(m_copyFenceEvent, INFINITE);
        }
    }

    // Occlusion voxel data only changes when a new dataset is loaded/exported, unlike the surfel/chunk
    // buffers which churn every frame -- so unlike those, this is a plain upload-heap resource read
    // directly as an SRV rather than a default-heap buffer kept current via the copy queue.
    void PreprocessRenderer::UpdateOcclusionVoxelBuffer(const State* pState)
    {
        if (pState->pOcclusionVoxels == m_lastOcclusionVoxelsPtr && pState->occlusionVoxelCount == m_lastOcclusionVoxelCount)
            return;

        m_lastOcclusionVoxelsPtr = pState->pOcclusionVoxels;
        m_lastOcclusionVoxelCount = pState->occlusionVoxelCount;

        if (pState->occlusionVoxelCount == 0 || pState->pOcclusionVoxels == nullptr)
        {
            if (m_pOcclusionVoxelBuffer)
            {
                m_pDevice->GPUFlush();
                m_pOcclusionVoxelBuffer->Unmap(0, nullptr);
                m_pOcclusionVoxelBuffer->Release();
                m_pOcclusionVoxelBuffer = nullptr;
            }
            m_pOcclusionVoxelBufferMapped = nullptr;
            m_occlusionVoxelBufferCapacityBytes = 0;
            return;
        }

        uint32_t neededBytes = pState->occlusionVoxelCount * (uint32_t)sizeof(OcclusionVoxelGPU);
        if (neededBytes > m_occlusionVoxelBufferCapacityBytes)
        {
            m_pDevice->GPUFlush();
            if (m_pOcclusionVoxelBuffer)
            {
                m_pOcclusionVoxelBuffer->Unmap(0, nullptr);
                m_pOcclusionVoxelBuffer->Release();
                m_pOcclusionVoxelBuffer = nullptr;
            }

            CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
            CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(neededBytes);
            m_pDevice->GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pOcclusionVoxelBuffer));
            SetName(m_pOcclusionVoxelBuffer, "PreprocessRenderer::m_pOcclusionVoxelBuffer");
            m_pOcclusionVoxelBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pOcclusionVoxelBufferMapped));
            m_occlusionVoxelBufferCapacityBytes = neededBytes;
        }

        if (m_pOcclusionVoxelBufferMapped)
        {
            memcpy(m_pOcclusionVoxelBufferMapped, pState->pOcclusionVoxels, neededBytes);
        }
    }

    // Releases every GPU resource/PSO/buffer created in OnCreate
    void PreprocessRenderer::OnDestroy()
    {
        FlushCopyQueue();
        if (m_copyFenceEvent) { CloseHandle(m_copyFenceEvent); m_copyFenceEvent = nullptr; }
        if (m_pCopyFence) { m_pCopyFence->Release(); m_pCopyFence = nullptr; }
        if (m_pCopyCmdList) { m_pCopyCmdList->Release(); m_pCopyCmdList = nullptr; }
        if (m_pCopyAllocator) { m_pCopyAllocator->Release(); m_pCopyAllocator = nullptr; }
        if (m_pCopyQueue) { m_pCopyQueue->Release(); m_pCopyQueue = nullptr; }
        if (m_pSurfelBuffer) { m_pSurfelBuffer->Unmap(0, nullptr); m_pSurfelBuffer->Release(); m_pSurfelBuffer = nullptr; }
        if (m_pSurfelGpuBuffer) { m_pSurfelGpuBuffer->Release(); m_pSurfelGpuBuffer = nullptr; }
        if (m_pSurfelGpuOutBuffer) { m_pSurfelGpuOutBuffer->Release(); m_pSurfelGpuOutBuffer = nullptr; }
        m_pSurfelBufferMapped = nullptr;
        m_surfelBufferCapacityBytes = 0;
        m_surfelBufferGPUAddress = 0;

        if (m_pRawSurfelBuffer) { m_pRawSurfelBuffer->Unmap(0, nullptr); m_pRawSurfelBuffer->Release(); m_pRawSurfelBuffer = nullptr; }
        if (m_pRawSurfelGpuBuffer) { m_pRawSurfelGpuBuffer->Release(); m_pRawSurfelGpuBuffer = nullptr; }
        if (m_pRawSurfelGpuOutBuffer) { m_pRawSurfelGpuOutBuffer->Release(); m_pRawSurfelGpuOutBuffer = nullptr; }
        m_pRawSurfelBufferMapped = nullptr;
        m_rawSurfelBufferCapacityBytes = 0;
        m_rawSurfelBufferGPUAddress = 0;

        if (m_pGPUSortPairBuffer) { m_pGPUSortPairBuffer->Release(); m_pGPUSortPairBuffer = nullptr; }
        m_sortPairBufferCapacityBytes = 0;

        if (m_pChunkUploadBuffer) { m_pChunkUploadBuffer->Unmap(0, nullptr); m_pChunkUploadBuffer->Release(); m_pChunkUploadBuffer = nullptr; }
        m_pChunkUploadBufferMapped = nullptr;
        if (m_pChunkGpuBuffer) { m_pChunkGpuBuffer->Release(); m_pChunkGpuBuffer = nullptr; }
        m_chunkBufferCapacityBytes = 0;
        if (m_pSortedChunkIndicesGpuBuffer) { m_pSortedChunkIndicesGpuBuffer->Release(); m_pSortedChunkIndicesGpuBuffer = nullptr; }
        m_sortedChunkIndicesCapacityBytes = 0;

        m_lastSurfelsPtr = nullptr;
        m_lastSurfelCount = 0;
        m_lastRenderMode = 0;

        if (m_pCommandSignature) { m_pCommandSignature->Release(); m_pCommandSignature = nullptr; }
        if (m_pProjectKeysPSO) { m_pProjectKeysPSO->Release(); m_pProjectKeysPSO = nullptr; }
        if (m_pProjectChunkKeysPSO) { m_pProjectChunkKeysPSO->Release(); m_pProjectChunkKeysPSO = nullptr; }
        if (m_pBitonicLocalSortPSO) { m_pBitonicLocalSortPSO->Release(); m_pBitonicLocalSortPSO = nullptr; }
        if (m_pBitonicGlobalSortPSO) { m_pBitonicGlobalSortPSO->Release(); m_pBitonicGlobalSortPSO = nullptr; }
        if (m_pBitonicLocalMergePSO) { m_pBitonicLocalMergePSO->Release(); m_pBitonicLocalMergePSO = nullptr; }
        if (m_pGatherSurfelsPSO) { m_pGatherSurfelsPSO->Release(); m_pGatherSurfelsPSO = nullptr; }
        if (m_pGatherChunkIndicesPSO) { m_pGatherChunkIndicesPSO->Release(); m_pGatherChunkIndicesPSO = nullptr; }
        if (m_pRadixSortPSO) { m_pRadixSortPSO->Release(); m_pRadixSortPSO = nullptr; }
        if (m_pCullPSO) { m_pCullPSO->Release(); m_pCullPSO = nullptr; }
        if (m_pComputeRootSignature) { m_pComputeRootSignature->Release(); m_pComputeRootSignature = nullptr; }
        if (m_pPipelineState) { m_pPipelineState->Release(); m_pPipelineState = nullptr; }
        if (m_pPipelineStateOcclusionTest) { m_pPipelineStateOcclusionTest->Release(); m_pPipelineStateOcclusionTest = nullptr; }
        if (m_pOccluderPSO) { m_pOccluderPSO->Release(); m_pOccluderPSO = nullptr; }
        if (m_pItemPrepassPSO) { m_pItemPrepassPSO->Release(); m_pItemPrepassPSO = nullptr; }
        if (m_pOcclusionVoxelBuffer) { m_pOcclusionVoxelBuffer->Unmap(0, nullptr); m_pOcclusionVoxelBuffer->Release(); m_pOcclusionVoxelBuffer = nullptr; }
        m_pOcclusionVoxelBufferMapped = nullptr;
        m_occlusionVoxelBufferCapacityBytes = 0;
        m_lastOcclusionVoxelsPtr = nullptr;
        m_lastOcclusionVoxelCount = 0;
        m_clearBitmaskCS.OnDestroy();
        m_silhouetteEdgeExtractCS.OnDestroy();
        if (m_pSilhouetteBitmaskGpuBuffer) { m_pSilhouetteBitmaskGpuBuffer->Release(); m_pSilhouetteBitmaskGpuBuffer = nullptr; }
        if (m_pSilhouetteReadbackBuffer) { m_pSilhouetteReadbackBuffer->Release(); m_pSilhouetteReadbackBuffer = nullptr; }
        m_bitmaskCapacityBytes = 0;
        if (m_pRootSignature) { m_pRootSignature->Release(); m_pRootSignature = nullptr; }

        m_depthBuffer.OnDestroy();
        m_sceneColorBuffer.OnDestroy();
        m_historyColorBuffer.OnDestroy();
        m_resolvedColorBuffer.OnDestroy();
        if (m_pTemporalPSO) { m_pTemporalPSO->Release(); m_pTemporalPSO = nullptr; }
        if (m_pTemporalRootSig) { m_pTemporalRootSig->Release(); m_pTemporalRootSig = nullptr; }
        m_imGui.OnDestroy();
        m_commandListRing.OnDestroy();
        m_constantBufferRing.OnDestroy();
        m_uploadHeap.OnDestroy();
        m_resourceViewHeaps.OnDestroy();
    }

    // Keeps the GPU surfel/chunk/sorted-index buffers current with pState, growing and re-uploading
    // them only when the source data pointer, count, or camera-relative sort key actually changed.
    void PreprocessRenderer::UpdateSurfelBuffers(
        const State* pState,
        XMFLOAT3 eyePos,
        XMFLOAT3 forward)
    {
        if (pState == nullptr) return;

        const PackedSurfelGPU* pSurfels = pState->pSurfels;
        const SurfelVertex* pRawSurfels = pState->pRawSurfels;
        uint32_t surfelCount = pState->surfelCount;
        uint32_t renderMode = pState->renderMode;

        const void* activePtr = (renderMode == 2) ? (const void*)pRawSurfels : (const void*)pSurfels;

        if (activePtr == nullptr || surfelCount == 0)
        {
            m_surfelBufferGPUAddress = 0;
            m_rawSurfelBufferGPUAddress = 0;
            return;
        }

        bool pipelineModeChanged = (pState->gpuRadixSort != m_lastGpuRadixSort) || (pState->useChunkedPipeline != m_lastUseChunkedPipeline);
        if (pipelineModeChanged)
        {
            m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
            m_needUploadToGpu = true;
            m_needUploadChunkIndicesToGpu = true;
            m_gpuSortNeedsRun = true;
        }
        m_lastGpuRadixSort = pState->gpuRadixSort;
        m_lastUseChunkedPipeline = pState->useChunkedPipeline;

        bool modelChanged = (m_lastSurfelsPtr != activePtr) || (m_lastSurfelCount != surfelCount) || (m_lastRenderMode != renderMode) || pipelineModeChanged;
        float camMoved = std::abs(eyePos.x - m_lastSortEye.x) + std::abs(eyePos.y - m_lastSortEye.y) + std::abs(eyePos.z - m_lastSortEye.z);
        float forwardMoved = std::abs(forward.x - m_lastSortForward.x) + std::abs(forward.y - m_lastSortForward.y) + std::abs(forward.z - m_lastSortForward.z);
        bool needsSort = modelChanged || (camMoved > 1e-4f) || (forwardMoved > 1e-4f);

        m_lastSurfelsPtr = activePtr;
        m_lastSurfelCount = surfelCount;
        m_lastRenderMode = renderMode;

        if (renderMode == 2)
        {
            // Find next power of 2 for GPU Bitonic network
            uint32_t numElements = 1;
            while (numElements < surfelCount) numElements <<= 1;
            uint32_t requiredBytes = numElements * sizeof(SurfelVertex);

            if (requiredBytes > m_rawSurfelBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                FlushCopyQueue(); // m_pCopyQueue may still be reading m_pRawSurfelBuffer via CopyResource; Device::GPUFlush doesn't know about this queue

                if (m_pRawSurfelBuffer) { m_pRawSurfelBuffer->Unmap(0, nullptr); m_pRawSurfelBuffer->Release(); m_pRawSurfelBuffer = nullptr; }
                if (m_pRawSurfelGpuBuffer) { m_pRawSurfelGpuBuffer->Release(); m_pRawSurfelGpuBuffer = nullptr; }
                if (m_pRawSurfelGpuOutBuffer) { m_pRawSurfelGpuOutBuffer->Release(); m_pRawSurfelGpuOutBuffer = nullptr; }
                m_pRawSurfelBufferMapped = nullptr;

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                    D3D12_HEAP_FLAG_NONE,
                    &CD3DX12_RESOURCE_DESC::Buffer(requiredBytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&m_pRawSurfelBuffer)));
                SetName(m_pRawSurfelBuffer, "PreprocessRenderer::m_pRawSurfelBuffer");

                CD3DX12_RESOURCE_DESC gpuBufDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredBytes);
                gpuBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_pRawSurfelGpuBuffer)));
                SetName(m_pRawSurfelGpuBuffer, "PreprocessRenderer::m_pRawSurfelGpuBuffer");

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                    nullptr,
                    IID_PPV_ARGS(&m_pRawSurfelGpuOutBuffer)));
                SetName(m_pRawSurfelGpuOutBuffer, "PreprocessRenderer::m_pRawSurfelGpuOutBuffer");

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
                    SetName(m_pGPUSortPairBuffer, "PreprocessRenderer::m_pGPUSortPairBuffer");
                    m_sortPairBufferCapacityBytes = pairBytes;
                }

                m_pRawSurfelBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pRawSurfelBufferMapped));
                m_rawSurfelBufferCapacityBytes = requiredBytes;
                modelChanged = true;
                needsSort = true;
            }

            if (surfelCount > 0 && pRawSurfels != nullptr && m_pRawSurfelBufferMapped != nullptr)
            {
                SurfelVertex* pDst = reinterpret_cast<SurfelVertex*>(m_pRawSurfelBufferMapped);

                if (pState->useChunkedPipeline)
                {
                    // Micro-chunked pipeline: keep points partitioned in chunk contiguous ranges
                    if (pState->gpuRadixSort)
                    {
                        m_metrics.isGPUSortActive = true;
                        m_metrics.cpuSortTimeMs = 0.0f;
                        if (needsSort)
                        {
                            m_gpuSortNeedsRun = true;
                        }

                        if (modelChanged)
                        {
                            memcpy(pDst, pRawSurfels, surfelCount * sizeof(SurfelVertex));
                            if (numElements > surfelCount)
                            {
                                memset(pDst + surfelCount, 0, (numElements - surfelCount) * sizeof(SurfelVertex));
                            }
                            m_needUploadToGpu = true;
                        }
                    }
                    if (modelChanged)
                    {
                        memcpy(pDst, pRawSurfels, surfelCount * sizeof(SurfelVertex));
                        if (numElements > surfelCount)
                        {
                            memset(pDst + surfelCount, 0, (numElements - surfelCount) * sizeof(SurfelVertex));
                        }
                        m_needUploadToGpu = true;
                    }
                    m_metrics.isGPUSortActive = false;
                    m_metrics.cpuSortTimeMs = 0.0f;
                    m_metrics.gpuSortTimeMs = 0.0f;
                }
                else
                {
                    // Flat pipeline mode: global sort of all surfels
                    if (pState->gpuRadixSort)
                    {
                        m_metrics.isGPUSortActive = true;
                        m_metrics.cpuSortTimeMs = 0.0f;
                        m_gpuSortNeedsRun = true;

                        if (modelChanged)
                        {
                            memcpy(pDst, pRawSurfels, surfelCount * sizeof(SurfelVertex));
                            if (numElements > surfelCount)
                            {
                                memset(pDst + surfelCount, 0, (numElements - surfelCount) * sizeof(SurfelVertex));
                            }
                            m_needUploadToGpu = true;
                        }

                        m_lastSortEye = eyePos;
                        m_lastSortForward = forward;
                    }
                    else
                    {
                        m_metrics.isGPUSortActive = false;
                        m_metrics.wasSortedThisFrame = true;
                        auto sortStart = std::chrono::high_resolution_clock::now();

                        m_lastSortEye = eyePos;
                        m_lastSortForward = forward;

                        if (m_sortIndicesA.size() < surfelCount)
                        {
                            m_sortIndicesA.resize(surfelCount);
                            m_sortIndicesB.resize(surfelCount);
                            m_sortKeys.resize(surfelCount);
                            m_sortDists.resize(surfelCount);
                        }

                        // 1. Parallel Projected Distance Calculation
                        float minD = 1e9f, maxD = -1e9f;
                        #pragma omp parallel for reduction(min:minD) reduction(max:maxD)
                        for (int i = 0; i < (int)surfelCount; i++)
                        {
                            const auto& pos = pRawSurfels[i].position;
                            float d = (pos.x - eyePos.x) * forward.x + (pos.y - eyePos.y) * forward.y + (pos.z - eyePos.z) * forward.z;
                            m_sortDists[i] = d;
                            if (d < minD) minD = d;
                            if (d > maxD) maxD = d;
                        }

                        float range = std::max(0.001f, maxD - minD);
                        float scale = 65535.0f / range;

                        // 2. Parallel 16-bit key computation
                        #pragma omp parallel for
                        for (int i = 0; i < (int)surfelCount; i++)
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

                        // 5. Parallel Gather sorted surfels into mapped GPU upload buffer
                        #pragma omp parallel for
                        for (int i = 0; i < (int)surfelCount; i++)
                        {
                            pDst[i] = pRawSurfels[m_sortIndicesA[i]];
                        }
                        for (uint32_t i = surfelCount; i < numElements; i++)
                        {
                            pDst[i] = {};
                        }

                        m_needUploadToGpu = true;

                        auto sortEnd = std::chrono::high_resolution_clock::now();
                        m_metrics.cpuSortTimeMs = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();
                        m_metrics.gpuSortTimeMs = 0.0f;
                    }
                }
            }

            m_rawSurfelBufferGPUAddress = m_pRawSurfelBuffer->GetGPUVirtualAddress();
        }
        else
        {
            // Mode 1: Quantized 8-byte Surfels / PackedSurfelGPU
            uint32_t numElements = 1;
            while (numElements < surfelCount) numElements <<= 1;
            uint32_t requiredBytes = numElements * sizeof(PackedSurfelGPU);

            if (requiredBytes > m_surfelBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                FlushCopyQueue(); // m_pCopyQueue may still be reading m_pSurfelBuffer via CopyResource; Device::GPUFlush doesn't know about this queue

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
                SetName(m_pSurfelBuffer, "PreprocessRenderer::m_pSurfelBuffer");

                CD3DX12_RESOURCE_DESC gpuBufDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredBytes);
                gpuBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_pSurfelGpuBuffer)));
                SetName(m_pSurfelGpuBuffer, "PreprocessRenderer::m_pSurfelGpuBuffer");

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                    nullptr,
                    IID_PPV_ARGS(&m_pSurfelGpuOutBuffer)));
                SetName(m_pSurfelGpuOutBuffer, "PreprocessRenderer::m_pSurfelGpuOutBuffer");

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
                    SetName(m_pGPUSortPairBuffer, "PreprocessRenderer::m_pGPUSortPairBuffer");
                    m_sortPairBufferCapacityBytes = pairBytes;
                }

                m_pSurfelBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pSurfelBufferMapped));
                m_surfelBufferCapacityBytes = requiredBytes;
                modelChanged = true;
                needsSort = true;
            }

            if (surfelCount > 0 && pSurfels != nullptr && m_pSurfelBufferMapped != nullptr)
            {
                PackedSurfelGPU* pDst = reinterpret_cast<PackedSurfelGPU*>(m_pSurfelBufferMapped);

                if (pState->useChunkedPipeline)
                {
                    // Micro-chunked pipeline: keep points partitioned in chunk contiguous ranges
                    if (pState->gpuRadixSort)
                    {
                        m_metrics.isGPUSortActive = true;
                        m_metrics.cpuSortTimeMs = 0.0f;
                        if (needsSort)
                        {
                            m_gpuSortNeedsRun = true;
                        }

                        if (modelChanged)
                        {
                            memcpy(pDst, pSurfels, surfelCount * sizeof(PackedSurfelGPU));
                            if (numElements > surfelCount)
                            {
                                memset(pDst + surfelCount, 0xFF, (numElements - surfelCount) * sizeof(PackedSurfelGPU));
                            }
                            m_needUploadToGpu = true;
                        }
                    }
                    if (modelChanged)
                    {
                        memcpy(pDst, pSurfels, surfelCount * sizeof(PackedSurfelGPU));
                        if (numElements > surfelCount)
                        {
                            memset(pDst + surfelCount, 0xFF, (numElements - surfelCount) * sizeof(PackedSurfelGPU));
                        }
                        m_needUploadToGpu = true;
                    }
                    m_metrics.isGPUSortActive = false;
                    m_metrics.cpuSortTimeMs = 0.0f;
                    m_metrics.gpuSortTimeMs = 0.0f;
                }
                else
                {
                    // Flat pipeline mode: global sort of all surfels
                    if (pState->gpuRadixSort)
                    {
                        m_metrics.isGPUSortActive = true;
                        m_metrics.cpuSortTimeMs = 0.0f;
                        m_gpuSortNeedsRun = true;

                        if (modelChanged)
                        {
                            memcpy(pDst, pSurfels, surfelCount * sizeof(PackedSurfelGPU));
                            if (numElements > surfelCount)
                            {
                                memset(pDst + surfelCount, 0xFF, (numElements - surfelCount) * sizeof(PackedSurfelGPU));
                            }
                            m_needUploadToGpu = true;
                        }

                        m_lastSortEye = eyePos;
                        m_lastSortForward = forward;
                    }
                    else
                    {
                        m_metrics.isGPUSortActive = false;
                        m_metrics.wasSortedThisFrame = true;
                        auto sortStart = std::chrono::high_resolution_clock::now();

                        m_lastSortEye = eyePos;
                        m_lastSortForward = forward;

                        if (m_sortIndicesA.size() < surfelCount)
                        {
                            m_sortIndicesA.resize(surfelCount);
                            m_sortIndicesB.resize(surfelCount);
                            m_sortKeys.resize(surfelCount);
                            m_sortDists.resize(surfelCount);
                        }

                        // 1. Calculate projected distance along camera forward vector for packed surfels
                        float minD = 1e9f, maxD = -1e9f;
                        XMFLOAT3 aabbExtents(
                            pState->aabbExtents.x,
                            pState->aabbExtents.y,
                            pState->aabbExtents.z
                        );

                        #pragma omp parallel for reduction(min:minD) reduction(max:maxD)
                        for (int i = 0; i < (int)surfelCount; i++)
                        {
                            const auto& p = pSurfels[i];
                            uint32_t qx = p.packedPosRadius & 0x3FF;
                            uint32_t qy = (p.packedPosRadius >> 10) & 0x3FF;
                            uint32_t qz = (p.packedPosRadius >> 20) & 0x3FF;

                            float px = pState->aabbMin.x + (qx / 1023.0f) * aabbExtents.x;
                            float py = pState->aabbMin.y + (qy / 1023.0f) * aabbExtents.y;
                            float pz = pState->aabbMin.z + (qz / 1023.0f) * aabbExtents.z;

                            float d = (px - eyePos.x) * forward.x + (py - eyePos.y) * forward.y + (pz - eyePos.z) * forward.z;
                            m_sortDists[i] = d;
                            if (d < minD) minD = d;
                            if (d > maxD) maxD = d;
                        }

                        float range = std::max(0.001f, maxD - minD);
                        float scale = 65535.0f / range;

                        // 2. Compute 16-bit keys (inverted for back-to-front descending order)
                        #pragma omp parallel for
                        for (int i = 0; i < (int)surfelCount; i++)
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

                        // 5. Gather sorted packed surfels directly into mapped GPU upload buffer
                        #pragma omp parallel for
                        for (int i = 0; i < (int)surfelCount; i++)
                        {
                            pDst[i] = pSurfels[m_sortIndicesA[i]];
                        }
                        for (uint32_t i = surfelCount; i < numElements; i++)
                        {
                            pDst[i].packedPosRadius = 0xFFFFFFFF;
                            pDst[i].packedNormal = 0xFFFF;
                            pDst[i].packedColor = 0xFFFF;
                        }

                        m_needUploadToGpu = true;

                        auto sortEnd = std::chrono::high_resolution_clock::now();
                        m_metrics.cpuSortTimeMs = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();
                        m_metrics.gpuSortTimeMs = 0.0f;
                    }
                }
            }

            m_surfelBufferGPUAddress = m_pSurfelBuffer->GetGPUVirtualAddress();
        }

        // Manage Meshlet Chunk buffers
        if (pState->chunkCount > 0 && pState->pChunks != nullptr)
        {
            uint32_t chunkCount = pState->chunkCount;
            uint32_t numChunkElements = 1;
            while (numChunkElements < chunkCount) numChunkElements <<= 1;
            if (numChunkElements < 131072) numChunkElements = 131072;

            uint32_t chunkBytes = numChunkElements * sizeof(MeshletChunkGPU);
            if (chunkBytes > m_chunkBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                FlushCopyQueue(); // m_pCopyQueue may still be reading m_pChunkUploadBuffer via CopyResource; Device::GPUFlush doesn't know about this queue

                if (m_pChunkUploadBuffer) { m_pChunkUploadBuffer->Unmap(0, nullptr); m_pChunkUploadBuffer->Release(); m_pChunkUploadBuffer = nullptr; }
                if (m_pChunkGpuBuffer) { m_pChunkGpuBuffer->Release(); m_pChunkGpuBuffer = nullptr; }
                m_pChunkUploadBufferMapped = nullptr;

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                    D3D12_HEAP_FLAG_NONE,
                    &CD3DX12_RESOURCE_DESC::Buffer(chunkBytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&m_pChunkUploadBuffer)));
                SetName(m_pChunkUploadBuffer, "PreprocessRenderer::m_pChunkUploadBuffer");

                CD3DX12_RESOURCE_DESC chunkGpuDesc = CD3DX12_RESOURCE_DESC::Buffer(chunkBytes);
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &chunkGpuDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_pChunkGpuBuffer)));
                SetName(m_pChunkGpuBuffer, "PreprocessRenderer::m_pChunkGpuBuffer");
                m_chunkGpuBufferState = D3D12_RESOURCE_STATE_COPY_DEST;

                uint32_t idxBytes = numChunkElements * sizeof(uint32_t);
                if (m_pSortedChunkIndicesGpuBuffer) { m_pSortedChunkIndicesGpuBuffer->Release(); m_pSortedChunkIndicesGpuBuffer = nullptr; }
                CD3DX12_RESOURCE_DESC idxBufDesc = CD3DX12_RESOURCE_DESC::Buffer(idxBytes);
                idxBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &idxBufDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_pSortedChunkIndicesGpuBuffer)));
                SetName(m_pSortedChunkIndicesGpuBuffer, "PreprocessRenderer::m_pSortedChunkIndicesGpuBuffer");
                m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_COPY_DEST;

                if (m_pSortedChunkIndicesUploadBuffer) { m_pSortedChunkIndicesUploadBuffer->Unmap(0, nullptr); m_pSortedChunkIndicesUploadBuffer->Release(); m_pSortedChunkIndicesUploadBuffer = nullptr; }
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                    D3D12_HEAP_FLAG_NONE,
                    &CD3DX12_RESOURCE_DESC::Buffer(idxBytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&m_pSortedChunkIndicesUploadBuffer)));
                SetName(m_pSortedChunkIndicesUploadBuffer, "PreprocessRenderer::m_pSortedChunkIndicesUploadBuffer");
                m_pSortedChunkIndicesUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pSortedChunkIndicesUploadBufferMapped));

                uint32_t pairBytes = numChunkElements * sizeof(uint32_t) * 2;
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
                    SetName(m_pGPUSortPairBuffer, "PreprocessRenderer::m_pGPUSortPairBuffer");
                    m_sortPairBufferCapacityBytes = pairBytes;
                }

                m_pChunkUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pChunkUploadBufferMapped));
                m_chunkBufferCapacityBytes = chunkBytes;
                m_sortedChunkIndicesCapacityBytes = idxBytes;
                modelChanged = true;
                needsSort = true;
                m_needUploadToGpu = true;
                m_needUploadChunkIndicesToGpu = true;
                m_gpuSortNeedsRun = true;
            }

            if (m_pChunkUploadBufferMapped != nullptr && pState->pChunks != nullptr && chunkCount > 0)
            {
                MeshletChunkGPU* pDstChunks = reinterpret_cast<MeshletChunkGPU*>(m_pChunkUploadBufferMapped);
                memcpy(pDstChunks, pState->pChunks, chunkCount * sizeof(MeshletChunkGPU));
                if (numChunkElements > chunkCount)
                {
                    memset(pDstChunks + chunkCount, 0, (numChunkElements - chunkCount) * sizeof(MeshletChunkGPU));
                }
                m_needUploadChunksToGpu = true;

                if (m_pSortedChunkIndicesUploadBufferMapped != nullptr)
                {
                    for (uint32_t i = 0; i < numChunkElements; i++)
                    {
                        m_pSortedChunkIndicesUploadBufferMapped[i] = (i < chunkCount) ? i : 0;
                    }
                    m_needUploadChunkIndicesToGpu = true;
                }
                m_gpuSortNeedsRun = true;
            }

            // If CPU sort is active in Chunked Pipeline, compute chunk sorting on CPU and prepare upload
            if (pState->useChunkedPipeline && !pState->gpuRadixSort && needsSort && m_pSortedChunkIndicesUploadBufferMapped != nullptr)
            {
                auto sortStart = std::chrono::high_resolution_clock::now();
                if (m_chunkDists.size() < chunkCount)
                {
                    m_chunkDists.resize(chunkCount);
                }
                for (uint32_t i = 0; i < chunkCount; i++)
                {
                    const auto& c = pState->pChunks[i];
                    float d = (c.center.x - eyePos.x) * forward.x + (c.center.y - eyePos.y) * forward.y + (c.center.z - eyePos.z) * forward.z;
                    m_chunkDists[i] = { d, i };
                }

                // Descending sort (far to near) with strict weak ordering:
                std::sort(m_chunkDists.begin(), m_chunkDists.begin() + chunkCount, [](const auto& a, const auto& b) {
                    if (a.first != b.first) return a.first > b.first;
                    return a.second < b.second;
                });

                for (uint32_t i = 0; i < chunkCount; i++)
                {
                    m_pSortedChunkIndicesUploadBufferMapped[i] = m_chunkDists[i].second;
                }
                if (numChunkElements > chunkCount)
                {
                    memset(m_pSortedChunkIndicesUploadBufferMapped + chunkCount, 0, (numChunkElements - chunkCount) * sizeof(uint32_t));
                }

                m_needUploadChunkIndicesToGpu = true;
                auto sortEnd = std::chrono::high_resolution_clock::now();
                m_metrics.cpuSortTimeMs = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();
                m_metrics.wasSortedThisFrame = true;
            }
        }

        if (needsSort)
        {
            m_lastSortEye = eyePos;
            m_lastSortForward = forward;
            if (pState->gpuRadixSort)
            {
                m_gpuSortNeedsRun = true;
            }
        }
    }

    // (Re)creates the depth buffer and every TAA/item-prepass texture at the new swapchain size
    void PreprocessRenderer::OnCreateWindowSizeDependentResources(SwapChain* pSwapChain, uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;

        DXGI_FORMAT swapFormat = pSwapChain ? pSwapChain->GetFormat() : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        DXGI_FORMAT typelessFormat = (swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM) ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        DXGI_FORMAT unormFormat = (swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

        m_depthBuffer.InitDepthStencil(m_pDevice, "PreprocessRenderer::m_depthBuffer", &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), 1.0f);
        m_depthBuffer.CreateDSV(0, &m_depthBufferDSV);

        // Optimized Item Buffer (R32_UINT) and Item Depth Buffer (D32_FLOAT) for GPU Silhouette Inversion
        m_itemWidth = std::clamp(width / 4, 320u, 480u);
        m_itemHeight = std::clamp(height / 4, 180u, 270u);

        m_itemBuffer.InitRenderTarget(m_pDevice, "PreprocessRenderer::m_itemBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, m_itemWidth, m_itemHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        m_itemBuffer.CreateRTV(0, &m_itemRTV);
        m_itemBuffer.CreateSRV(0, &m_itemSRV);

        m_itemDepthBuffer.InitDepthStencil(m_pDevice, "PreprocessRenderer::m_itemDepthBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, m_itemWidth, m_itemHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), 1.0f);
        m_itemDepthBuffer.CreateDSV(0, &m_itemDepthDSV);
        m_itemDepthBuffer.CreateSRV(0, &m_itemDepthSRV);

        // Populate contiguous 2-descriptor SRV table for compute shader (t0: ChunkID, t1: Depth)
        m_itemBuffer.CreateSRV(0, &m_itemTableSRVs);
        m_itemDepthBuffer.CreateSRV(1, &m_itemTableSRVs);

        // -------------------------------------------------------------------------
        // Temporal Anti-Aliasing (TAA) & Dither Transition Resolver Buffers
        // -------------------------------------------------------------------------
        D3D12_CLEAR_VALUE sceneClear = {};
        sceneClear.Format = swapFormat;
        sceneClear.Color[0] = 0.0f;
        sceneClear.Color[1] = 0.0f;
        sceneClear.Color[2] = 0.0f;
        sceneClear.Color[3] = 1.0f;

        m_sceneColorBuffer.Init(m_pDevice, "PreprocessRenderer::m_sceneColorBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET, &sceneClear);
        m_sceneColorBuffer.CreateRTV(0, &m_sceneColorRTV, 0, -1, -1, swapFormat);
        m_sceneColorBuffer.CreateSRV(0, &m_sceneColorSRV);

        m_historyColorBuffer.Init(m_pDevice, "PreprocessRenderer::m_historyColorBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr);
        m_historyColorBuffer.CreateSRV(0, &m_historyColorSRV);
        m_historyColorBuffer.CreateUAV(0, &m_historyColorUAV);

        m_resolvedColorBuffer.Init(m_pDevice, "PreprocessRenderer::m_resolvedColorBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr);
        m_resolvedColorBuffer.CreateSRV(0, &m_resolvedColorSRV);
        m_resolvedColorBuffer.CreateUAV(0, &m_resolvedColorUAV);

        m_depthBuffer.CreateSRV(0, &m_depthBufferSRV);

        // Populate contiguous 3-descriptor SRV table (t0: sceneColor, t1: depth, t2: historyColor)
        m_sceneColorBuffer.CreateSRV(0, &m_temporalTableSRVs);
        m_depthBuffer.CreateSRV(1, &m_temporalTableSRVs);
        m_historyColorBuffer.CreateSRV(2, &m_temporalTableSRVs);

        // Populate contiguous 2-descriptor UAV table (u0: resolvedColor, u1: historyColor)
        m_resolvedColorBuffer.CreateUAV(0, &m_temporalTableUAVs);
        m_historyColorBuffer.CreateUAV(1, &m_temporalTableUAVs);

        m_temporalFirstFrame = true;
    }

    // Releases the depth buffer and every TAA/item-prepass texture
    void PreprocessRenderer::OnDestroyWindowSizeDependentResources()
    {
        m_sceneColorBuffer.OnDestroy();
        m_historyColorBuffer.OnDestroy();
        m_resolvedColorBuffer.OnDestroy();
        m_itemBuffer.OnDestroy();
        m_itemDepthBuffer.OnDestroy();
        m_depthBuffer.OnDestroy();
    }

    void PreprocessRenderer::OnUpdateDisplayDependentResources(SwapChain* /*pSwapChain*/) {}

    // The whole frame: upload surfel/chunk/occlusion buffers, run the silhouette prepass and GPU sort
    // if needed, draw the occluder + main splat passes, then resolve TAA and draw ImGui on top.
    void PreprocessRenderer::OnRender(State* pState, SwapChain* pSwapChain)
    {
        static uint32_t s_frameCounter = 0;
        s_frameCounter++;
        if (s_frameCounter % 60 == 0)
        {
            LogTransitionTrace("Renderer Heartbeat: frame=%u, chunkCount=%u, surfelCount=%u, fps=%.1f",
                s_frameCounter, pState->chunkCount, pState->surfelCount, m_metrics.frameRate);
        }

        // Throttle CPU so it does not overwrite in-flight command allocator / dynamic buffers
        pSwapChain->WaitForSwapChain();

        // Safely map silhouette bitmask readback buffer from previous completed frame
        if (m_pSilhouetteReadbackBuffer != nullptr && !m_silhouetteBitmaskCPU.empty())
        {
            uint32_t bitmaskDwords = (uint32_t)m_silhouetteBitmaskCPU.size();
            void* pReadData = nullptr;
            D3D12_RANGE readRange = { 0, bitmaskDwords * sizeof(uint32_t) };
            if (SUCCEEDED(m_pSilhouetteReadbackBuffer->Map(0, &readRange, &pReadData)) && pReadData != nullptr)
            {
                memcpy(m_silhouetteBitmaskCPU.data(), pReadData, bitmaskDwords * sizeof(uint32_t));
                D3D12_RANGE writtenRange = { 0, 0 };
                m_pSilhouetteReadbackBuffer->Unmap(0, &writtenRange);
            }
        }

        m_constantBufferRing.OnBeginFrame();
        m_commandListRing.OnBeginFrame();

        ID3D12GraphicsCommandList* pCmdLst = m_commandListRing.GetNewCommandList();

        ID3D12DescriptorHeap* descriptorHeaps[] = { m_resourceViewHeaps.GetCBV_SRV_UAVHeap() };
        pCmdLst->SetDescriptorHeaps(1, descriptorHeaps);

        ID3D12Resource* pBackBuffer = pSwapChain->GetCurrentBackBufferResource();
        bool useTemporal = pState->enableTemporalFiltering && (m_pTemporalPSO != nullptr) && (m_pTemporalRootSig != nullptr);

        if (!useTemporal)
        {
            D3D12_RESOURCE_BARRIER toRtv = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            pCmdLst->ResourceBarrier(1, &toRtv);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = useTemporal ? m_sceneColorRTV.GetCPU() : *pSwapChain->GetCurrentBackBufferRTV();
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_depthBufferDSV.GetCPU();
        pCmdLst->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // Pure pitch black (SuperSplat reference)
        pCmdLst->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        pCmdLst->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        CD3DX12_VIEWPORT viewport(0.0f, 0.0f, (float)m_width, (float)m_height);
        CD3DX12_RECT scissor(0, 0, m_width, m_height);
        pCmdLst->RSSetViewports(1, &viewport);
        pCmdLst->RSSetScissorRects(1, &scissor);

        // Active viewing camera
        const float cy = cosf(pState->camPitch), sy = sinf(pState->camPitch);
        const float sx = sinf(pState->camYaw), cx = cosf(pState->camYaw);
        XMFLOAT3 eyePos(
            pState->camTarget.x + pState->camDistance * cy * sx,
            pState->camTarget.y + pState->camDistance * sy,
            pState->camTarget.z + pState->camDistance * cy * cx
        );

        auto frameStart = std::chrono::high_resolution_clock::now();

        XMVECTOR eye = XMLoadFloat3(&eyePos);
        XMVECTOR at = XMLoadFloat3(&pState->camTarget);
        XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(at, eye));
        XMFLOAT3 forwardNorm;
        XMStoreFloat3(&forwardNorm, forward);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
        XMVECTOR camUp = XMVector3Cross(forward, right);

        XMMATRIX view = XMMatrixLookAtRH(eye, at, worldUp);
        XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, pState->aspectRatio, 0.1f, 500.0f);
        XMMATRIX unjitteredViewProj = XMMatrixMultiply(view, proj);

        // 8-Phase Halton (2, 3) Sub-Pixel Camera Jitter
        static const float s_halton23[8][2] = {
            { 0.000000f, -0.166667f },
            {-0.250000f,  0.166667f },
            { 0.250000f, -0.388889f },
            {-0.375000f, -0.055556f },
            { 0.125000f,  0.277778f },
            {-0.125000f, -0.277778f },
            { 0.375000f,  0.055556f },
            {-0.437500f,  0.388889f }
        };

        XMMATRIX viewProj = unjitteredViewProj;
        if (pState->enableTemporalFiltering && pState->enableSubpixelJitter && m_width > 0 && m_height > 0)
        {
            m_jitterPhase = (m_jitterPhase + 1) % 8;
            float jitterX = s_halton23[m_jitterPhase][0] * (2.0f / (float)m_width);
            float jitterY = s_halton23[m_jitterPhase][1] * (2.0f / (float)m_height);
            XMMATRIX jitterMat = XMMatrixTranslation(jitterX, jitterY, 0.0f);
            XMMATRIX jitteredProj = XMMatrixMultiply(proj, jitterMat);
            viewProj = XMMatrixMultiply(view, jitteredProj);
        }

        // Culling camera (detached freeze or active)
        float cPitch = pState->detachCullCamera ? pState->cullPitch : pState->camPitch;
        float cYaw   = pState->detachCullCamera ? pState->cullYaw : pState->camYaw;
        float cDist  = pState->detachCullCamera ? pState->cullDistance : pState->camDistance;
        XMFLOAT3 cTarget = pState->detachCullCamera ? pState->cullTarget : pState->camTarget;

        const float c_cy = cosf(cPitch), c_sy = sinf(cPitch);
        const float c_sx = sinf(cYaw), c_cx = cosf(cYaw);
        XMFLOAT3 cullEyePos(
            cTarget.x + cDist * c_cy * c_sx,
            cTarget.y + cDist * c_sy,
            cTarget.z + cDist * c_cy * c_cx
        );
        XMVECTOR cEye = XMLoadFloat3(&cullEyePos);
        XMVECTOR cAt = XMLoadFloat3(&cTarget);
        XMVECTOR cForward = XMVector3Normalize(XMVectorSubtract(cAt, cEye));
        XMFLOAT3 cullForwardNorm;
        XMStoreFloat3(&cullForwardNorm, cForward);

        XMMATRIX cView = XMMatrixLookAtRH(cEye, cAt, worldUp);
        XMMATRIX cProj = XMMatrixPerspectiveFovRH(XM_PIDIV4, pState->aspectRatio, 0.1f, 500.0f);
        XMMATRIX cViewProj = XMMatrixMultiply(cView, cProj);

        // Depth sorting is ALWAYS executed from the active viewer's perspective
        UpdateSurfelBuffers(pState, eyePos, forwardNorm);
        UpdateOcclusionVoxelBuffer(pState);
        uint32_t surfelCount = pState->surfelCount;

        // Compute Geometry Optimization & Culling Statistics
        m_cullStats = {};
        uint32_t baseDatasetSurfels = (pState->totalDatasetSurfels > 0) ? pState->totalDatasetSurfels : surfelCount;
        uint32_t baseDatasetChunks  = (pState->totalDatasetChunks > 0)  ? pState->totalDatasetChunks  : pState->chunkCount;
        m_cullStats.totalDatasetSurfels = baseDatasetSurfels;
        m_cullStats.totalDatasetChunks  = baseDatasetChunks;
        m_cullStats.lodActiveSurfels    = surfelCount;
        m_cullStats.lodActiveChunks     = pState->chunkCount;
        m_cullStats.lodPrunedSurfels    = (baseDatasetSurfels > surfelCount) ? (baseDatasetSurfels - surfelCount) : 0;

        XMMATRIX activeCullMatrix = pState->detachCullCamera ? cViewProj : viewProj;
        XMFLOAT3 activeCullEye = pState->detachCullCamera ? cullEyePos : eyePos;

        if (pState->useChunkedPipeline && pState->chunkCount > 0 && pState->pChunks != nullptr)
        {
            uint32_t frustumCulledChunks = 0;
            uint32_t frustumCulledSurfels = 0;
            uint32_t coneCulledChunks = 0;
            uint32_t coneCulledSurfels = 0;
            uint32_t passedChunks = 0;
            uint32_t passedSurfels = 0;

            // Extract normalized 6 frustum planes (Gribb-Hartmann) for O(1) sphere-frustum testing
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, activeCullMatrix);
            XMFLOAT4 planes[6];
            planes[0] = { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 }; // Left
            planes[1] = { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 }; // Right
            planes[2] = { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 }; // Bottom
            planes[3] = { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 }; // Top
            planes[4] = { m._13, m._23, m._33, m._43 };                                  // Near
            planes[5] = { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 }; // Far

            for (int p = 0; p < 6; p++)
            {
                float len = std::sqrt(planes[p].x * planes[p].x + planes[p].y * planes[p].y + planes[p].z * planes[p].z);
                if (len > 1e-6f)
                {
                    planes[p].x /= len;
                    planes[p].y /= len;
                    planes[p].z /= len;
                    planes[p].w /= len;
                }
            }

            for (uint32_t i = 0; i < pState->chunkCount; i++)
            {
                const auto& chunk = pState->pChunks[i];
                bool isVisible = true;
                for (int p = 0; p < 6; p++)
                {
                    float dist = planes[p].x * chunk.center.x + planes[p].y * chunk.center.y + planes[p].z * chunk.center.z + planes[p].w;
                    if (dist < -chunk.boundingRadius)
                    {
                        isVisible = false;
                        break;
                    }
                }

                if (!isVisible)
                {
                    frustumCulledChunks++;
                    frustumCulledSurfels += chunk.surfelCount;
                }
                else
                {
                    bool coneVisible = true;
                    if (pState->enableConeCulling && chunk.coneCutoff > -0.99f)
                    {
                        float dx = chunk.center.x - activeCullEye.x;
                        float dy = chunk.center.y - activeCullEye.y;
                        float dz = chunk.center.z - activeCullEye.z;
                        float distSq = dx * dx + dy * dy + dz * dz;
                        if (distSq > 1e-8f)
                        {
                            float invDist = 1.0f / std::sqrt(distSq);
                            float vx = dx * invDist;
                            float vy = dy * invDist;
                            float vz = dz * invDist;
                            float sinCone = std::sqrt(std::max(0.0f, 1.0f - chunk.coneCutoff * chunk.coneCutoff));
                            float nDotV = chunk.coneAxis.x * vx + chunk.coneAxis.y * vy + chunk.coneAxis.z * vz;
                            if (nDotV > sinCone + 0.02f)
                            {
                                coneVisible = false;
                            }
                        }
                    }

                    if (!coneVisible)
                    {
                        coneCulledChunks++;
                        coneCulledSurfels += chunk.surfelCount;
                    }
                    else
                    {
                        passedChunks++;
                        passedSurfels += chunk.surfelCount;
                    }
                }
            }

            m_cullStats.asFrustumCulledChunks  = frustumCulledChunks;
            m_cullStats.asFrustumCulledSurfels = frustumCulledSurfels;
            m_cullStats.asConeCulledChunks     = coneCulledChunks;
            m_cullStats.asConeCulledSurfels    = coneCulledSurfels;
            m_cullStats.asPassedChunks         = passedChunks;
            m_cullStats.asPassedSurfels        = passedSurfels;
            m_cullStats.msDrawnSurfels         = passedSurfels;
        }
        else
        {
            m_cullStats.asPassedChunks  = pState->chunkCount;
            m_cullStats.asPassedSurfels = surfelCount;
            m_cullStats.msDrawnSurfels  = surfelCount;
        }

        m_cullStats.generatedVertices  = m_cullStats.msDrawnSurfels * 4;
        m_cullStats.generatedTriangles = m_cullStats.msDrawnSurfels * 2;

        if (m_cullStats.totalDatasetSurfels > 0)
        {
            m_cullStats.totalCullingRatio = ((float)(m_cullStats.totalDatasetSurfels - m_cullStats.msDrawnSurfels) / (float)m_cullStats.totalDatasetSurfels) * 100.0f;
            m_cullStats.lodDecimationRatio = ((float)m_cullStats.lodPrunedSurfels / (float)m_cullStats.totalDatasetSurfels) * 100.0f;
        }
        if (m_cullStats.lodActiveSurfels > 0)
        {
            m_cullStats.asCullingRatio = ((float)(m_cullStats.asFrustumCulledSurfels + m_cullStats.asConeCulledSurfels) / (float)m_cullStats.lodActiveSurfels) * 100.0f;
        }
        float bytesPerSurfel = (pState->renderMode == 2) ? 40.0f : 8.0f;
        m_cullStats.vramBandwidthSavedMB = (float)(m_cullStats.lodPrunedSurfels + m_cullStats.asFrustumCulledSurfels + m_cullStats.asConeCulledSurfels) * bytesPerSurfel / (1024.0f * 1024.0f);


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
        pCB->viewerEyePos = eyePos;
        pCB->sphereRadius = 1.0f;
        pCB->surfelCount = surfelCount;
        pCB->renderMode = pState->renderMode;
        pCB->orientMode = pState->orientMode;
        pCB->totalChunks = pState->chunkCount;
        pCB->aabbMin = pState->aabbMin;
        pCB->useChunkedPipeline = (pState->useChunkedPipeline && pState->chunkCount > 0 && m_pChunkGpuBuffer != nullptr) ? 1 : 0;
        pCB->aabbExtents = pState->aabbExtents;
        pCB->useDetachedCullCam = pState->detachCullCamera ? 1 : 0;
        XMStoreFloat4x4(&pCB->cullViewProj, cViewProj);
        pCB->cullEyePos = cullEyePos;
        pCB->enableDithering = pState->enableDithering ? 1 : 0;
        pCB->highlightSilhouette = pState->highlightSilhouette ? 1 : 0;
        pCB->enableConeCulling = pState->enableConeCulling ? 1 : 0;
        pCB->showOnlyLocked = pState->showOnlyLockedChunks ? 1 : 0;
        pCB->showChunkStream = pState->showChunkStream ? 1 : 0;
        pCB->enableOcclusionCulling = (pState->enableOcclusionCulling && pState->occlusionVoxelCount > 0 && m_pOcclusionVoxelBuffer != nullptr) ? 1 : 0;
        pCB->occlusionShrinkRuntime = pState->occlusionShrinkRuntime;
        pCB->showOcclusionVolumeOnly = pState->showOcclusionVolumeOnly ? 1 : 0;
        pCB->occlusionVoxelCount = pState->occlusionVoxelCount;

        ID3D12Resource* pGpuRes = (pState->renderMode == 2) ? m_pRawSurfelGpuBuffer : m_pSurfelGpuBuffer;
        ID3D12Resource* pGpuOutRes = (pState->renderMode == 2) ? m_pRawSurfelGpuOutBuffer : m_pSurfelGpuOutBuffer;
        ID3D12Resource* pUploadRes = (pState->renderMode == 2) ? m_pRawSurfelBuffer : m_pSurfelBuffer;
        auto dispatchStart = std::chrono::high_resolution_clock::now();
        m_metrics.uploadTimeMs = 0.0f;
        m_metrics.silhouettePrepassTimeMs = 0.0f;
        m_metrics.occluderPassTimeMs = 0.0f;
        m_metrics.mainDispatchTimeMs = 0.0f;
        m_metrics.taaResolveTimeMs = 0.0f;

        // Interior Occlusion Volume: solid depth-writing cubes drawn before the splat pass so far-side
        // surfels visible through gaps in the near side get discarded by the main pass's depth test
        // (see m_pPipelineStateOcclusionTest). Reuses the frame's already-allocated SurfelsCB (cbAddress) --
        // it only needs viewProj/eye and the occlusion-specific fields added to that struct.
        auto DrawOccluderPass = [&]()
        {
            // Drawn whenever culling is enabled OR the user just wants to look at the volume on its
            // own -- "view only" must not require "enable culling" too, or the two together would draw
            // neither the occluder (culling off) nor the splats (view-only skips them): a blank screen.
            bool wantOccluderVisible = pState->enableOcclusionCulling || pState->showOcclusionVolumeOnly;
            if (!wantOccluderVisible || pState->occlusionVoxelCount == 0 || m_pOcclusionVoxelBuffer == nullptr || m_pOccluderPSO == nullptr)
                return;

            auto occluderStart = std::chrono::high_resolution_clock::now();

            pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
            pCmdLst->SetPipelineState(m_pOccluderPSO);
            pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
            pCmdLst->SetGraphicsRootShaderResourceView(1, 0); // Unused by occluderMS/PS
            pCmdLst->SetGraphicsRootShaderResourceView(2, 0); // Unused by occluderMS/PS
            pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
            pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);
            pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer->GetGPUVirtualAddress());

            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> occluderCmd6;
            pCmdLst->QueryInterface(IID_PPV_ARGS(&occluderCmd6));
            uint32_t dimX = std::min(pState->occlusionVoxelCount, 32768u);
            uint32_t dimY = (dimX > 0) ? ((pState->occlusionVoxelCount + dimX - 1) / dimX) : 1;
            occluderCmd6->DispatchMesh(dimX, dimY, 1);

            auto occluderEnd = std::chrono::high_resolution_clock::now();
            m_metrics.occluderPassTimeMs = std::chrono::duration<float, std::milli>(occluderEnd - occluderStart).count();
        };
        ID3D12PipelineState* pMainSplatPSO = (pState->enableOcclusionCulling && m_pPipelineStateOcclusionTest != nullptr)
            ? m_pPipelineStateOcclusionTest
            : m_pPipelineState;

        if (surfelCount > 0 && pGpuRes != nullptr && pUploadRes != nullptr)
        {
            if (m_needUploadToGpu || m_needUploadChunksToGpu)
            {
                auto uploadStart = std::chrono::high_resolution_clock::now();
                if (pState->useCopyQueue && m_pCopyQueue != nullptr && m_pCopyCmdList != nullptr && m_pCopyAllocator != nullptr)
                {
                    // Ensure previous background copy execution has completed before resetting allocator
                    if (m_pCopyFence->GetCompletedValue() < m_copyFenceValue)
                    {
                        m_pCopyFence->SetEventOnCompletion(m_copyFenceValue, m_copyFenceEvent);
                        WaitForSingleObject(m_copyFenceEvent, INFINITE);
                    }

                    m_pCopyAllocator->Reset();
                    m_pCopyCmdList->Reset(m_pCopyAllocator, nullptr);

                    if (m_needUploadToGpu)
                    {
                        m_pCopyCmdList->CopyResource(pGpuRes, pUploadRes);
                    }
                    if (m_needUploadChunksToGpu && m_pChunkGpuBuffer != nullptr && m_pChunkUploadBuffer != nullptr)
                    {
                        m_pCopyCmdList->CopyResource(m_pChunkGpuBuffer, m_pChunkUploadBuffer);
                    }

                    m_pCopyCmdList->Close();
                    ID3D12CommandList* ppCopyLists[] = { m_pCopyCmdList };
                    m_pCopyQueue->ExecuteCommandLists(1, ppCopyLists);

                    uint64_t fenceVal = ++m_copyFenceValue;
                    m_pCopyQueue->Signal(m_pCopyFence, fenceVal);

                    // Direct graphics queue awaits completion of background DMA upload before compute/mesh execution
                    m_pDevice->GetGraphicsQueue()->Wait(m_pCopyFence, fenceVal);

                    if (m_needUploadChunksToGpu && m_pChunkGpuBuffer != nullptr)
                    {
                        D3D12_RESOURCE_BARRIER chunkToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                            m_pChunkGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        pCmdLst->ResourceBarrier(1, &chunkToSrv);
                        m_chunkGpuBufferState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    }
                }
                else
                {
                    // Direct Queue fallback
                    if (m_needUploadToGpu && pGpuRes != nullptr && pUploadRes != nullptr)
                    {
                        D3D12_RESOURCE_BARRIER preCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                            pGpuRes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                        pCmdLst->ResourceBarrier(1, &preCopy);

                        pCmdLst->CopyResource(pGpuRes, pUploadRes);

                        D3D12_RESOURCE_BARRIER postCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                            pGpuRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        pCmdLst->ResourceBarrier(1, &postCopy);
                    }
                    if (m_needUploadChunksToGpu && m_pChunkGpuBuffer != nullptr && m_pChunkUploadBuffer != nullptr)
                    {
                        if (m_chunkGpuBufferState != D3D12_RESOURCE_STATE_COPY_DEST)
                        {
                            D3D12_RESOURCE_BARRIER preCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                                m_pChunkGpuBuffer, m_chunkGpuBufferState, D3D12_RESOURCE_STATE_COPY_DEST);
                            pCmdLst->ResourceBarrier(1, &preCopy);
                            m_chunkGpuBufferState = D3D12_RESOURCE_STATE_COPY_DEST;
                        }

                        pCmdLst->CopyResource(m_pChunkGpuBuffer, m_pChunkUploadBuffer);

                        D3D12_RESOURCE_BARRIER chunkToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                            m_pChunkGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        pCmdLst->ResourceBarrier(1, &chunkToSrv);
                        m_chunkGpuBufferState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    }

                    if (m_needUploadChunkIndicesToGpu && m_pSortedChunkIndicesGpuBuffer != nullptr && m_pSortedChunkIndicesUploadBuffer != nullptr)
                    {
                        if (m_sortedChunkIndicesState != D3D12_RESOURCE_STATE_COPY_DEST)
                        {
                            D3D12_RESOURCE_BARRIER preCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                                m_pSortedChunkIndicesGpuBuffer, m_sortedChunkIndicesState, D3D12_RESOURCE_STATE_COPY_DEST);
                            pCmdLst->ResourceBarrier(1, &preCopy);
                            m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_COPY_DEST;
                        }

                        pCmdLst->CopyResource(m_pSortedChunkIndicesGpuBuffer, m_pSortedChunkIndicesUploadBuffer);

                        D3D12_RESOURCE_BARRIER idxToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                            m_pSortedChunkIndicesGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        pCmdLst->ResourceBarrier(1, &idxToSrv);
                        m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                        m_needUploadChunkIndicesToGpu = false;
                    }
                }
                m_needUploadToGpu = false;
                m_needUploadChunksToGpu = false;
                m_needUploadChunkIndicesToGpu = false;

                auto uploadEnd = std::chrono::high_resolution_clock::now();
                m_metrics.uploadTimeMs = std::chrono::duration<float, std::milli>(uploadEnd - uploadStart).count();
            }

            bool useChunked = (pState->useChunkedPipeline && pState->chunkCount > 0 && m_pChunkGpuBuffer != nullptr && m_pSortedChunkIndicesGpuBuffer != nullptr);

            if (useChunked)
            {
                // =========================================================================
                // Two-Level Hierarchical Micro-Chunk Pipeline (Coarse GPU Sort + AS Culling)
                // =========================================================================
                uint32_t chunkCount = pState->chunkCount;
                uint32_t numChunkElements = 1;
                while (numChunkElements < chunkCount) numChunkElements <<= 1;
                if (numChunkElements < 1024) numChunkElements = 1024;

                if (pState->gpuRadixSort && m_pProjectChunkKeysPSO && m_pBitonicLocalSortPSO && m_pBitonicGlobalSortPSO && m_pBitonicLocalMergePSO && m_pGatherChunkIndicesPSO && m_pComputeRootSignature && m_pGPUSortPairBuffer != nullptr)
                {
                    if (m_gpuSortNeedsRun)
                    {
                        auto gpuSortStart = std::chrono::high_resolution_clock::now();

                        if (m_sortedChunkIndicesState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                        {
                            D3D12_RESOURCE_BARRIER preBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                m_pSortedChunkIndicesGpuBuffer, m_sortedChunkIndicesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            pCmdLst->ResourceBarrier(1, &preBarrier);
                            m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        }

                        pCmdLst->SetComputeRootSignature(m_pComputeRootSignature);
                        pCmdLst->SetComputeRootShaderResourceView(1, m_pSurfelGpuBuffer ? m_pSurfelGpuBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootShaderResourceView(2, m_pRawSurfelGpuBuffer ? m_pRawSurfelGpuBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootShaderResourceView(3, m_pChunkGpuBuffer->GetGPUVirtualAddress());
                        pCmdLst->SetComputeRootUnorderedAccessView(4, m_pGPUSortPairBuffer->GetGPUVirtualAddress());
                        pCmdLst->SetComputeRootUnorderedAccessView(5, m_pSurfelGpuOutBuffer ? m_pSurfelGpuOutBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootUnorderedAccessView(6, m_pRawSurfelGpuOutBuffer ? m_pRawSurfelGpuOutBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootUnorderedAccessView(7, m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress());

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
                        baseCB.totalSurfels = chunkCount;
                        baseCB.numElements = numChunkElements;
                        baseCB.renderMode = pState->renderMode;
                        baseCB.aabbMin = pState->aabbMin;
                        baseCB.aabbExtents = pState->aabbExtents;

                        uint32_t localGroups = (numChunkElements + 1023) / 1024;
                        uint32_t globalGroups = (numChunkElements + 255) / 256;

                        D3D12_RESOURCE_BARRIER uavPairBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_pGPUSortPairBuffer);
                        D3D12_RESOURCE_BARRIER uavIdxBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_pSortedChunkIndicesGpuBuffer);

                        // 1. Project Chunk Centers
                        {
                            BitonicCB* pSortCB = nullptr;
                            D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                            if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                            {
                                *pSortCB = baseCB;
                                pCmdLst->SetPipelineState(m_pProjectChunkKeysPSO);
                                pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                                pCmdLst->Dispatch(globalGroups, 1, 1);
                                pCmdLst->ResourceBarrier(1, &uavPairBarrier);
                            }
                        }

                        // 2. Local LDS Block Sort
                        {
                            BitonicCB* pSortCB = nullptr;
                            D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                            if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                            {
                                *pSortCB = baseCB;
                                pSortCB->level = std::min(numChunkElements, 1024u);
                                pSortCB->levelMask = pSortCB->level >> 1;

                                pCmdLst->SetPipelineState(m_pBitonicLocalSortPSO);
                                pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                                pCmdLst->Dispatch(localGroups, 1, 1);
                                pCmdLst->ResourceBarrier(1, &uavPairBarrier);
                            }
                        }

                        // 3. Outer Levels (if numChunkElements >= 2048)
                        for (uint32_t level = 2048; level <= numChunkElements; level <<= 1)
                        {
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

                        // 4. Gather Sorted Chunk Indices
                        {
                            BitonicCB* pSortCB = nullptr;
                            D3D12_GPU_VIRTUAL_ADDRESS sortCbAddr = 0;
                            if (m_constantBufferRing.AllocConstantBuffer(sizeof(BitonicCB), (void**)&pSortCB, &sortCbAddr))
                            {
                                *pSortCB = baseCB;
                                pCmdLst->SetPipelineState(m_pGatherChunkIndicesPSO);
                                pCmdLst->SetComputeRootConstantBufferView(0, sortCbAddr);
                                pCmdLst->Dispatch(globalGroups, 1, 1);
                                pCmdLst->ResourceBarrier(1, &uavIdxBarrier);
                            }
                        }

                        D3D12_RESOURCE_BARRIER postBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                            m_pSortedChunkIndicesGpuBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        pCmdLst->ResourceBarrier(1, &postBarrier);
                        m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

                        auto gpuSortEnd = std::chrono::high_resolution_clock::now();
                        m_metrics.gpuSortTimeMs = std::chrono::duration<float, std::milli>(gpuSortEnd - gpuSortStart).count();
                        m_metrics.isGPUSortActive = true;
                        m_metrics.wasSortedThisFrame = true;
                        m_gpuSortNeedsRun = false;
                    }
                }
                else if (m_needUploadChunkIndicesToGpu && m_pSortedChunkIndicesGpuBuffer && m_pSortedChunkIndicesUploadBuffer)
                {
                    if (m_sortedChunkIndicesState != D3D12_RESOURCE_STATE_COPY_DEST)
                    {
                        D3D12_RESOURCE_BARRIER preCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                            m_pSortedChunkIndicesGpuBuffer, m_sortedChunkIndicesState, D3D12_RESOURCE_STATE_COPY_DEST);
                        pCmdLst->ResourceBarrier(1, &preCopy);
                        m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_COPY_DEST;
                    }

                    pCmdLst->CopyResource(m_pSortedChunkIndicesGpuBuffer, m_pSortedChunkIndicesUploadBuffer);

                    D3D12_RESOURCE_BARRIER idxToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pSortedChunkIndicesGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    pCmdLst->ResourceBarrier(1, &idxToSrv);
                    m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    m_needUploadChunkIndicesToGpu = false;
                }

                // Ensure m_pSortedChunkIndicesGpuBuffer is in ALL_SHADER_RESOURCE state before mesh dispatch
                if (m_pSortedChunkIndicesGpuBuffer && m_sortedChunkIndicesState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
                {
                    D3D12_RESOURCE_BARRIER ensureSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pSortedChunkIndicesGpuBuffer, m_sortedChunkIndicesState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    pCmdLst->ResourceBarrier(1, &ensureSrv);
                    m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                }

                // Ensure m_pChunkGpuBuffer is in ALL_SHADER_RESOURCE state before mesh dispatch
                if (m_pChunkGpuBuffer && m_chunkGpuBufferState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
                {
                    D3D12_RESOURCE_BARRIER ensureChunkSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pChunkGpuBuffer, m_chunkGpuBufferState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    pCmdLst->ResourceBarrier(1, &ensureChunkSrv);
                    m_chunkGpuBufferState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                }

                D3D12_GPU_VIRTUAL_ADDRESS surfelAddr = pGpuRes->GetGPUVirtualAddress();

                DrawOccluderPass();

                if (!pState->showOcclusionVolumeOnly)
                {
                    auto mainDispatchStart = std::chrono::high_resolution_clock::now();

                    pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                    pCmdLst->SetPipelineState(pMainSplatPSO);
                    pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                    pCmdLst->SetGraphicsRootShaderResourceView(1, surfelAddr);
                    pCmdLst->SetGraphicsRootShaderResourceView(2, surfelAddr);
                    pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer ? m_pOcclusionVoxelBuffer->GetGPUVirtualAddress() : 0);

                    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
                    pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
                    uint32_t asGroupCount = (chunkCount + AS_GROUP_SIZE - 1) / AS_GROUP_SIZE;
                    cmdList6->DispatchMesh(asGroupCount, 1, 1);

                    auto mainDispatchEnd = std::chrono::high_resolution_clock::now();
                    m_metrics.mainDispatchTimeMs += std::chrono::duration<float, std::milli>(mainDispatchEnd - mainDispatchStart).count();
                }
            }
            // Execute Flat GPU LDS Key-Index Sorting Pipeline
            else if (pState->gpuRadixSort && m_pProjectKeysPSO && m_pBitonicLocalSortPSO && m_pBitonicGlobalSortPSO && m_pBitonicLocalMergePSO && m_pGatherSurfelsPSO && m_pComputeRootSignature && pGpuOutRes != nullptr && m_pGPUSortPairBuffer != nullptr)
            {
                if (m_gpuSortNeedsRun)
                {
                    auto gpuSortStart = std::chrono::high_resolution_clock::now();

                    // Transition input buffer to SRV, and output buffer to UAV
                    D3D12_RESOURCE_BARRIER preBarriers[2] = {};
                    preBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(pGpuRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    preBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(pGpuOutRes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    pCmdLst->ResourceBarrier(2, preBarriers);

                    pCmdLst->SetComputeRootSignature(m_pComputeRootSignature);
                    pCmdLst->SetComputeRootShaderResourceView(1, m_pSurfelGpuBuffer ? m_pSurfelGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootShaderResourceView(2, m_pRawSurfelGpuBuffer ? m_pRawSurfelGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootUnorderedAccessView(4, m_pGPUSortPairBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetComputeRootUnorderedAccessView(5, m_pSurfelGpuOutBuffer ? m_pSurfelGpuOutBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootUnorderedAccessView(6, m_pRawSurfelGpuOutBuffer ? m_pRawSurfelGpuOutBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootUnorderedAccessView(7, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);

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

                    uint32_t localGroups = (numElements + 1023) / 1024;
                    uint32_t globalGroups = (numElements + 255) / 256;

                    D3D12_RESOURCE_BARRIER uavPairBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_pGPUSortPairBuffer);
                    D3D12_RESOURCE_BARRIER uavOutBarrier = CD3DX12_RESOURCE_BARRIER::UAV(pGpuOutRes);

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
                    postBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(pGpuOutRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(pGpuRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    pCmdLst->ResourceBarrier(2, postBarriers);

                    auto gpuSortEnd = std::chrono::high_resolution_clock::now();
                    m_metrics.gpuSortTimeMs = std::chrono::duration<float, std::milli>(gpuSortEnd - gpuSortStart).count();
                    m_metrics.isGPUSortActive = true;
                    m_metrics.wasSortedThisFrame = true;
                    m_gpuSortNeedsRun = false;
                }
                else
                {
                    m_metrics.gpuSortTimeMs = 0.0f;
                    m_metrics.isGPUSortActive = true;
                    m_metrics.wasSortedThisFrame = false;
                }
            }

            bool isFlatGpuSorted = (!pState->useChunkedPipeline && pState->gpuRadixSort && pGpuOutRes != nullptr);
            ID3D12Resource* pDrawSurfelRes = isFlatGpuSorted ? pGpuOutRes : pGpuRes;

            D3D12_GPU_VIRTUAL_ADDRESS drawSurfelAddr = (pDrawSurfelRes != nullptr) ? pDrawSurfelRes->GetGPUVirtualAddress() : m_surfelBufferGPUAddress;

            // -------------------------------------------------------------------------
            // GPU ITEM PREPASS & SILHOUETTE EDGE EXTRACTION (Option 2)
            // -------------------------------------------------------------------------
            uint32_t bitmaskDwords = (pState->chunkCount + 31) / 32;
            if (bitmaskDwords < 64) bitmaskDwords = 64;
            uint32_t bitmaskBytes = bitmaskDwords * sizeof(uint32_t);

            bool edgeNeedsRecalc = false;
            if (pState->enableGpuSilhouetteInversion && pState->chunkCount > 0 && m_pItemPrepassPSO != nullptr && m_itemBuffer.GetResource() != nullptr)
            {
                if (!m_edgeBitmaskValid ||
                    memcmp(&m_lastEdgeViewProj, &pCB->viewProj, sizeof(XMFLOAT4X4)) != 0 ||
                    m_lastEdgeChunkCount != pState->chunkCount ||
                    m_lastEdgeDepthThreshold != pState->silhouetteDepthThreshold ||
                    m_lastEdgeExteriorOnly != pState->silhouetteExteriorOnly)
                {
                    edgeNeedsRecalc = true;
                }
            }

            if (edgeNeedsRecalc)
            {
                auto silhouettePrepassStart = std::chrono::high_resolution_clock::now();

                if (bitmaskBytes > m_bitmaskCapacityBytes)
                {
                    // Unlike every other resize path in this file, this one was releasing live GPU
                    // resources with no synchronization at all: the compute dispatch that UAV-writes
                    // m_pSilhouetteBitmaskGpuBuffer and the CopyBufferRegion into m_pSilhouetteReadbackBuffer
                    // are submitted on pCmdLst and can still be executing on the GPU (WaitForSwapChain()
                    // only throttles allocator reuse across a few buffered frames, it is not a full flush)
                    // when chunkCount first grows enough to need a bigger bitmask -- which happens on
                    // essentially every frame of a dither transition. Releasing/recreating out from under
                    // an in-flight GPU write is a use-after-free on the GPU: undefined behavior, capable of
                    // hanging the device (DXGI_ERROR_DEVICE_HUNG) with no CPU-visible symptom.
                    m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);

                    if (m_pSilhouetteBitmaskGpuBuffer) { m_pSilhouetteBitmaskGpuBuffer->Release(); m_pSilhouetteBitmaskGpuBuffer = nullptr; }
                    if (m_pSilhouetteReadbackBuffer) { m_pSilhouetteReadbackBuffer->Release(); m_pSilhouetteReadbackBuffer = nullptr; }

                    CD3DX12_RESOURCE_DESC uavDesc = CD3DX12_RESOURCE_DESC::Buffer(bitmaskBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                        D3D12_HEAP_FLAG_NONE,
                        &uavDesc,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        nullptr,
                        IID_PPV_ARGS(&m_pSilhouetteBitmaskGpuBuffer)));
                    SetName(m_pSilhouetteBitmaskGpuBuffer, "PreprocessRenderer::m_pSilhouetteBitmaskGpuBuffer");

                    ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
                        D3D12_HEAP_FLAG_NONE,
                        &CD3DX12_RESOURCE_DESC::Buffer(bitmaskBytes),
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr,
                        IID_PPV_ARGS(&m_pSilhouetteReadbackBuffer)));
                    SetName(m_pSilhouetteReadbackBuffer, "PreprocessRenderer::m_pSilhouetteReadbackBuffer");

                    D3D12_UNORDERED_ACCESS_VIEW_DESC uavView = {};
                    uavView.Format = DXGI_FORMAT_UNKNOWN;
                    uavView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    uavView.Buffer.NumElements = bitmaskDwords;
                    uavView.Buffer.StructureByteStride = sizeof(uint32_t);
                    m_pDevice->GetDevice()->CreateUnorderedAccessView(m_pSilhouetteBitmaskGpuBuffer, nullptr, &uavView, m_silhouetteBitmaskUAV.GetCPU());

                    m_bitmaskCapacityBytes = bitmaskBytes;
                    m_silhouetteBitmaskCPU.resize(bitmaskDwords, 0);
                }

                // 1. Transition ItemBuffer -> RENDER_TARGET, ItemDepthBuffer -> DEPTH_WRITE
                D3D12_RESOURCE_BARRIER preBarriers[2] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(m_itemBuffer.GetResource(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    CD3DX12_RESOURCE_BARRIER::Transition(m_itemDepthBuffer.GetResource(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
                };
                pCmdLst->ResourceBarrier(2, preBarriers);

                D3D12_CPU_DESCRIPTOR_HANDLE itemRtvHandle = m_itemRTV.GetCPU();
                D3D12_CPU_DESCRIPTOR_HANDLE itemDsvHandle = m_itemDepthDSV.GetCPU();
                pCmdLst->OMSetRenderTargets(1, &itemRtvHandle, FALSE, &itemDsvHandle);

                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                pCmdLst->ClearRenderTargetView(itemRtvHandle, clearZero, 0, nullptr);
                pCmdLst->ClearDepthStencilView(itemDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                CD3DX12_VIEWPORT itemViewport(0.0f, 0.0f, (float)m_itemWidth, (float)m_itemHeight);
                CD3DX12_RECT itemScissor(0, 0, m_itemWidth, m_itemHeight);
                pCmdLst->RSSetViewports(1, &itemViewport);
                pCmdLst->RSSetScissorRects(1, &itemScissor);

                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(m_pItemPrepassPSO);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, drawSurfelAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(2, drawSurfelAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer ? m_pOcclusionVoxelBuffer->GetGPUVirtualAddress() : 0);

                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> itemCmd6;
                pCmdLst->QueryInterface(IID_PPV_ARGS(&itemCmd6));
                uint32_t totalItemGroups = (pState->useChunkedPipeline && pState->chunkCount > 0) ? pState->chunkCount : ((surfelCount + SURFELS_PER_GROUP - 1) / SURFELS_PER_GROUP);
                uint32_t dimX = std::min(totalItemGroups, 32768u);
                uint32_t dimY = (dimX > 0) ? ((totalItemGroups + dimX - 1) / dimX) : 1;
                itemCmd6->DispatchMesh(dimX, dimY, 1);

                // 2. Transition ItemBuffer -> ALL_SHADER_RESOURCE, ItemDepthBuffer -> ALL_SHADER_RESOURCE
                D3D12_RESOURCE_BARRIER midBarriers[2] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(m_itemBuffer.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(m_itemDepthBuffer.GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
                };
                pCmdLst->ResourceBarrier(2, midBarriers);

                // 3. Dispatch Edge Extraction Compute Shader
                struct EdgeExtractCB
                {
                    uint32_t screenWidth;
                    uint32_t screenHeight;
                    float    depthThreshold;
                    uint32_t totalChunks;
                    uint32_t bitmaskDwordCount;
                    uint32_t exteriorOnly;
                    float    pad[2];
                };

                EdgeExtractCB* pEdgeCB = nullptr;
                D3D12_GPU_VIRTUAL_ADDRESS edgeCbAddr = 0;
                if (m_constantBufferRing.AllocConstantBuffer(sizeof(EdgeExtractCB), (void**)&pEdgeCB, &edgeCbAddr))
                {
                    pEdgeCB->screenWidth = m_itemWidth;
                    pEdgeCB->screenHeight = m_itemHeight;
                    pEdgeCB->depthThreshold = pState->silhouetteDepthThreshold;
                    pEdgeCB->totalChunks = pState->chunkCount;
                    pEdgeCB->bitmaskDwordCount = bitmaskDwords;
                    pEdgeCB->exteriorOnly = pState->silhouetteExteriorOnly ? 1 : 0;

                    // Clear bitmask via dedicated 64-thread compute shader
                    m_clearBitmaskCS.Draw(pCmdLst, edgeCbAddr, &m_silhouetteBitmaskUAV, nullptr, (bitmaskDwords + 63) / 64, 1, 1);

                    D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_pSilhouetteBitmaskGpuBuffer);
                    pCmdLst->ResourceBarrier(1, &uavBarrier);

                    m_silhouetteEdgeExtractCS.Draw(pCmdLst, edgeCbAddr, &m_silhouetteBitmaskUAV, &m_itemTableSRVs, (m_itemWidth + 15) / 16, (m_itemHeight + 15) / 16, 1);
                    pCmdLst->ResourceBarrier(1, &uavBarrier);

                    // 4. Copy Bitmask to Readback Buffer
                    D3D12_RESOURCE_BARRIER copyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pSilhouetteBitmaskGpuBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    pCmdLst->ResourceBarrier(1, &copyBarrier);

                    pCmdLst->CopyBufferRegion(m_pSilhouetteReadbackBuffer, 0, m_pSilhouetteBitmaskGpuBuffer, 0, bitmaskDwords * sizeof(uint32_t));

                    D3D12_RESOURCE_BARRIER restoreBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pSilhouetteBitmaskGpuBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    pCmdLst->ResourceBarrier(1, &restoreBarrier);
                }

                m_lastEdgeViewProj = pCB->viewProj;
                m_lastEdgeChunkCount = pState->chunkCount;
                m_lastEdgeDepthThreshold = pState->silhouetteDepthThreshold;
                m_lastEdgeExteriorOnly = pState->silhouetteExteriorOnly;
                m_edgeBitmaskValid = true;

                // Restore Main Render Target & Viewport
                pCmdLst->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
                pCmdLst->RSSetViewports(1, &viewport);
                pCmdLst->RSSetScissorRects(1, &scissor);

                auto silhouettePrepassEnd = std::chrono::high_resolution_clock::now();
                m_metrics.silhouettePrepassTimeMs = std::chrono::duration<float, std::milli>(silhouettePrepassEnd - silhouettePrepassStart).count();
            }

            DrawOccluderPass();

            // Draw Main Mesh Shader Pass
            if (!pState->showOcclusionVolumeOnly)
            {
                auto mainDispatchStart2 = std::chrono::high_resolution_clock::now();

                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(pMainSplatPSO);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, drawSurfelAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(2, drawSurfelAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer ? m_pOcclusionVoxelBuffer->GetGPUVirtualAddress() : 0);

                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
                pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
                uint32_t groupCount = (pState->useChunkedPipeline && pState->chunkCount > 0)
                    ? ((pState->chunkCount + 32 - 1) / 32)
                    : ((surfelCount + SURFELS_PER_GROUP - 1) / SURFELS_PER_GROUP);

                if (pState->chunkCount == 0 || groupCount == 0)
                {
                    LogTransitionTrace("PreprocessRenderer::OnRender WARNING: chunkCount=%u, surfelCount=%u, groupCount=%u, useChunkedPipeline=%d",
                        pState->chunkCount, surfelCount, groupCount, pState->useChunkedPipeline ? 1 : 0);
                }

                cmdList6->DispatchMesh(groupCount, 1, 1);

                auto mainDispatchEnd2 = std::chrono::high_resolution_clock::now();
                m_metrics.mainDispatchTimeMs += std::chrono::duration<float, std::milli>(mainDispatchEnd2 - mainDispatchStart2).count();
            }
        }
        auto dispatchEnd = std::chrono::high_resolution_clock::now();
        m_metrics.gpuDispatchTimeMs = std::chrono::duration<float, std::milli>(dispatchEnd - dispatchStart).count();

        // -------------------------------------------------------------------------
        // Temporal Anti-Aliasing (TAA) & Dither Transition Resolver Post-Process
        // -------------------------------------------------------------------------
        if (pState->enableTemporalFiltering && m_pTemporalPSO != nullptr && m_pTemporalRootSig != nullptr)
        {
            auto taaStart = std::chrono::high_resolution_clock::now();

            // Transition resources for Temporal Filter compute pass
            D3D12_RESOURCE_BARRIER preTemporalBarriers[4] = {};
            preTemporalBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_sceneColorBuffer.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            preTemporalBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            preTemporalBarriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(m_historyColorBuffer.GetResource(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            preTemporalBarriers[3] = CD3DX12_RESOURCE_BARRIER::Transition(m_resolvedColorBuffer.GetResource(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            pCmdLst->ResourceBarrier(4, preTemporalBarriers);

            struct TemporalCB
            {
                XMFLOAT4X4 curViewProj;
                XMFLOAT4X4 prevViewProj;
                XMFLOAT4X4 invCurViewProj;
                uint32_t   screenWidth;
                uint32_t   screenHeight;
                float      blendWeight;
                uint32_t   enableClamping;
                uint32_t   firstFrame;
                XMFLOAT3   pad;
            };

            TemporalCB* pTempCB = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS tempCbAddr = 0;
            if (m_constantBufferRing.AllocConstantBuffer(sizeof(TemporalCB), (void**)&pTempCB, &tempCbAddr))
            {
                XMStoreFloat4x4(&pTempCB->curViewProj, unjitteredViewProj);
                pTempCB->prevViewProj = m_temporalFirstFrame ? pTempCB->curViewProj : m_prevViewProj;
                XMMATRIX invCurVP = XMMatrixInverse(nullptr, unjitteredViewProj);
                XMStoreFloat4x4(&pTempCB->invCurViewProj, invCurVP);
                pTempCB->screenWidth = m_width;
                pTempCB->screenHeight = m_height;
                pTempCB->blendWeight = std::clamp(pState->temporalBlendWeight, 0.01f, 1.0f);
                pTempCB->enableClamping = pState->enableVarianceClamping ? 1 : 0;
                pTempCB->firstFrame = m_temporalFirstFrame ? 1 : 0;

                pCmdLst->SetComputeRootSignature(m_pTemporalRootSig);
                pCmdLst->SetPipelineState(m_pTemporalPSO);
                pCmdLst->SetComputeRootConstantBufferView(0, tempCbAddr);
                pCmdLst->SetComputeRootDescriptorTable(1, m_temporalTableSRVs.GetGPU());
                pCmdLst->SetComputeRootDescriptorTable(2, m_temporalTableUAVs.GetGPU());

                uint32_t groupsX = (m_width + 7) / 8;
                uint32_t groupsY = (m_height + 7) / 8;
                pCmdLst->Dispatch(groupsX, groupsY, 1);
            }

            // Post-temporal transitions: Blit resolved output to backbuffer
            D3D12_RESOURCE_BARRIER postTemporalBarriers[5] = {};
            postTemporalBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_resolvedColorBuffer.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            postTemporalBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            postTemporalBarriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(m_historyColorBuffer.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            postTemporalBarriers[3] = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            postTemporalBarriers[4] = CD3DX12_RESOURCE_BARRIER::Transition(m_sceneColorBuffer.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            pCmdLst->ResourceBarrier(5, postTemporalBarriers);

            pCmdLst->CopyResource(pBackBuffer, m_resolvedColorBuffer.GetResource());

            D3D12_RESOURCE_BARRIER toRtv = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
            pCmdLst->ResourceBarrier(1, &toRtv);

            D3D12_RESOURCE_BARRIER resToSrv = CD3DX12_RESOURCE_BARRIER::Transition(m_resolvedColorBuffer.GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            pCmdLst->ResourceBarrier(1, &resToSrv);

            XMStoreFloat4x4(&m_prevViewProj, unjitteredViewProj);
            m_temporalFirstFrame = false;

            auto taaEnd = std::chrono::high_resolution_clock::now();
            m_metrics.taaResolveTimeMs = std::chrono::duration<float, std::milli>(taaEnd - taaStart).count();
        }
        else
        {
            m_temporalFirstFrame = true;
        }

        // Set backbuffer as render target for ImGui UI overlay
        D3D12_CPU_DESCRIPTOR_HANDLE backRtv = *pSwapChain->GetCurrentBackBufferRTV();
        pCmdLst->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);

        // Draw ImGui UI on top
        auto uiStart = std::chrono::high_resolution_clock::now();
        m_imGui.Draw(pCmdLst);
        auto uiEnd = std::chrono::high_resolution_clock::now();
        m_metrics.uiDrawTimeMs = std::chrono::duration<float, std::milli>(uiEnd - uiStart).count();

        {
            D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            pCmdLst->ResourceBarrier(1, &toPresent);
        }

        pCmdLst->Close();

        ID3D12CommandList* pCmdLists[] = { pCmdLst };
        m_pDevice->GetGraphicsQueue()->ExecuteCommandLists(1, pCmdLists);

        auto now = std::chrono::high_resolution_clock::now();
        if (m_lastWallClockTime.time_since_epoch().count() == 0)
        {
            m_lastWallClockTime = now;
            m_metrics.frameRate = 60.0f;
            m_metrics.totalFrameTimeMs = 16.6f;
        }
        else
        {
            float wallDeltaMs = std::chrono::duration<float, std::milli>(now - m_lastWallClockTime).count();
            m_lastWallClockTime = now;

            if (wallDeltaMs > 0.0f && wallDeltaMs < 1000.0f)
            {
                m_fpsAccumTimeMs += wallDeltaMs;
                m_fpsAccumFrames++;
            }

            // Update displayed FPS every 200ms or 20 frames for instant responsiveness and zero jitter
            if (m_fpsAccumTimeMs >= 200.0f || m_fpsAccumFrames >= 20)
            {
                if (m_fpsAccumTimeMs > 0.001f && m_fpsAccumFrames > 0)
                {
                    m_metrics.frameRate = (float)m_fpsAccumFrames * 1000.0f / m_fpsAccumTimeMs;
                    m_metrics.totalFrameTimeMs = m_fpsAccumTimeMs / (float)m_fpsAccumFrames;
                }
                m_fpsAccumTimeMs = 0.0f;
                m_fpsAccumFrames = 0;
            }
        }

        // EMA smoothing for stage breakdowns
        if (m_metrics.wasSortedThisFrame)
        {
            m_smoothGpuSortMs = (m_smoothGpuSortMs > 0.0001f) ? (m_smoothGpuSortMs * 0.8f + m_metrics.gpuSortTimeMs * 0.2f) : m_metrics.gpuSortTimeMs;
        }
        m_smoothDispatchMs = (m_smoothDispatchMs > 0.0001f) ? (m_smoothDispatchMs * 0.85f + m_metrics.gpuDispatchTimeMs * 0.15f) : m_metrics.gpuDispatchTimeMs;
        m_smoothUiMs = (m_smoothUiMs > 0.0001f) ? (m_smoothUiMs * 0.85f + m_metrics.uiDrawTimeMs * 0.15f) : m_metrics.uiDrawTimeMs;

        // Upload and silhouette prepass are cached/sporadic (only re-run when dirty or the view/edge
        // cache invalidates) -- only feed the EMA on the frames they actually ran, holding the last
        // representative cost otherwise, same as m_smoothGpuSortMs above.
        if (m_metrics.uploadTimeMs > 0.0001f)
            m_smoothUploadMs = (m_smoothUploadMs > 0.0001f) ? (m_smoothUploadMs * 0.85f + m_metrics.uploadTimeMs * 0.15f) : m_metrics.uploadTimeMs;
        if (m_metrics.silhouettePrepassTimeMs > 0.0001f)
            m_smoothSilhouettePrepassMs = (m_smoothSilhouettePrepassMs > 0.0001f) ? (m_smoothSilhouettePrepassMs * 0.85f + m_metrics.silhouettePrepassTimeMs * 0.15f) : m_metrics.silhouettePrepassTimeMs;

        // Occluder pass and main dispatch run every frame they're active, and TAA resolve runs every
        // frame it's enabled -- plain EMA so they decay toward 0 when that stage turns off.
        m_smoothOccluderMs = (m_smoothOccluderMs > 0.0001f) ? (m_smoothOccluderMs * 0.85f + m_metrics.occluderPassTimeMs * 0.15f) : m_metrics.occluderPassTimeMs;
        m_smoothMainDispatchMs = (m_smoothMainDispatchMs > 0.0001f) ? (m_smoothMainDispatchMs * 0.85f + m_metrics.mainDispatchTimeMs * 0.15f) : m_metrics.mainDispatchTimeMs;
        m_smoothTaaMs = (m_smoothTaaMs > 0.0001f) ? (m_smoothTaaMs * 0.85f + m_metrics.taaResolveTimeMs * 0.15f) : m_metrics.taaResolveTimeMs;
    }
}
