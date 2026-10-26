// SplatRenderer.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The GPU heart of SplatLab: root signatures, PSOs, and per-frame command
// recording for the main mesh-shader splat pass, the GPU silhouette item-prepass, the
// interior occlusion volume pass, the GPU bitonic depth sort, and TAA resolve.

#include "SplatRenderer.h"
#include <d3dx12.h>
#include "Misc/Error.h"

namespace Splat
{
    using namespace CAULDRON_DX12;

    static const uint32_t BACK_BUFFER_COUNT = 2;
    static const uint32_t SPLATS_PER_GROUP = 64;
    static const uint32_t AS_GROUP_SIZE = 32;

    // Splat packages: what is uploaded versus which pipeline draws it. Modes 3 and 4 both upload the
    // 20-byte Gaussian records and the SH stream (State::pSplats, pSplatSH); mode 3 draws them through
    // the sorted Gaussian pipeline (RunSplatSort, premultiplied blend PSOs), mode 4 through the splat
    // pipeline as opaque discs (chunk sort, splat PSOs, item prepass, Detach Camera passes).
    static bool SplatRecords(uint32_t renderMode) { return renderMode == 3 || renderMode == 4; }
    static bool SortedGaussians(uint32_t renderMode) { return renderMode == 3; }

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
    // and the dedicated copy queue used for async splat/chunk uploads.
    static const char* g_appTitle = "SplatLab";
    void SplatRenderer::SetAppTitle(const char* title) { g_appTitle = (title && *title) ? title : "SplatLab"; }

    void SplatRenderer::OnCreate(Device* pDevice, SwapChain* pSwapChain)
    {
        m_pDevice = pDevice;

        {
            // GPU capabilities decide the render path (see RenderPath): mesh shaders when the hardware has
            // them, otherwise the instanced vertex-shader fallback -- compiled for Shader Model 6.0 where
            // DXIL is supported and through the legacy compiler as Shader Model 5.1 where it is not. This
            // runs before any shader is compiled (the UI's come first) so the compiler choice applies to all.
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
            HRESULT hr7 = pDevice->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
            m_gpuCaps.meshShaders = SUCCEEDED(hr7) && options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;

            D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_5 };
            if (FAILED(pDevice->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
            {
                sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
                if (FAILED(pDevice->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
                    sm.HighestShaderModel = D3D_SHADER_MODEL_5_1;
            }
            m_gpuCaps.highestShaderModel = (uint32_t)sm.HighestShaderModel;
            m_gpuCaps.shaderModel6 = sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0;

            const RenderPath autoPath = (m_gpuCaps.meshShaders && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5) ? RenderPath::MeshShaders
                                      : (m_gpuCaps.shaderModel6 ? RenderPath::VertexShadersSM6 : RenderPath::VertexShadersSM5);
            m_renderPath = autoPath;
            m_renderPathForced = false;
            if (m_renderPathOverride >= 0 && m_renderPathOverride <= 2)
            {
                const RenderPath wanted = (RenderPath)m_renderPathOverride;
                const bool runnable = (wanted == RenderPath::MeshShaders) ? (autoPath == RenderPath::MeshShaders)
                                    : (wanted == RenderPath::VertexShadersSM6) ? m_gpuCaps.shaderModel6 : true;
                if (runnable && wanted != autoPath)
                {
                    m_renderPath = wanted;
                    m_renderPathForced = true;
                }
            }
            SetLegacyShaderCompiler(m_renderPath == RenderPath::VertexShadersSM5);
            LogTransitionTrace("SplatRenderer::OnCreate GPU: mesh shaders %s, highest shader model 0x%02X -> %s%s",
                m_gpuCaps.meshShaders ? "yes" : "no", m_gpuCaps.highestShaderModel, GetRenderPathDescription(), m_renderPathForced ? " (forced by config)" : "");
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
        m_resourceViewHeaps.AllocRTVDescriptor(1, &m_sceneColorRTVUnorm);
        m_resourceViewHeaps.AllocRTVDescriptor(1, &m_loupeRTV);
        m_resourceViewHeaps.AllocRTVDescriptor(1, &m_loupeRTVUnorm);
        m_resourceViewHeaps.AllocDSVDescriptor(1, &m_loupeDSV);
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

        CD3DX12_ROOT_PARAMETER rootParams[10];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // b0 - SplatCB
        rootParams[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // t0 - g_PointBuffer
        rootParams[2].InitAsShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_ALL); // t1 - g_RawSplatBuffer
        rootParams[3].InitAsShaderResourceView(2, 0, D3D12_SHADER_VISIBILITY_ALL); // t2 - g_ChunkBuffer
        rootParams[4].InitAsShaderResourceView(3, 0, D3D12_SHADER_VISIBILITY_ALL); // t3 - g_SortedChunkIndices
        rootParams[5].InitAsShaderResourceView(4, 0, D3D12_SHADER_VISIBILITY_ALL); // t4 - g_OcclusionVoxelBuffer
        rootParams[6].InitAsShaderResourceView(5, 0, D3D12_SHADER_VISIBILITY_ALL); // t5 - g_SplatBuffer (splat mode)
        rootParams[7].InitAsShaderResourceView(6, 0, D3D12_SHADER_VISIBILITY_ALL); // t6 - g_SHBuffer
        rootParams[8].InitAsShaderResourceView(7, 0, D3D12_SHADER_VISIBILITY_ALL); // t7 - g_SortedSplats
        rootParams[9].InitAsShaderResourceView(8, 0, D3D12_SHADER_VISIBILITY_ALL); // t8 - g_SlotInfo

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 10;
        rsDesc.pParameters = rootParams;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
        D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);
        m_pDevice->GetDevice()->CreateRootSignature(0, pOutBlob->GetBufferPointer(), pOutBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));

        // Create Compute Root Signature & Compute Pipeline States (GPU LDS Key-Index Sorting)
        CD3DX12_ROOT_PARAMETER computeRootParams[8];
        computeRootParams[0].InitAsConstantBufferView(0, 0); // b0 - BitonicSortCB
        computeRootParams[1].InitAsShaderResourceView(0, 0); // t0 - InPackedSplats
        computeRootParams[2].InitAsShaderResourceView(1, 0); // t1 - InRawSplats
        computeRootParams[3].InitAsShaderResourceView(2, 0); // t2 - InChunks
        computeRootParams[4].InitAsUnorderedAccessView(0, 0); // u0 - SortPairs
        computeRootParams[5].InitAsUnorderedAccessView(1, 0); // u1 - OutPackedSplats
        computeRootParams[6].InitAsUnorderedAccessView(2, 0); // u2 - OutRawSplats
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
        CreateComputePSO("GatherSplatsCS", &m_pGatherSplatsPSO);
        CreateComputePSO("GatherChunkIndicesCS", &m_pGatherChunkIndicesPSO);

        // Splat mode: the render-target formats of its two blend spaces, and the per-splat depth sort.
        {
            const DXGI_FORMAT swapFmt = pSwapChain->GetFormat();
            m_srgbFormat = swapFmt;
            m_unormFormat = (swapFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM
                          : (swapFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM
                          : swapFmt;

            CD3DX12_ROOT_PARAMETER sortParams[8];
            sortParams[0].InitAsConstantBufferView(0, 0);   // b0 - SplatSortCB
            sortParams[1].InitAsShaderResourceView(0, 0);   // t0 - splats
            sortParams[2].InitAsShaderResourceView(1, 0);   // t1 - chunks
            sortParams[3].InitAsShaderResourceView(2, 0);   // t2 - chunk slot bases
            sortParams[4].InitAsUnorderedAccessView(0, 0);  // u0 - slot info
            sortParams[5].InitAsUnorderedAccessView(1, 0);  // u1 - pairs in
            sortParams[6].InitAsUnorderedAccessView(2, 0);  // u2 - pairs out
            sortParams[7].InitAsUnorderedAccessView(3, 0);  // u3 - histogram
            CD3DX12_ROOT_SIGNATURE_DESC sortRs = {};
            sortRs.NumParameters = 8;
            sortRs.pParameters = sortParams;
            Microsoft::WRL::ComPtr<ID3DBlob> pSortBlob, pSortErr;
            if (SUCCEEDED(D3D12SerializeRootSignature(&sortRs, D3D_ROOT_SIGNATURE_VERSION_1, &pSortBlob, &pSortErr)))
                m_pDevice->GetDevice()->CreateRootSignature(0, pSortBlob->GetBufferPointer(), pSortBlob->GetBufferSize(), IID_PPV_ARGS(&m_pSplatSortRootSig));

            auto CreateSortPSO = [&](const char* entryPoint, ID3D12PipelineState** ppPSO)
            {
                D3D12_SHADER_BYTECODE cs = {};
                if (m_pSplatSortRootSig != nullptr && CompileShaderFromFile("SplatSortCS.hlsl", NULL, entryPoint, "-T cs_6_0", &cs) && cs.pShaderBytecode != nullptr)
                {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
                    psoDesc.pRootSignature = m_pSplatSortRootSig;
                    psoDesc.CS = cs;
                    m_pDevice->GetDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(ppPSO));
                }
                else
                {
                    LogTransitionTrace("SplatRenderer::OnCreate ERROR: SplatSortCS.hlsl (%s) did not compile -- splat mode will draw in chunk order.", entryPoint);
                }
            };
            CreateSortPSO("ExpandCS", &m_pSplatExpandPSO);
            CreateSortPSO("HistogramCS", &m_pSplatHistPSO);
            CreateSortPSO("ScanCS", &m_pSplatScanPSO);
            CreateSortPSO("ScatterCS", &m_pSplatScatterPSO);

            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.NumDescriptors = 1;
            m_pDevice->GetDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_pBackBufferUnormRtvHeap));
        }

        if (m_renderPath == RenderPath::MeshShaders)
        {
        D3D12_SHADER_BYTECODE as = {};
        D3D12_SHADER_BYTECODE ms = {};
        D3D12_SHADER_BYTECODE ps = {};
        CompileShaderFromFile("Splat.hlsl", NULL, "mainAS", "-T as_6_5", &as);
        CompileShaderFromFile("Splat.hlsl", NULL, "mainMS", "-T ms_6_5", &ms);
        CompileShaderFromFile("Splat.hlsl", NULL, "mainPS", "-T ps_6_5", &ps);

        CD3DX12_RASTERIZER_DESC rasterizer(D3D12_DEFAULT);
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;

        // 1. Fast Splat Blending Pipeline State (Depth Testing Disabled for maximum >200 FPS fillrate)
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
        HRESULT hrMainPSO = device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&m_pPipelineState));

        // Create Item Prepass Pipeline State Object (itemMS & itemPS with DepthWrite ON, RTV: R32_UINT, DSV: D32_FLOAT)
        D3D12_SHADER_BYTECODE itemMs = {};
        D3D12_SHADER_BYTECODE itemPs = {};
        CompileShaderFromFile("Splat.hlsl", NULL, "itemMS", "-T ms_6_5", &itemMs);
        CompileShaderFromFile("Splat.hlsl", NULL, "itemPS", "-T ps_6_5", &itemPs);

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
        HRESULT hrItemPSO = device2->CreatePipelineState(&itemStreamDesc, IID_PPV_ARGS(&m_pItemPrepassPSO));

        // Create Occluder Pipeline State Object (occluderMS & occluderPS): solid depth-writing interior
        // occlusion volume cubes, drawn into the main color+depth target before the splat pass. Same
        // direct-dispatch (no AS stage) shape as the item prepass above.
        D3D12_SHADER_BYTECODE occluderMs = {};
        D3D12_SHADER_BYTECODE occluderPs = {};
        bool occluderShadersOk = CompileShaderFromFile("Splat.hlsl", NULL, "occluderMS", "-T ms_6_5", &occluderMs) && occluderMs.pShaderBytecode != nullptr;
        occluderShadersOk = (CompileShaderFromFile("Splat.hlsl", NULL, "occluderPS", "-T ps_6_5", &occluderPs) && occluderPs.pShaderBytecode != nullptr) && occluderShadersOk;
        if (!occluderShadersOk)
        {
            LogTransitionTrace("SplatRenderer::OnCreate ERROR: Failed to compile occluderMS/occluderPS -- occlusion volume feature will be a silent no-op.");
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
            LogTransitionTrace("SplatRenderer::OnCreate ERROR: CreatePipelineState(occluder) failed hr=0x%08X -- occlusion volume feature will be a silent no-op.", (unsigned int)hrOccluderPSO);
        }
        {
            // Splat mode with display-space blending draws into a plain UNORM view; the occluder that
            // writes depth for it needs a matching render-target format.
            const DXGI_FORMAT swapFmtNow = pSwapChain->GetFormat();
            const DXGI_FORMAT unormNow = (swapFmtNow == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM
                                       : (swapFmtNow == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM : swapFmtNow;
            MeshShaderPipelineStateStream occluderUnorm = occluderStream;
            D3D12_RT_FORMAT_ARRAY unormFormats = {};
            unormFormats.NumRenderTargets = 1;
            unormFormats.RTFormats[0] = unormNow;
            occluderUnorm.RTVFormats = unormFormats;
            D3D12_PIPELINE_STATE_STREAM_DESC occluderUnormDesc = {};
            occluderUnormDesc.SizeInBytes = sizeof(occluderUnorm);
            occluderUnormDesc.pPipelineStateSubobjectStream = &occluderUnorm;
            if (FAILED(device2->CreatePipelineState(&occluderUnormDesc, IID_PPV_ARGS(&m_pOccluderPSOUnorm))))
                LogTransitionTrace("SplatRenderer::OnCreate ERROR: CreatePipelineState(occluder UNORM) failed -- splat mode draws without the occlusion volume when blending in display space.");
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
            LogTransitionTrace("SplatRenderer::OnCreate ERROR: CreatePipelineState(occlusionTest) failed hr=0x%08X -- occlusion culling will silently do nothing on the main splat pass.", (unsigned int)hrOcclusionTestPSO);
        }

        // Detach Camera variants (see the member comments in SplatRenderer.h).
        {
            CD3DX12_DEPTH_STENCIL_DESC dsMain(D3D12_DEFAULT);
            dsMain.DepthEnable = TRUE; dsMain.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; dsMain.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; dsMain.StencilEnable = FALSE;
            CD3DX12_DEPTH_STENCIL_DESC dsDepthOnly(D3D12_DEFAULT);
            dsDepthOnly.DepthEnable = TRUE; dsDepthOnly.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; dsDepthOnly.DepthFunc = D3D12_COMPARISON_FUNC_LESS; dsDepthOnly.StencilEnable = FALSE;
            CD3DX12_DEPTH_STENCIL_DESC dsEqual(D3D12_DEFAULT);
            dsEqual.DepthEnable = TRUE; dsEqual.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; dsEqual.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL; dsEqual.StencilEnable = FALSE;
            CD3DX12_BLEND_DESC noColour(D3D12_DEFAULT);
            noColour.RenderTarget[0].RenderTargetWriteMask = 0;

            auto makeVariant = [&](const CD3DX12_DEPTH_STENCIL_DESC& ds, const CD3DX12_BLEND_DESC& blend, ID3D12PipelineState** ppOut, const char* name)
            {
                MeshShaderPipelineStateStream v = stream;
                v.DepthStencilState = ds;
                v.BlendState = blend;
                D3D12_PIPELINE_STATE_STREAM_DESC vd = {};
                vd.SizeInBytes = sizeof(v);
                vd.pPipelineStateSubobjectStream = &v;
                HRESULT hr = device2->CreatePipelineState(&vd, IID_PPV_ARGS(ppOut));
                if (FAILED(hr))
                    LogTransitionTrace("SplatRenderer::OnCreate ERROR: CreatePipelineState(%s) failed hr=0x%08X -- Detach Camera falls back to the plain splat pass.", name, (unsigned int)hr);
            };
            makeVariant(dsMain, blendDesc, &m_pPipelineStateDetachedMain, "detached main");
            makeVariant(dsDepthOnly, noColour, &m_pPipelineStateCulledDepth, "culled depth");
            makeVariant(dsEqual, blendDesc, &m_pPipelineStateCulledColor, "culled colour");
        }

        // Splat mode PSOs: the fast (no depth) blend state on each of the two render-target formats.
        {
            auto makeSplat = [&](DXGI_FORMAT fmt, ID3D12PipelineState** ppOut, const char* name)
            {
                MeshShaderPipelineStateStream v = stream;
                v.DepthStencilState = depthStencilFast;
                v.BlendState = blendDesc;
                D3D12_RT_FORMAT_ARRAY fmts = {};
                fmts.NumRenderTargets = 1;
                fmts.RTFormats[0] = fmt;
                v.RTVFormats = fmts;
                D3D12_PIPELINE_STATE_STREAM_DESC vd = {};
                vd.SizeInBytes = sizeof(v);
                vd.pPipelineStateSubobjectStream = &v;
                HRESULT hr = device2->CreatePipelineState(&vd, IID_PPV_ARGS(ppOut));
                if (FAILED(hr))
                    LogTransitionTrace("SplatRenderer::OnCreate ERROR: CreatePipelineState(%s) failed hr=0x%08X -- splat mode unavailable on this path.", name, (unsigned int)hr);
            };
            makeSplat(m_unormFormat, &m_pSplatPSODisplay, "splat display");
            makeSplat(m_srgbFormat, &m_pSplatPSOLinear, "splat linear");
            // Depth-tested variants (no write): splats behind the interior occlusion volume are rejected
            auto makeSplatDepth = [&](DXGI_FORMAT fmt, ID3D12PipelineState** ppOut, const char* name)
            {
                MeshShaderPipelineStateStream v = stream;
                v.DepthStencilState = depthStencilOcclusionTest;
                v.BlendState = blendDesc;
                D3D12_RT_FORMAT_ARRAY fmts = {};
                fmts.NumRenderTargets = 1;
                fmts.RTFormats[0] = fmt;
                v.RTVFormats = fmts;
                D3D12_PIPELINE_STATE_STREAM_DESC vd = {};
                vd.SizeInBytes = sizeof(v);
                vd.pPipelineStateSubobjectStream = &v;
                if (FAILED(device2->CreatePipelineState(&vd, IID_PPV_ARGS(ppOut))))
                    LogTransitionTrace("SplatRenderer::OnCreate ERROR: CreatePipelineState(%s) failed -- splat mode will not depth-test against the occlusion volume.", name);
            };
            makeSplatDepth(m_unormFormat, &m_pSplatPSODisplayDepth, "splat display depth");
            makeSplatDepth(m_srgbFormat, &m_pSplatPSOLinearDepth, "splat linear depth");
        }

        if (FAILED(hrMainPSO) || FAILED(hrItemPSO) || m_pPipelineState == nullptr || m_pItemPrepassPSO == nullptr)
        {
            // The driver reported mesh shaders but could not build the pipelines (typically a shader
            // model it cannot actually compile): fall back to the vertex-shader path instead of running
            // with null pipeline states.
            LogTransitionTrace("SplatRenderer::OnCreate ERROR: mesh-shader pipeline creation failed (main hr=0x%08X, item hr=0x%08X) -- falling back to vertex shaders", (unsigned int)hrMainPSO, (unsigned int)hrItemPSO);
            if (m_pPipelineState) { m_pPipelineState->Release(); m_pPipelineState = nullptr; }
            if (m_pPipelineStateOcclusionTest) { m_pPipelineStateOcclusionTest->Release(); m_pPipelineStateOcclusionTest = nullptr; }
            if (m_pPipelineStateDetachedMain) { m_pPipelineStateDetachedMain->Release(); m_pPipelineStateDetachedMain = nullptr; }
            if (m_pPipelineStateCulledDepth) { m_pPipelineStateCulledDepth->Release(); m_pPipelineStateCulledDepth = nullptr; }
            if (m_pPipelineStateCulledColor) { m_pPipelineStateCulledColor->Release(); m_pPipelineStateCulledColor = nullptr; }
            if (m_pSplatPSODisplay) { m_pSplatPSODisplay->Release(); m_pSplatPSODisplay = nullptr; }
            if (m_pSplatPSOLinear) { m_pSplatPSOLinear->Release(); m_pSplatPSOLinear = nullptr; }
            if (m_pSplatPSODisplayDepth) { m_pSplatPSODisplayDepth->Release(); m_pSplatPSODisplayDepth = nullptr; }
            if (m_pSplatPSOLinearDepth) { m_pSplatPSOLinearDepth->Release(); m_pSplatPSOLinearDepth = nullptr; }
            if (m_pOccluderPSOUnorm) { m_pOccluderPSOUnorm->Release(); m_pOccluderPSOUnorm = nullptr; }
            if (m_pItemPrepassPSO) { m_pItemPrepassPSO->Release(); m_pItemPrepassPSO = nullptr; }
            if (m_pOccluderPSO) { m_pOccluderPSO->Release(); m_pOccluderPSO = nullptr; }
            m_gpuCaps.meshPipelineFailed = true;
            m_renderPath = m_gpuCaps.shaderModel6 ? RenderPath::VertexShadersSM6 : RenderPath::VertexShadersSM5;
        }
        }
        CreateShadowMeshPipelines(pSwapChain); // Classic graphics pipeline: the same on every render path
        if (m_renderPath != RenderPath::MeshShaders)
        {
            CreateVertexShaderPipelines(pSwapChain);

            // Say so up front, in a Windows dialog, so nobody mistakes the slower fallback for the real
            // thing: which stages this GPU path is doing without and what that means.
            const bool forced = m_renderPathForced;
            const char* why = forced ? "bypassed by \"render_path\" in config.json" : (m_gpuCaps.meshPipelineFailed ? "reported by the driver, but its pipelines could not be created" : "not provided by this GPU or driver");
            char msg[1024];
            snprintf(msg, sizeof(msg),
                "%s is running without DirectX 12 mesh shaders (Shader Model 6.5).\n\n"
                "Hardware stages not in use (%s):\n"
                "    - Amplification (task) shader stage\n"
                "    - Mesh shader stage\n"
                "%s"
                "\nRender path: %s.\n\n"
                "The picture is the same, but frame rates are lower. The banner at the top left of the viewport "
                "lists the stages the fallback is standing in for while it is active.%s",
                g_appTitle,
                why,
                (m_renderPath == RenderPath::VertexShadersSM5) ? "    - Shader Model 6 (DXIL) shader compiler\n" : "",
                GetRenderPathDescription(),
                forced ? "\n\nSet \"render_path\" to \"auto\" in config.json to use the best path this GPU supports." : "\n\nFor full performance use a GPU with D3D12 Mesh Shader Tier 1 (GeForce RTX 20 series or newer, Radeon RX 6000 or newer, Intel Arc) and a current driver.");
            char title[128];
            snprintf(title, sizeof(title), "%s - Mesh shader fallback engaged", g_appTitle);
            if (!m_quietStartup) MessageBoxA(NULL, msg, title, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
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

        if (m_renderPath == RenderPath::MeshShaders)
            m_pDevice->GetDevice()->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&m_pCommandSignature));

        // Create Dedicated DX12 Hardware DMA Copy Queue for asynchronous PCIe transfers
        D3D12_COMMAND_QUEUE_DESC copyQueueDesc = {};
        copyQueueDesc.Type     = D3D12_COMMAND_LIST_TYPE_COPY;
        copyQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        copyQueueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

        if (SUCCEEDED(m_pDevice->GetDevice()->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&m_pCopyQueue))))
        {
            SetName(m_pCopyQueue, "SplatRenderer::m_pCopyQueue");
            m_pDevice->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_pCopyAllocator));
            SetName(m_pCopyAllocator, "SplatRenderer::m_pCopyAllocator");
            m_pDevice->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_pCopyAllocator, nullptr, IID_PPV_ARGS(&m_pCopyCmdList));
            SetName(m_pCopyCmdList, "SplatRenderer::m_pCopyCmdList");
            m_pCopyCmdList->Close();

            m_pDevice->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pCopyFence));
            SetName(m_pCopyFence, "SplatRenderer::m_pCopyFence");
            m_pDevice->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pGraphicsProgressFence));
            SetName(m_pGraphicsProgressFence, "SplatRenderer::m_pGraphicsProgressFence");
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

    void SplatRenderer::FlushCopyQueue()
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

    // Chooses the occlusion volume mip drawn this frame: ONE mip for the whole volume, by screen-space
    // cell size. The rule is the finest mip whose cell still projects to at least
    // State::occlusionMipMinCellPixels at the model's nearest point to the eye -- any finer and the
    // cubes are sub-pixel rasterization work that is also too tight for the big coarse-LOD splats in
    // view (a tangent disc sags below a curved surface by ~r^2 / 2R and gets clipped once that exceeds
    // the skin's erosion margin), any coarser and the volume stops hugging the surface at a size the
    // screen can still resolve. About 15% hysteresis on the switch in either direction keeps a camera
    // hovering near a threshold from flickering between mips.
    //
    // Deliberately NOT tied to the per-chunk splat LOD: the volume is global, these are single-object
    // scans, and mixing mips per chunk would open seams where a coarse block's buried face meets a
    // neighbouring chunk that eroded that cell away. A screen-space rule also sidesteps the mismatch
    // between cube size (doubles per mip) and splat spacing (~1.4x per decimation level).
    uint32_t SplatRenderer::SelectOcclusionMip(const State* pState, const OcclusionMipTable& mips, const XMFLOAT3& eyePos)
    {
        if (mips.mipCount == 0)
        {
            m_activeOcclusionMip = 0;
            m_activeOcclusionMipCellPixels = 0.0f;
            return 0;
        }

        // Distance from the eye to the nearest point of the model's bounds (0 inside; never closer than
        // the near plane, so a camera inside the model sees the finest mip).
        const XMFLOAT3& mn = pState->aabbMin;
        const XMFLOAT3 mx(mn.x + pState->aabbExtents.x, mn.y + pState->aabbExtents.y, mn.z + pState->aabbExtents.z);
        const float ddx = std::max(0.0f, std::max(mn.x - eyePos.x, eyePos.x - mx.x));
        const float ddy = std::max(0.0f, std::max(mn.y - eyePos.y, eyePos.y - mx.y));
        const float ddz = std::max(0.0f, std::max(mn.z - eyePos.z, eyePos.z - mx.z));
        const float dist = std::max(0.1f, sqrtf(ddx * ddx + ddy * ddy + ddz * ddz));

        // Pixels covered by one metre at that distance: viewport height over the frustum's visible height
        // there (vertical FOV is XM_PIDIV4 in every projection this renderer builds).
        const float pixelsPerMetre = (float)std::max(1u, m_height) / (2.0f * dist * tanf(XM_PIDIV4 * 0.5f));
        auto cellPixels = [&](uint32_t k) -> float { return mips.cellSize[k] * pixelsPerMetre; };

        uint32_t mip = std::min(m_activeOcclusionMip, mips.mipCount - 1);
        if (pState->occlusionMipOverride >= 0)
        {
            mip = std::min((uint32_t)pState->occlusionMipOverride, mips.mipCount - 1);
        }
        else
        {
            const float threshold = std::max(0.5f, pState->occlusionMipMinCellPixels);
            const float hysteresis = 1.15f;
            // Coarsen while the current mip's cell has become clearly sub-threshold ...
            while (mip + 1 < mips.mipCount && cellPixels(mip) < threshold / hysteresis) mip++;
            // ... and refine while the next finer mip's cell would be clearly resolvable.
            while (mip > 0 && cellPixels(mip - 1) >= threshold * hysteresis) mip--;
        }

        if (mip != m_activeOcclusionMip)
        {
            LogTransitionTrace("SplatRenderer: occlusion volume mip %u -> %u (%u blocks, cell %.4f m = %.1f px at %.2f m%s)",
                m_activeOcclusionMip, mip, mips.blockCount[mip], mips.cellSize[mip], cellPixels(mip), dist,
                pState->occlusionMipOverride >= 0 ? ", forced" : "");
        }
        m_activeOcclusionMip = mip;
        m_activeOcclusionMipCellPixels = cellPixels(mip);
        return mip;
    }

    // Occlusion voxel data only changes when a new dataset is loaded/exported, unlike the splat/chunk
    // buffers which churn every frame -- so unlike those, this is a plain upload-heap resource read
    // directly as an SRV rather than a default-heap buffer kept current via the copy queue.
    void SplatRenderer::UpdateOcclusionVoxelBuffer(const State* pState)
    {
        // The vector keeps its capacity across rebuilds, so a rebuild that produces the same block
        // count (a colour grade, say) leaves pointer and count unchanged -- the version counter is what
        // actually says "new data".
        if (pState->pOcclusionVoxels == m_lastOcclusionVoxelsPtr &&
            pState->occlusionVoxelCount == m_lastOcclusionVoxelCount &&
            pState->occlusionVoxelVersion == m_lastOcclusionVoxelVersion)
            return;

        m_lastOcclusionVoxelsPtr = pState->pOcclusionVoxels;
        m_lastOcclusionVoxelCount = pState->occlusionVoxelCount;
        m_lastOcclusionVoxelVersion = pState->occlusionVoxelVersion;

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
            SetName(m_pOcclusionVoxelBuffer, "SplatRenderer::m_pOcclusionVoxelBuffer");
            m_pOcclusionVoxelBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pOcclusionVoxelBufferMapped));
            m_occlusionVoxelBufferCapacityBytes = neededBytes;
        }

        if (m_pOcclusionVoxelBufferMapped)
        {
            memcpy(m_pOcclusionVoxelBufferMapped, pState->pOcclusionVoxels, neededBytes);
        }
    }

    // Releases every GPU resource/PSO/buffer created in OnCreate
    // View Shadow Mesh: ShadowMesh.hlsl's vertex and pixel shaders, opaque and depth-writing, in two render-target formats
    // like the occluder. A failure only loses the debug view.
    void SplatRenderer::CreateShadowMeshPipelines(SwapChain* pSwapChain)
    {
        D3D12_SHADER_BYTECODE vs = {}, ps = {};
        const bool ok = CompileShaderFromFile("ShadowMesh.hlsl", NULL, "shadowMeshVS", "-T vs_6_0", &vs) && vs.pShaderBytecode != nullptr &&
                        CompileShaderFromFile("ShadowMesh.hlsl", NULL, "shadowMeshPS", "-T ps_6_0", &ps) && ps.pShaderBytecode != nullptr;
        if (!ok)
        {
            LogTransitionTrace("SplatRenderer::CreateShadowMeshPipelines ERROR: ShadowMesh.hlsl failed to compile -- View Shadow Mesh will draw nothing.");
            return;
        }
        CD3DX12_RASTERIZER_DESC rasterizer(D3D12_DEFAULT);
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;
        CD3DX12_DEPTH_STENCIL_DESC depthWrite(D3D12_DEFAULT);
        depthWrite.DepthEnable = TRUE;
        depthWrite.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthWrite.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        CD3DX12_DEPTH_STENCIL_DESC depthTest(D3D12_DEFAULT);
        depthTest.DepthEnable = TRUE;
        depthTest.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depthTest.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        for (int k = 0; k < 4; k++)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
            d.pRootSignature = m_pRootSignature;
            d.VS = vs;
            d.PS = ps;
            d.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            d.SampleMask = UINT_MAX;
            d.RasterizerState = rasterizer;
            d.DepthStencilState = k < 2 ? depthWrite : depthTest;
            d.InputLayout = { nullptr, 0 };
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = (k % 2) == 0 ? pSwapChain->GetFormat() : m_unormFormat;
            d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            d.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };
            ID3D12PipelineState** out = k == 0 ? &m_pShadowMeshPSO : k == 1 ? &m_pShadowMeshPSOUnorm : k == 2 ? &m_pShadowMeshFillPSO : &m_pShadowMeshFillPSOUnorm;
            const HRESULT hr = m_pDevice->GetDevice()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(out));
            if (FAILED(hr)) LogTransitionTrace("SplatRenderer::CreateShadowMeshPipelines ERROR: PSO %d failed hr=0x%08X", k, (unsigned int)hr);
        }
    }

    void SplatRenderer::ReleaseMesh(MeshBuffers& m)
    {
        if (m.vertices) { m.vertices->Release(); m.vertices = nullptr; }
        if (m.indices) { m.indices->Release(); m.indices = nullptr; }
        m.indexCount = 0;
        m.version = UINT32_MAX;
    }

    void SplatRenderer::UploadMesh(MeshBuffers& m, const void* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, uint32_t version)
    {
        if (version == m.version) return;
        m_pDevice->GPUFlush();
        ReleaseMesh(m);
        m.version = version;
        if (vertices == nullptr || indices == nullptr || indexCount == 0) return;
        auto upload = [&](const void* data, size_t bytes, ID3D12Resource** out)
        {
            CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
            CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (FAILED(m_pDevice->GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out))))
                return false;
            void* mapped = nullptr;
            (*out)->Map(0, nullptr, &mapped);
            memcpy(mapped, data, bytes);
            (*out)->Unmap(0, nullptr);
            return true;
        };
        if (upload(vertices, (size_t)vertexCount * 16, &m.vertices) && upload(indices, (size_t)indexCount * 4, &m.indices)) m.indexCount = indexCount;
    }

    // Uploads the shadow mesh when it changes. Upload-heap buffers read directly as SRVs, like the occlusion volume's.
    void SplatRenderer::UpdateShadowMeshBuffers(const State* pState)
    {
        if (pState->shadowMeshVersion == m_shadowMeshUploadedVersion) return;
        m_shadowMeshUploadedVersion = pState->shadowMeshVersion;
        m_pDevice->GPUFlush();
        if (m_pShadowMeshVertexBuffer) { m_pShadowMeshVertexBuffer->Release(); m_pShadowMeshVertexBuffer = nullptr; }
        if (m_pShadowMeshIndexBuffer) { m_pShadowMeshIndexBuffer->Release(); m_pShadowMeshIndexBuffer = nullptr; }
        m_shadowMeshIndexCount = 0;
        if (pState->pShadowMeshVertices == nullptr || pState->pShadowMeshIndices == nullptr || pState->shadowMeshIndexCount == 0) return;
        auto upload = [&](const void* data, size_t bytes, ID3D12Resource** out, const char* name)
        {
            CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
            CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (FAILED(m_pDevice->GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out))))
                return false;
            SetName(*out, name);
            void* mapped = nullptr;
            (*out)->Map(0, nullptr, &mapped);
            memcpy(mapped, data, bytes);
            (*out)->Unmap(0, nullptr);
            return true;
        };
        if (upload(pState->pShadowMeshVertices, (size_t)pState->shadowMeshVertexCount * 16, &m_pShadowMeshVertexBuffer, "SplatRenderer::m_pShadowMeshVertexBuffer") &&
            upload(pState->pShadowMeshIndices, (size_t)pState->shadowMeshIndexCount * 4, &m_pShadowMeshIndexBuffer, "SplatRenderer::m_pShadowMeshIndexBuffer"))
            m_shadowMeshIndexCount = pState->shadowMeshIndexCount;
    }

    // The fallback's pipelines: the same stages as the mesh path as instanced vertex shaders (see
    // Splat.hlsl, mainVS/itemVS/occluderVS), built as classic graphics pipeline states with no input
    // layout (the shaders read the splat buffers by SV_InstanceID). Targets are vs_6_0/ps_6_0; in
    // legacy-compiler mode (SetLegacyShaderCompiler) they become vs_5_1/ps_5_1 automatically.
    void SplatRenderer::CreateVertexShaderPipelines(SwapChain* pSwapChain)
    {
        DefineList defines;
        defines["SPLATS_NO_MESH_SHADERS"] = "1";

        D3D12_SHADER_BYTECODE vs = {}, ps = {}, itemVs = {}, itemPs = {}, occVs = {}, occPs = {};
        bool mainOk = CompileShaderFromFile("Splat.hlsl", &defines, "mainVS", "-T vs_6_0", &vs) && vs.pShaderBytecode != nullptr;
        mainOk = (CompileShaderFromFile("Splat.hlsl", &defines, "mainPS", "-T ps_6_0", &ps) && ps.pShaderBytecode != nullptr) && mainOk;
        bool itemOk = CompileShaderFromFile("Splat.hlsl", &defines, "itemVS", "-T vs_6_0", &itemVs) && itemVs.pShaderBytecode != nullptr;
        itemOk = (CompileShaderFromFile("Splat.hlsl", &defines, "itemPS", "-T ps_6_0", &itemPs) && itemPs.pShaderBytecode != nullptr) && itemOk;
        bool occOk = CompileShaderFromFile("Splat.hlsl", &defines, "occluderVS", "-T vs_6_0", &occVs) && occVs.pShaderBytecode != nullptr;
        occOk = (CompileShaderFromFile("Splat.hlsl", &defines, "occluderPS", "-T ps_6_0", &occPs) && occPs.pShaderBytecode != nullptr) && occOk;
        if (!mainOk || !itemOk)
        {
            LogTransitionTrace("SplatRenderer::CreateVertexShaderPipelines ERROR: fallback shaders failed to compile (main %d, item %d)", mainOk ? 1 : 0, itemOk ? 1 : 0);
            char msg[256];
            snprintf(msg, sizeof(msg), "%s could not compile its vertex-shader fallback for this GPU.\nThe trace log next to the executable holds the compiler output.", g_appTitle);
            MessageBoxA(NULL, msg, g_appTitle, MB_ICONERROR);
            exit(1);
        }
        if (!occOk)
        {
            LogTransitionTrace("SplatRenderer::CreateVertexShaderPipelines ERROR: occluder fallback shaders failed to compile -- occlusion volume feature will be a silent no-op.");
        }

        CD3DX12_RASTERIZER_DESC rasterizer(D3D12_DEFAULT);
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;

        // Premultiplied alpha blending for the splats: Output = SrcColor + DestColor * (1 - SrcAlpha)
        CD3DX12_BLEND_DESC splatBlend(D3D12_DEFAULT);
        splatBlend.RenderTarget[0].BlendEnable = TRUE;
        splatBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        splatBlend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        splatBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        splatBlend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        splatBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        splatBlend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        splatBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        CD3DX12_BLEND_DESC opaqueBlend(D3D12_DEFAULT);

        CD3DX12_DEPTH_STENCIL_DESC depthOff(D3D12_DEFAULT);
        depthOff.DepthEnable = FALSE;
        depthOff.StencilEnable = FALSE;
        CD3DX12_DEPTH_STENCIL_DESC depthWrite(D3D12_DEFAULT);
        depthWrite.DepthEnable = TRUE;
        depthWrite.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthWrite.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        CD3DX12_DEPTH_STENCIL_DESC depthTestOnly(D3D12_DEFAULT);
        depthTestOnly.DepthEnable = TRUE;
        depthTestOnly.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depthTestOnly.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        depthTestOnly.StencilEnable = FALSE;

        auto create = [&](const D3D12_SHADER_BYTECODE& v, const D3D12_SHADER_BYTECODE& px, const D3D12_BLEND_DESC& blend,
                          const D3D12_DEPTH_STENCIL_DESC& depth, DXGI_FORMAT rtv, ID3D12PipelineState** ppOut, const char* name) -> bool
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
            d.pRootSignature = m_pRootSignature;
            d.VS = v;
            d.PS = px;
            d.BlendState = blend;
            d.SampleMask = UINT_MAX;
            d.RasterizerState = rasterizer;
            d.DepthStencilState = depth;
            d.InputLayout = { nullptr, 0 };
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = rtv;
            d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            d.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };
            HRESULT hr = m_pDevice->GetDevice()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(ppOut));
            if (FAILED(hr))
                LogTransitionTrace("SplatRenderer::CreateVertexShaderPipelines ERROR: %s PSO failed hr=0x%08X", name, (unsigned int)hr);
            return SUCCEEDED(hr);
        };

        bool ok = create(vs, ps, splatBlend, depthOff, pSwapChain->GetFormat(), &m_pPipelineState, "main splat");
        ok = create(vs, ps, splatBlend, depthTestOnly, pSwapChain->GetFormat(), &m_pPipelineStateOcclusionTest, "occlusion-test splat") && ok;
        {
            // Detach Camera variants (see the member comments in SplatRenderer.h). Failure here only
            // loses the three-pass sequence; the plain splat pass still runs.
            CD3DX12_DEPTH_STENCIL_DESC dsMain(D3D12_DEFAULT);
            dsMain.DepthEnable = TRUE; dsMain.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; dsMain.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; dsMain.StencilEnable = FALSE;
            CD3DX12_DEPTH_STENCIL_DESC dsEqual(D3D12_DEFAULT);
            dsEqual.DepthEnable = TRUE; dsEqual.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; dsEqual.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL; dsEqual.StencilEnable = FALSE;
            CD3DX12_BLEND_DESC noColour(D3D12_DEFAULT);
            noColour.RenderTarget[0].RenderTargetWriteMask = 0;
            create(vs, ps, splatBlend, dsMain, pSwapChain->GetFormat(), &m_pPipelineStateDetachedMain, "detached main splat");
            create(vs, ps, noColour, depthWrite, pSwapChain->GetFormat(), &m_pPipelineStateCulledDepth, "culled depth splat");
            create(vs, ps, splatBlend, dsEqual, pSwapChain->GetFormat(), &m_pPipelineStateCulledColor, "culled colour splat");
        }
        create(vs, ps, splatBlend, depthOff, m_unormFormat, &m_pSplatPSODisplay, "splat display");
        create(vs, ps, splatBlend, depthOff, m_srgbFormat, &m_pSplatPSOLinear, "splat linear");
        create(vs, ps, splatBlend, depthTestOnly, m_unormFormat, &m_pSplatPSODisplayDepth, "splat display depth");
        create(vs, ps, splatBlend, depthTestOnly, m_srgbFormat, &m_pSplatPSOLinearDepth, "splat linear depth");
        ok = create(itemVs, itemPs, opaqueBlend, depthWrite, DXGI_FORMAT_R32_UINT, &m_pItemPrepassPSO, "item prepass") && ok;
        if (occOk)
        {
            create(occVs, occPs, opaqueBlend, depthWrite, pSwapChain->GetFormat(), &m_pOccluderPSO, "occluder");
            create(occVs, occPs, opaqueBlend, depthWrite, m_unormFormat, &m_pOccluderPSOUnorm, "occluder UNORM");
        }
        if (!ok)
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s could not create its vertex-shader fallback pipelines on this GPU.\nThe trace log next to the executable holds the details.", g_appTitle);
            MessageBoxA(NULL, msg, g_appTitle, MB_ICONERROR);
            exit(1);
        }
        LogTransitionTrace("SplatRenderer: vertex-shader render path ready: %s", GetRenderPathDescription());
    }

    const char* SplatRenderer::GetRenderPathDescription() const
    {
        switch (m_renderPath)
        {
        case RenderPath::VertexShadersSM6: return "Vertex shaders, Shader Model 6.0 (no mesh shader support)";
        case RenderPath::VertexShadersSM5: return "Vertex shaders, Shader Model 5.1 via the legacy compiler (no DXIL support)";
        default:                           return "Mesh shaders, Shader Model 6.5";
        }
    }

    void SplatRenderer::OnDestroy()
    {
        FlushCopyQueue();
        if (m_copyFenceEvent) { CloseHandle(m_copyFenceEvent); m_copyFenceEvent = nullptr; }
        if (m_pCopyFence) { m_pCopyFence->Release(); m_pCopyFence = nullptr; }
        if (m_pGraphicsProgressFence) { m_pGraphicsProgressFence->Release(); m_pGraphicsProgressFence = nullptr; }
        if (m_pCopyCmdList) { m_pCopyCmdList->Release(); m_pCopyCmdList = nullptr; }
        if (m_pCopyAllocator) { m_pCopyAllocator->Release(); m_pCopyAllocator = nullptr; }
        if (m_pCopyQueue) { m_pCopyQueue->Release(); m_pCopyQueue = nullptr; }
        if (m_pPointBuffer) { m_pPointBuffer->Unmap(0, nullptr); m_pPointBuffer->Release(); m_pPointBuffer = nullptr; }
        if (m_pPointGpuBuffer) { m_pPointGpuBuffer->Release(); m_pPointGpuBuffer = nullptr; }
        if (m_pSplatGpuOutBuffer) { m_pSplatGpuOutBuffer->Release(); m_pSplatGpuOutBuffer = nullptr; }
        m_pPointBufferMapped = nullptr;
        m_pointBufferCapacityBytes = 0;
        m_splatBufferGPUAddress = 0;

        if (m_pRawSplatBuffer) { m_pRawSplatBuffer->Unmap(0, nullptr); m_pRawSplatBuffer->Release(); m_pRawSplatBuffer = nullptr; }
        if (m_pRawSplatGpuBuffer) { m_pRawSplatGpuBuffer->Release(); m_pRawSplatGpuBuffer = nullptr; }
        if (m_pRawSplatGpuOutBuffer) { m_pRawSplatGpuOutBuffer->Release(); m_pRawSplatGpuOutBuffer = nullptr; }
        m_pRawSplatBufferMapped = nullptr;
        m_rawSplatBufferCapacityBytes = 0;
        m_rawSplatBufferGPUAddress = 0;

        if (m_pGPUSortPairBuffer) { m_pGPUSortPairBuffer->Release(); m_pGPUSortPairBuffer = nullptr; }
        m_sortPairBufferCapacityBytes = 0;

        if (m_pSplatBuffer) { m_pSplatBuffer->Unmap(0, nullptr); m_pSplatBuffer->Release(); m_pSplatBuffer = nullptr; }
        if (m_pSplatGpuBuffer) { m_pSplatGpuBuffer->Release(); m_pSplatGpuBuffer = nullptr; }
        m_pSplatBufferMapped = nullptr; m_splatBufferCapacityBytes = 0;
        if (m_pSHBuffer) { m_pSHBuffer->Unmap(0, nullptr); m_pSHBuffer->Release(); m_pSHBuffer = nullptr; }
        if (m_pSHGpuBuffer) { m_pSHGpuBuffer->Release(); m_pSHGpuBuffer = nullptr; }
        m_pSHBufferMapped = nullptr; m_shBufferCapacityBytes = 0;
        for (uint32_t r = 0; r < kUploadRing; r++)
        {
            if (m_pChunkSlotBaseUploadRing[r]) { m_pChunkSlotBaseUploadRing[r]->Unmap(0, nullptr); m_pChunkSlotBaseUploadRing[r]->Release(); m_pChunkSlotBaseUploadRing[r] = nullptr; }
            m_pChunkSlotBaseMappedRing[r] = nullptr;
            if (m_pSortedChunkIndicesUploadBufferRing[r]) { m_pSortedChunkIndicesUploadBufferRing[r]->Unmap(0, nullptr); m_pSortedChunkIndicesUploadBufferRing[r]->Release(); m_pSortedChunkIndicesUploadBufferRing[r] = nullptr; }
            m_pSortedChunkIndicesUploadBufferMappedRing[r] = nullptr;
        }
        m_pChunkSlotBaseUpload = nullptr; m_pChunkSlotBaseMapped = nullptr;
        m_pSortedChunkIndicesUploadBuffer = nullptr; m_pSortedChunkIndicesUploadBufferMapped = nullptr;
        if (m_pChunkSlotBaseGpu) { m_pChunkSlotBaseGpu->Release(); m_pChunkSlotBaseGpu = nullptr; }
        if (m_pSlotInfoBuffer) { m_pSlotInfoBuffer->Release(); m_pSlotInfoBuffer = nullptr; }
        if (m_pSplatPairsA) { m_pSplatPairsA->Release(); m_pSplatPairsA = nullptr; }
        if (m_pSplatPairsB) { m_pSplatPairsB->Release(); m_pSplatPairsB = nullptr; }
        if (m_pSplatHistBuffer) { m_pSplatHistBuffer->Release(); m_pSplatHistBuffer = nullptr; }
        m_splatSortCapacitySlots = 0;
        if (m_pSplatExpandPSO) { m_pSplatExpandPSO->Release(); m_pSplatExpandPSO = nullptr; }
        if (m_pSplatHistPSO) { m_pSplatHistPSO->Release(); m_pSplatHistPSO = nullptr; }
        if (m_pSplatScanPSO) { m_pSplatScanPSO->Release(); m_pSplatScanPSO = nullptr; }
        if (m_pSplatScatterPSO) { m_pSplatScatterPSO->Release(); m_pSplatScatterPSO = nullptr; }
        if (m_pSplatSortRootSig) { m_pSplatSortRootSig->Release(); m_pSplatSortRootSig = nullptr; }
        if (m_pSplatPSODisplay) { m_pSplatPSODisplay->Release(); m_pSplatPSODisplay = nullptr; }
        if (m_pSplatPSOLinear) { m_pSplatPSOLinear->Release(); m_pSplatPSOLinear = nullptr; }
        if (m_pSplatPSODisplayDepth) { m_pSplatPSODisplayDepth->Release(); m_pSplatPSODisplayDepth = nullptr; }
        if (m_pSplatPSOLinearDepth) { m_pSplatPSOLinearDepth->Release(); m_pSplatPSOLinearDepth = nullptr; }
        if (m_pOccluderPSOUnorm) { m_pOccluderPSOUnorm->Release(); m_pOccluderPSOUnorm = nullptr; }
        if (m_pBackBufferUnormRtvHeap) { m_pBackBufferUnormRtvHeap->Release(); m_pBackBufferUnormRtvHeap = nullptr; }

        for (uint32_t r = 0; r < kUploadRing; r++)
        {
            if (m_pChunkUploadBufferRing[r]) { m_pChunkUploadBufferRing[r]->Unmap(0, nullptr); m_pChunkUploadBufferRing[r]->Release(); m_pChunkUploadBufferRing[r] = nullptr; }
            m_pChunkUploadBufferMappedRing[r] = nullptr;
        }
        m_pChunkUploadBuffer = nullptr;
        m_pChunkUploadBufferMapped = nullptr;
        if (m_pChunkGpuBuffer) { m_pChunkGpuBuffer->Release(); m_pChunkGpuBuffer = nullptr; }
        m_chunkBufferCapacityBytes = 0;
        if (m_pSortedChunkIndicesGpuBuffer) { m_pSortedChunkIndicesGpuBuffer->Release(); m_pSortedChunkIndicesGpuBuffer = nullptr; }
        m_sortedChunkIndicesCapacityBytes = 0;
        m_lastUploadedChunks.clear();

        m_lastSplatsPtr = nullptr;
        m_lastSplatCount = 0;
        m_lastRenderMode = 0;

        if (m_pCommandSignature) { m_pCommandSignature->Release(); m_pCommandSignature = nullptr; }
        if (m_pProjectKeysPSO) { m_pProjectKeysPSO->Release(); m_pProjectKeysPSO = nullptr; }
        if (m_pProjectChunkKeysPSO) { m_pProjectChunkKeysPSO->Release(); m_pProjectChunkKeysPSO = nullptr; }
        if (m_pBitonicLocalSortPSO) { m_pBitonicLocalSortPSO->Release(); m_pBitonicLocalSortPSO = nullptr; }
        if (m_pBitonicGlobalSortPSO) { m_pBitonicGlobalSortPSO->Release(); m_pBitonicGlobalSortPSO = nullptr; }
        if (m_pBitonicLocalMergePSO) { m_pBitonicLocalMergePSO->Release(); m_pBitonicLocalMergePSO = nullptr; }
        if (m_pGatherSplatsPSO) { m_pGatherSplatsPSO->Release(); m_pGatherSplatsPSO = nullptr; }
        if (m_pGatherChunkIndicesPSO) { m_pGatherChunkIndicesPSO->Release(); m_pGatherChunkIndicesPSO = nullptr; }
        if (m_pRadixSortPSO) { m_pRadixSortPSO->Release(); m_pRadixSortPSO = nullptr; }
        if (m_pCullPSO) { m_pCullPSO->Release(); m_pCullPSO = nullptr; }
        if (m_pComputeRootSignature) { m_pComputeRootSignature->Release(); m_pComputeRootSignature = nullptr; }
        if (m_pPipelineState) { m_pPipelineState->Release(); m_pPipelineState = nullptr; }
        if (m_pPipelineStateOcclusionTest) { m_pPipelineStateOcclusionTest->Release(); m_pPipelineStateOcclusionTest = nullptr; }
        if (m_pPipelineStateDetachedMain) { m_pPipelineStateDetachedMain->Release(); m_pPipelineStateDetachedMain = nullptr; }
        if (m_pPipelineStateCulledDepth) { m_pPipelineStateCulledDepth->Release(); m_pPipelineStateCulledDepth = nullptr; }
        if (m_pPipelineStateCulledColor) { m_pPipelineStateCulledColor->Release(); m_pPipelineStateCulledColor = nullptr; }
        if (m_pOccluderPSO) { m_pOccluderPSO->Release(); m_pOccluderPSO = nullptr; }
        if (m_pItemPrepassPSO) { m_pItemPrepassPSO->Release(); m_pItemPrepassPSO = nullptr; }
        if (m_pOcclusionVoxelBuffer) { m_pOcclusionVoxelBuffer->Unmap(0, nullptr); m_pOcclusionVoxelBuffer->Release(); m_pOcclusionVoxelBuffer = nullptr; }
        m_pOcclusionVoxelBufferMapped = nullptr;
        m_occlusionVoxelBufferCapacityBytes = 0;
        if (m_pShadowMeshPSO) { m_pShadowMeshPSO->Release(); m_pShadowMeshPSO = nullptr; }
        if (m_pShadowMeshPSOUnorm) { m_pShadowMeshPSOUnorm->Release(); m_pShadowMeshPSOUnorm = nullptr; }
        if (m_pShadowMeshFillPSO) { m_pShadowMeshFillPSO->Release(); m_pShadowMeshFillPSO = nullptr; }
        if (m_pShadowMeshFillPSOUnorm) { m_pShadowMeshFillPSOUnorm->Release(); m_pShadowMeshFillPSOUnorm = nullptr; }
        if (m_pShadowMeshVertexBuffer) { m_pShadowMeshVertexBuffer->Release(); m_pShadowMeshVertexBuffer = nullptr; }
        if (m_pShadowMeshIndexBuffer) { m_pShadowMeshIndexBuffer->Release(); m_pShadowMeshIndexBuffer = nullptr; }
        m_shadowMeshUploadedVersion = UINT32_MAX;
        m_shadowMeshIndexCount = 0;
        ReleaseMesh(m_occluderMesh);
        m_lastOcclusionVoxelsPtr = nullptr;
        m_lastOcclusionVoxelCount = 0;
        m_clearBitmaskCS.OnDestroy();
        m_silhouetteEdgeExtractCS.OnDestroy();
        if (m_pSilhouetteBitmaskGpuBuffer) { m_pSilhouetteBitmaskGpuBuffer->Release(); m_pSilhouetteBitmaskGpuBuffer = nullptr; }
        if (m_pSilhouetteReadbackBuffer) { m_pSilhouetteReadbackBuffer->Release(); m_pSilhouetteReadbackBuffer = nullptr; }
        m_bitmaskCapacityBytes = 0;
        if (m_pCaptureReadback) { m_pCaptureReadback->Release(); m_pCaptureReadback = nullptr; }
        m_captureReadbackBytes = 0;
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

    bool SplatRenderer::TakeSceneCapture(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
    {
        if (!m_captureReady) return false;
        m_captureReady = false;
        rgba.swap(m_capturePixels);
        width = m_captureWidth;
        height = m_captureHeight;
        return true;
    }

    // Splat mode: grow-only buffers of the per-frame depth sort, sized for the padded slot count.
    void SplatRenderer::EnsureSplatSortBuffers(uint32_t paddedSlots)
    {
        if (paddedSlots <= m_splatSortCapacitySlots) return;
        // Grow by doubling: the slot count climbs every frame while a model refines (parents and children
        // both in the list), and growing to the exact count reallocated, with a full GPU flush, every time
        // it crossed the next 1024 slots -- most frames of a refinement.
        paddedSlots = std::max(paddedSlots, m_splatSortCapacitySlots * 2u);
        m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
        if (m_pSlotInfoBuffer) { m_pSlotInfoBuffer->Release(); m_pSlotInfoBuffer = nullptr; }
        if (m_pSplatPairsA) { m_pSplatPairsA->Release(); m_pSplatPairsA = nullptr; }
        if (m_pSplatPairsB) { m_pSplatPairsB->Release(); m_pSplatPairsB = nullptr; }
        if (m_pSplatHistBuffer) { m_pSplatHistBuffer->Release(); m_pSplatHistBuffer = nullptr; }
        auto makeUav = [&](uint64_t bytes, ID3D12Resource** ppOut, const char* name)
        {
            CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(ppOut)));
            SetName(*ppOut, name);
        };
        const uint64_t pairBytes = (uint64_t)paddedSlots * 8u;
        makeUav(pairBytes, &m_pSlotInfoBuffer, "SplatRenderer::m_pSlotInfoBuffer");
        makeUav(pairBytes, &m_pSplatPairsA, "SplatRenderer::m_pSplatPairsA");
        makeUav(pairBytes, &m_pSplatPairsB, "SplatRenderer::m_pSplatPairsB");
        makeUav((uint64_t)256u * (paddedSlots / 1024u) * 4u, &m_pSplatHistBuffer, "SplatRenderer::m_pSplatHistBuffer");
        m_slotInfoState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_splatPairsAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_splatSortCapacitySlots = paddedSlots;
        m_splatSortNeedsRun = true;
    }

    // Splat mode: expands the render list into slots and sorts them far-to-near (SplatSortCS.hlsl).
    // Leaves m_pSplatPairsA (the order) and m_pSlotInfoBuffer readable by the draw.
    void SplatRenderer::RunSplatSort(ID3D12GraphicsCommandList* pCmdLst, const State* pState, XMFLOAT3 eyePos, XMFLOAT3 forward, const XMFLOAT4X4& cullViewProj)
    {
        static uint32_t s_sortTraceFrame = 0;
        const bool traceThisFrame = ((s_sortTraceFrame++ % 3600) == 0); // A line a minute at most: only the "cannot run" cases are worth logging
        if (!m_pSplatSortRootSig || !m_pSplatExpandPSO || !m_pSplatHistPSO || !m_pSplatScanPSO || !m_pSplatScatterPSO)
        {
            if (traceThisFrame) LogTransitionTrace("RunSplatSort: pipeline missing (rootsig %d expand %d hist %d scan %d scatter %d)", m_pSplatSortRootSig != nullptr, m_pSplatExpandPSO != nullptr, m_pSplatHistPSO != nullptr, m_pSplatScanPSO != nullptr, m_pSplatScatterPSO != nullptr);
            return;
        }
        if (!m_pSplatGpuBuffer || !m_pChunkGpuBuffer || !m_pChunkSlotBaseGpu || pState->chunkCount == 0 || m_splatSlotCount == 0)
        {
            if (traceThisFrame) LogTransitionTrace("RunSplatSort: inputs missing (splats %d chunks %d slotBase %d chunkCount %u slots %u)", m_pSplatGpuBuffer != nullptr, m_pChunkGpuBuffer != nullptr, m_pChunkSlotBaseGpu != nullptr, pState->chunkCount, m_splatSlotCount);
            return;
        }

        const uint32_t slotCount = m_splatSlotCount;
        const uint32_t padded = (slotCount + 1023u) & ~1023u;
        const uint32_t numBlocks = padded / 1024u;
        EnsureSplatSortBuffers(padded);

        auto toState = [&](ID3D12Resource* pRes, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES want)
        {
            if (state == want) return;
            D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(pRes, state, want);
            pCmdLst->ResourceBarrier(1, &b);
            state = want;
        };

        if (!m_splatSortNeedsRun)
        {
            toState(m_pSplatPairsA, m_splatPairsAState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            toState(m_pSlotInfoBuffer, m_slotInfoState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            return;
        }

        auto sortStart = std::chrono::high_resolution_clock::now();
        toState(m_pSplatPairsA, m_splatPairsAState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        toState(m_pSlotInfoBuffer, m_slotInfoState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (m_chunkSlotBaseState == D3D12_RESOURCE_STATE_COPY_DEST)
        {
            D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(m_pChunkSlotBaseGpu, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            pCmdLst->ResourceBarrier(1, &b);
            m_chunkSlotBaseState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        }

        struct SplatSortCB
        {
            XMFLOAT4X4 cullViewProj;
            XMFLOAT3   camPos;      uint32_t totalChunks;
            XMFLOAT3   camForward;  uint32_t slotCount;
            XMFLOAT3   aabbMin;     uint32_t pass;
            XMFLOAT3   aabbExtents; uint32_t numBlocks;
            uint32_t   paddedSlotCount; uint32_t pad0, pad1, pad2;
        };
        auto bindCB = [&](uint32_t pass)
        {
            SplatSortCB* pCB = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
            if (!m_constantBufferRing.AllocConstantBuffer(sizeof(SplatSortCB), (void**)&pCB, &addr)) return false;
            pCB->cullViewProj = cullViewProj;
            pCB->camPos = eyePos;           pCB->totalChunks = pState->chunkCount;
            pCB->camForward = forward;      pCB->slotCount = slotCount;
            pCB->aabbMin = pState->aabbMin; pCB->pass = pass;
            pCB->aabbExtents = pState->aabbExtents; pCB->numBlocks = numBlocks;
            pCB->paddedSlotCount = padded;  pCB->pad0 = pCB->pad1 = pCB->pad2 = 0;
            pCmdLst->SetComputeRootConstantBufferView(0, addr);
            return true;
        };

        pCmdLst->SetComputeRootSignature(m_pSplatSortRootSig);
        pCmdLst->SetComputeRootShaderResourceView(1, m_pSplatGpuBuffer->GetGPUVirtualAddress());
        pCmdLst->SetComputeRootShaderResourceView(2, m_pChunkGpuBuffer->GetGPUVirtualAddress());
        pCmdLst->SetComputeRootShaderResourceView(3, m_pChunkSlotBaseGpu->GetGPUVirtualAddress());
        pCmdLst->SetComputeRootUnorderedAccessView(4, m_pSlotInfoBuffer->GetGPUVirtualAddress());
        pCmdLst->SetComputeRootUnorderedAccessView(7, m_pSplatHistBuffer->GetGPUVirtualAddress());

        D3D12_RESOURCE_BARRIER uavA = CD3DX12_RESOURCE_BARRIER::UAV(m_pSplatPairsA);
        D3D12_RESOURCE_BARRIER uavB = CD3DX12_RESOURCE_BARRIER::UAV(m_pSplatPairsB);
        D3D12_RESOURCE_BARRIER uavHist = CD3DX12_RESOURCE_BARRIER::UAV(m_pSplatHistBuffer);
        D3D12_RESOURCE_BARRIER uavInfo = CD3DX12_RESOURCE_BARRIER::UAV(m_pSlotInfoBuffer);

        // 1. Expand the chunks into slots with their keys
        pCmdLst->SetComputeRootUnorderedAccessView(5, m_pSplatPairsA->GetGPUVirtualAddress());
        pCmdLst->SetComputeRootUnorderedAccessView(6, m_pSplatPairsB->GetGPUVirtualAddress());
        if (!bindCB(0)) return;
        pCmdLst->SetPipelineState(m_pSplatExpandPSO);
        pCmdLst->Dispatch(pState->chunkCount, 1, 1);
        D3D12_RESOURCE_BARRIER afterExpand[2] = { uavA, uavInfo };
        pCmdLst->ResourceBarrier(2, afterExpand);

        // 2. Four radix passes, ping-ponging A -> B -> A -> B -> A
        for (uint32_t pass = 0; pass < 4; pass++)
        {
            ID3D12Resource* pIn = (pass & 1u) ? m_pSplatPairsB : m_pSplatPairsA;
            ID3D12Resource* pOut = (pass & 1u) ? m_pSplatPairsA : m_pSplatPairsB;
            pCmdLst->SetComputeRootUnorderedAccessView(5, pIn->GetGPUVirtualAddress());
            pCmdLst->SetComputeRootUnorderedAccessView(6, pOut->GetGPUVirtualAddress());
            if (!bindCB(pass)) return;
            pCmdLst->SetPipelineState(m_pSplatHistPSO);
            pCmdLst->Dispatch(numBlocks, 1, 1);
            pCmdLst->ResourceBarrier(1, &uavHist);
            pCmdLst->SetPipelineState(m_pSplatScanPSO);
            pCmdLst->Dispatch(1, 1, 1);
            pCmdLst->ResourceBarrier(1, &uavHist);
            pCmdLst->SetPipelineState(m_pSplatScatterPSO);
            pCmdLst->Dispatch(numBlocks, 1, 1);
            D3D12_RESOURCE_BARRIER afterScatter = (pass & 1u) ? uavA : uavB;
            pCmdLst->ResourceBarrier(1, &afterScatter);
        }

        toState(m_pSplatPairsA, m_splatPairsAState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        toState(m_pSlotInfoBuffer, m_slotInfoState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        m_splatSortNeedsRun = false;
        m_heartbeatSorts++;
        m_metrics.gpuSortTimeMs = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - sortStart).count();
        m_metrics.isGPUSortActive = true;
        m_metrics.wasSortedThisFrame = true;
    }

    // Keeps the GPU splat/chunk/sorted-index buffers current with pState, growing and re-uploading
    // them only when the source data pointer, count, or camera-relative sort key actually changed.
    void SplatRenderer::UpdateSplatBuffers(
        const State* pState,
        XMFLOAT3 eyePos,
        XMFLOAT3 forward)
    {
        if (pState == nullptr) return;

        const PackedPointGPU* pPoints = pState->pPoints;
        const SplatPoint* pRawSplats = pState->pRawSplats;
        uint32_t splatCount = pState->splatCount;
        uint32_t renderMode = pState->renderMode;

        const void* activePtr = (renderMode == 2) ? (const void*)pRawSplats : SplatRecords(renderMode) ? (const void*)pState->pSplats : (const void*)pPoints;

        if (activePtr == nullptr || splatCount == 0)
        {
            m_splatBufferGPUAddress = 0;
            m_rawSplatBufferGPUAddress = 0;
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

        bool modelChanged = (m_lastSplatsPtr != activePtr) || (m_lastSplatCount != splatCount) || (m_lastRenderMode != renderMode) || pipelineModeChanged;
        float camMoved = std::abs(eyePos.x - m_lastSortEye.x) + std::abs(eyePos.y - m_lastSortEye.y) + std::abs(eyePos.z - m_lastSortEye.z);
        float forwardMoved = std::abs(forward.x - m_lastSortForward.x) + std::abs(forward.y - m_lastSortForward.y) + std::abs(forward.z - m_lastSortForward.z);
        bool needsSort = modelChanged || (camMoved > 1e-4f) || (forwardMoved > 1e-4f);

        if (m_lastRenderMode != renderMode && SplatRecords(renderMode))
            LogTransitionTrace(renderMode == 4 ? "SplatRenderer: splat package drawn as splats (opaque discs, chunk sort)"
                                               : "SplatRenderer: splat package drawn as sorted Gaussians");
        m_lastSplatsPtr = activePtr;
        m_lastSplatCount = splatCount;
        m_lastRenderMode = renderMode;

        if (renderMode == 2)
        {
            // Find next power of 2 for GPU Bitonic network
            uint32_t numElements = 1;
            while (numElements < splatCount) numElements <<= 1;
            uint32_t requiredBytes = numElements * sizeof(SplatPoint);

            if (requiredBytes > m_rawSplatBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                FlushCopyQueue(); // m_pCopyQueue may still be reading m_pRawSplatBuffer via CopyResource; Device::GPUFlush doesn't know about this queue

                if (m_pRawSplatBuffer) { m_pRawSplatBuffer->Unmap(0, nullptr); m_pRawSplatBuffer->Release(); m_pRawSplatBuffer = nullptr; }
                if (m_pRawSplatGpuBuffer) { m_pRawSplatGpuBuffer->Release(); m_pRawSplatGpuBuffer = nullptr; }
                if (m_pRawSplatGpuOutBuffer) { m_pRawSplatGpuOutBuffer->Release(); m_pRawSplatGpuOutBuffer = nullptr; }
                m_pRawSplatBufferMapped = nullptr;

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                    D3D12_HEAP_FLAG_NONE,
                    &CD3DX12_RESOURCE_DESC::Buffer(requiredBytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&m_pRawSplatBuffer)));
                SetName(m_pRawSplatBuffer, "SplatRenderer::m_pRawSplatBuffer");

                CD3DX12_RESOURCE_DESC gpuBufDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredBytes);
                gpuBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_pRawSplatGpuBuffer)));
                SetName(m_pRawSplatGpuBuffer, "SplatRenderer::m_pRawSplatGpuBuffer");

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                    nullptr,
                    IID_PPV_ARGS(&m_pRawSplatGpuOutBuffer)));
                SetName(m_pRawSplatGpuOutBuffer, "SplatRenderer::m_pRawSplatGpuOutBuffer");

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
                    SetName(m_pGPUSortPairBuffer, "SplatRenderer::m_pGPUSortPairBuffer");
                    m_sortPairBufferCapacityBytes = pairBytes;
                }

                m_pRawSplatBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pRawSplatBufferMapped));
                m_rawSplatBufferCapacityBytes = requiredBytes;
                modelChanged = true;
                needsSort = true;
            }

            if (splatCount > 0 && pRawSplats != nullptr && m_pRawSplatBufferMapped != nullptr)
            {
                SplatPoint* pDst = reinterpret_cast<SplatPoint*>(m_pRawSplatBufferMapped);

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
                            memcpy(pDst, pRawSplats, splatCount * sizeof(SplatPoint));
                            if (numElements > splatCount)
                            {
                                memset(pDst + splatCount, 0, (numElements - splatCount) * sizeof(SplatPoint));
                            }
                            m_needUploadToGpu = true;
                        }
                    }
                    if (modelChanged)
                    {
                        memcpy(pDst, pRawSplats, splatCount * sizeof(SplatPoint));
                        if (numElements > splatCount)
                        {
                            memset(pDst + splatCount, 0, (numElements - splatCount) * sizeof(SplatPoint));
                        }
                        m_needUploadToGpu = true;
                    }
                    m_metrics.isGPUSortActive = false;
                    m_metrics.cpuSortTimeMs = 0.0f;
                    m_metrics.gpuSortTimeMs = 0.0f;
                }
                else
                {
                    // Flat pipeline mode: global sort of all splats
                    if (pState->gpuRadixSort)
                    {
                        m_metrics.isGPUSortActive = true;
                        m_metrics.cpuSortTimeMs = 0.0f;
                        m_gpuSortNeedsRun = true;

                        if (modelChanged)
                        {
                            memcpy(pDst, pRawSplats, splatCount * sizeof(SplatPoint));
                            if (numElements > splatCount)
                            {
                                memset(pDst + splatCount, 0, (numElements - splatCount) * sizeof(SplatPoint));
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

                        if (m_sortIndicesA.size() < splatCount)
                        {
                            m_sortIndicesA.resize(splatCount);
                            m_sortIndicesB.resize(splatCount);
                            m_sortKeys.resize(splatCount);
                            m_sortDists.resize(splatCount);
                        }

                        // 1. Parallel Projected Distance Calculation
                        float minD = 1e9f, maxD = -1e9f;
                        #pragma omp parallel for reduction(min:minD) reduction(max:maxD)
                        for (int i = 0; i < (int)splatCount; i++)
                        {
                            const auto& pos = pRawSplats[i].position;
                            float d = (pos.x - eyePos.x) * forward.x + (pos.y - eyePos.y) * forward.y + (pos.z - eyePos.z) * forward.z;
                            m_sortDists[i] = d;
                            if (d < minD) minD = d;
                            if (d > maxD) maxD = d;
                        }

                        float range = std::max(0.001f, maxD - minD);
                        float scale = 65535.0f / range;

                        // 2. Parallel 16-bit key computation
                        #pragma omp parallel for
                        for (int i = 0; i < (int)splatCount; i++)
                        {
                            uint32_t k = (uint32_t)((m_sortDists[i] - minD) * scale);
                            if (k > 65535) k = 65535;
                            m_sortKeys[i] = 65535 - k;
                            m_sortIndicesA[i] = i;
                        }

                        // 3. Pass 1: Radix sort low 8 bits (Byte 0)
                        uint32_t count0[256] = {};
                        for (uint32_t i = 0; i < splatCount; i++)
                            count0[m_sortKeys[i] & 0xFF]++;

                        uint32_t offset0[256] = {};
                        for (uint32_t i = 1; i < 256; i++)
                            offset0[i] = offset0[i - 1] + count0[i - 1];

                        for (uint32_t i = 0; i < splatCount; i++)
                        {
                            uint32_t idx = m_sortIndicesA[i];
                            uint8_t byte0 = (uint8_t)(m_sortKeys[idx] & 0xFF);
                            m_sortIndicesB[offset0[byte0]++] = idx;
                        }

                        // 4. Pass 2: Radix sort high 8 bits (Byte 1)
                        uint32_t count1[256] = {};
                        for (uint32_t i = 0; i < splatCount; i++)
                            count1[(m_sortKeys[m_sortIndicesB[i]] >> 8) & 0xFF]++;

                        uint32_t offset1[256] = {};
                        for (uint32_t i = 1; i < 256; i++)
                            offset1[i] = offset1[i - 1] + count1[i - 1];

                        for (uint32_t i = 0; i < splatCount; i++)
                        {
                            uint32_t idx = m_sortIndicesB[i];
                            uint8_t byte1 = (uint8_t)((m_sortKeys[idx] >> 8) & 0xFF);
                            m_sortIndicesA[offset1[byte1]++] = idx;
                        }

                        // 5. Parallel Gather sorted splats into mapped GPU upload buffer
                        #pragma omp parallel for
                        for (int i = 0; i < (int)splatCount; i++)
                        {
                            pDst[i] = pRawSplats[m_sortIndicesA[i]];
                        }
                        for (uint32_t i = splatCount; i < numElements; i++)
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

            m_rawSplatBufferGPUAddress = m_pRawSplatBuffer->GetGPUVirtualAddress();
        }
        else if (SplatRecords(renderMode))
        {
            // Modes 3 and 4: splat records. The records and the SH stream are uploaded as they are; the
            // per-frame order comes from the GPU sort (RunSplatSort) in mode 3 and from the chunk sort in
            // mode 4 (set up below with the chunk buffers), so nothing here depends on the camera.
            const uint32_t splatBytes = std::max<uint32_t>(64u, splatCount) * (uint32_t)sizeof(PackedSplatGPU);
            const uint32_t shBytesWanted = std::max<uint32_t>(16u, splatCount * std::max<uint32_t>(1u, pState->shRecordBytes));
            auto growPair = [&](uint32_t bytes, ID3D12Resource*& pUpload, ID3D12Resource*& pGpu, uint8_t*& pMapped, uint32_t& capacity, const char* name) -> bool
            {
                if (bytes <= capacity) return false;
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                FlushCopyQueue();
                if (pUpload) { pUpload->Unmap(0, nullptr); pUpload->Release(); pUpload = nullptr; }
                if (pGpu) { pGpu->Release(); pGpu = nullptr; }
                pMapped = nullptr;
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(bytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pUpload)));
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(bytes),
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pGpu)));
                SetName(pGpu, name);
                pUpload->Map(0, nullptr, reinterpret_cast<void**>(&pMapped));
                capacity = bytes;
                return true;
            };
            bool grew = growPair(splatBytes, m_pSplatBuffer, m_pSplatGpuBuffer, m_pSplatBufferMapped, m_splatBufferCapacityBytes, "SplatRenderer::m_pSplatGpuBuffer");
            grew = growPair(shBytesWanted, m_pSHBuffer, m_pSHGpuBuffer, m_pSHBufferMapped, m_shBufferCapacityBytes, "SplatRenderer::m_pSHGpuBuffer") || grew;
            if (grew) { modelChanged = true; needsSort = true; }
            const bool shChanged = (m_lastSplatSHPtr != (const void*)pState->pSplatSH);
            m_lastSplatSHPtr = pState->pSplatSH;

            if (splatCount > 0 && pState->pSplats != nullptr && m_pSplatBufferMapped != nullptr && (modelChanged || shChanged || m_needUploadToGpu))
            {
                memcpy(m_pSplatBufferMapped, pState->pSplats, (size_t)splatCount * sizeof(PackedSplatGPU));
                if (m_pSHBufferMapped != nullptr)
                {
                    const size_t shBytes = (size_t)splatCount * pState->shRecordBytes;
                    if (pState->pSplatSH != nullptr && shBytes > 0 && shBytes <= m_shBufferCapacityBytes)
                        memcpy(m_pSHBufferMapped, pState->pSplatSH, shBytes);
                    else
                        memset(m_pSHBufferMapped, 0, std::min<size_t>(shBytesWanted, m_shBufferCapacityBytes));
                }
                m_needUploadToGpu = true;
            }
            m_splatBufferGPUAddress = m_pSplatGpuBuffer ? m_pSplatGpuBuffer->GetGPUVirtualAddress() : 0;
            if (SortedGaussians(renderMode) && (needsSort || modelChanged)) m_splatSortNeedsRun = true;
        }
        else
        {
            // Mode 1: Quantized 8-byte Splat / PackedPointGPU
            uint32_t numElements = 1;
            while (numElements < splatCount) numElements <<= 1;
            uint32_t requiredBytes = numElements * sizeof(PackedPointGPU);

            if (requiredBytes > m_pointBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                FlushCopyQueue(); // m_pCopyQueue may still be reading m_pPointBuffer via CopyResource; Device::GPUFlush doesn't know about this queue

                if (m_pPointBuffer) { m_pPointBuffer->Unmap(0, nullptr); m_pPointBuffer->Release(); m_pPointBuffer = nullptr; }
                if (m_pPointGpuBuffer) { m_pPointGpuBuffer->Release(); m_pPointGpuBuffer = nullptr; }
                if (m_pSplatGpuOutBuffer) { m_pSplatGpuOutBuffer->Release(); m_pSplatGpuOutBuffer = nullptr; }
                m_pPointBufferMapped = nullptr;

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                    D3D12_HEAP_FLAG_NONE,
                    &CD3DX12_RESOURCE_DESC::Buffer(requiredBytes),
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&m_pPointBuffer)));
                SetName(m_pPointBuffer, "SplatRenderer::m_pPointBuffer");

                CD3DX12_RESOURCE_DESC gpuBufDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredBytes);
                gpuBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_pPointGpuBuffer)));
                SetName(m_pPointGpuBuffer, "SplatRenderer::m_pPointGpuBuffer");

                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &gpuBufDesc,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                    nullptr,
                    IID_PPV_ARGS(&m_pSplatGpuOutBuffer)));
                SetName(m_pSplatGpuOutBuffer, "SplatRenderer::m_pSplatGpuOutBuffer");

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
                    SetName(m_pGPUSortPairBuffer, "SplatRenderer::m_pGPUSortPairBuffer");
                    m_sortPairBufferCapacityBytes = pairBytes;
                }

                m_pPointBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pPointBufferMapped));
                m_pointBufferCapacityBytes = requiredBytes;
                modelChanged = true;
                needsSort = true;
            }

            if (splatCount > 0 && pPoints != nullptr && m_pPointBufferMapped != nullptr)
            {
                PackedPointGPU* pDst = reinterpret_cast<PackedPointGPU*>(m_pPointBufferMapped);

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
                            memcpy(pDst, pPoints, splatCount * sizeof(PackedPointGPU));
                            if (numElements > splatCount)
                            {
                                memset(pDst + splatCount, 0xFF, (numElements - splatCount) * sizeof(PackedPointGPU));
                            }
                            m_needUploadToGpu = true;
                        }
                    }
                    if (modelChanged)
                    {
                        memcpy(pDst, pPoints, splatCount * sizeof(PackedPointGPU));
                        if (numElements > splatCount)
                        {
                            memset(pDst + splatCount, 0xFF, (numElements - splatCount) * sizeof(PackedPointGPU));
                        }
                        m_needUploadToGpu = true;
                    }
                    m_metrics.isGPUSortActive = false;
                    m_metrics.cpuSortTimeMs = 0.0f;
                    m_metrics.gpuSortTimeMs = 0.0f;
                }
                else
                {
                    // Flat pipeline mode: global sort of all splats
                    if (pState->gpuRadixSort)
                    {
                        m_metrics.isGPUSortActive = true;
                        m_metrics.cpuSortTimeMs = 0.0f;
                        m_gpuSortNeedsRun = true;

                        if (modelChanged)
                        {
                            memcpy(pDst, pPoints, splatCount * sizeof(PackedPointGPU));
                            if (numElements > splatCount)
                            {
                                memset(pDst + splatCount, 0xFF, (numElements - splatCount) * sizeof(PackedPointGPU));
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

                        if (m_sortIndicesA.size() < splatCount)
                        {
                            m_sortIndicesA.resize(splatCount);
                            m_sortIndicesB.resize(splatCount);
                            m_sortKeys.resize(splatCount);
                            m_sortDists.resize(splatCount);
                        }

                        // 1. Calculate projected distance along camera forward vector for packed splats
                        float minD = 1e9f, maxD = -1e9f;
                        XMFLOAT3 aabbExtents(
                            pState->aabbExtents.x,
                            pState->aabbExtents.y,
                            pState->aabbExtents.z
                        );

                        #pragma omp parallel for reduction(min:minD) reduction(max:maxD)
                        for (int i = 0; i < (int)splatCount; i++)
                        {
                            const auto& p = pPoints[i];
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
                        for (int i = 0; i < (int)splatCount; i++)
                        {
                            uint32_t k = (uint32_t)((m_sortDists[i] - minD) * scale);
                            if (k > 65535) k = 65535;
                            m_sortKeys[i] = 65535 - k;
                            m_sortIndicesA[i] = i;
                        }

                        // 3. Pass 1: Radix sort low 8 bits (Byte 0)
                        uint32_t count0[256] = {};
                        for (uint32_t i = 0; i < splatCount; i++)
                            count0[m_sortKeys[i] & 0xFF]++;

                        uint32_t offset0[256] = {};
                        for (uint32_t i = 1; i < 256; i++)
                            offset0[i] = offset0[i - 1] + count0[i - 1];

                        for (uint32_t i = 0; i < splatCount; i++)
                        {
                            uint32_t idx = m_sortIndicesA[i];
                            uint8_t byte0 = (uint8_t)(m_sortKeys[idx] & 0xFF);
                            m_sortIndicesB[offset0[byte0]++] = idx;
                        }

                        // 4. Pass 2: Radix sort high 8 bits (Byte 1)
                        uint32_t count1[256] = {};
                        for (uint32_t i = 0; i < splatCount; i++)
                            count1[(m_sortKeys[m_sortIndicesB[i]] >> 8) & 0xFF]++;

                        uint32_t offset1[256] = {};
                        for (uint32_t i = 1; i < 256; i++)
                            offset1[i] = offset1[i - 1] + count1[i - 1];

                        for (uint32_t i = 0; i < splatCount; i++)
                        {
                            uint32_t idx = m_sortIndicesB[i];
                            uint8_t byte1 = (uint8_t)((m_sortKeys[idx] >> 8) & 0xFF);
                            m_sortIndicesA[offset1[byte1]++] = idx;
                        }

                        // 5. Gather sorted packed splats directly into mapped GPU upload buffer
                        #pragma omp parallel for
                        for (int i = 0; i < (int)splatCount; i++)
                        {
                            pDst[i] = pPoints[m_sortIndicesA[i]];
                        }
                        for (uint32_t i = splatCount; i < numElements; i++)
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

            m_splatBufferGPUAddress = m_pPointBuffer->GetGPUVirtualAddress();
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

                for (uint32_t r = 0; r < kUploadRing; r++)
                {
                    if (m_pChunkUploadBufferRing[r]) { m_pChunkUploadBufferRing[r]->Unmap(0, nullptr); m_pChunkUploadBufferRing[r]->Release(); m_pChunkUploadBufferRing[r] = nullptr; }
                    m_pChunkUploadBufferMappedRing[r] = nullptr;
                }
                m_pChunkUploadBuffer = nullptr;
                m_pChunkUploadBufferMapped = nullptr;
                if (m_pChunkGpuBuffer) { m_pChunkGpuBuffer->Release(); m_pChunkGpuBuffer = nullptr; }

                for (uint32_t r = 0; r < kUploadRing; r++)
                {
                    ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                        D3D12_HEAP_FLAG_NONE,
                        &CD3DX12_RESOURCE_DESC::Buffer(chunkBytes),
                        D3D12_RESOURCE_STATE_GENERIC_READ,
                        nullptr,
                        IID_PPV_ARGS(&m_pChunkUploadBufferRing[r])));
                    SetName(m_pChunkUploadBufferRing[r], "SplatRenderer::m_pChunkUploadBuffer");
                    m_pChunkUploadBufferRing[r]->Map(0, nullptr, reinterpret_cast<void**>(&m_pChunkUploadBufferMappedRing[r]));
                }

                CD3DX12_RESOURCE_DESC chunkGpuDesc = CD3DX12_RESOURCE_DESC::Buffer(chunkBytes);
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &chunkGpuDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_pChunkGpuBuffer)));
                SetName(m_pChunkGpuBuffer, "SplatRenderer::m_pChunkGpuBuffer");
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
                SetName(m_pSortedChunkIndicesGpuBuffer, "SplatRenderer::m_pSortedChunkIndicesGpuBuffer");
                m_sortedChunkIndicesState = D3D12_RESOURCE_STATE_COPY_DEST;

                for (uint32_t r = 0; r < kUploadRing; r++)
                {
                    if (m_pSortedChunkIndicesUploadBufferRing[r]) { m_pSortedChunkIndicesUploadBufferRing[r]->Unmap(0, nullptr); m_pSortedChunkIndicesUploadBufferRing[r]->Release(); m_pSortedChunkIndicesUploadBufferRing[r] = nullptr; }
                    ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                        D3D12_HEAP_FLAG_NONE,
                        &CD3DX12_RESOURCE_DESC::Buffer(idxBytes),
                        D3D12_RESOURCE_STATE_GENERIC_READ,
                        nullptr,
                        IID_PPV_ARGS(&m_pSortedChunkIndicesUploadBufferRing[r])));
                    SetName(m_pSortedChunkIndicesUploadBufferRing[r], "SplatRenderer::m_pSortedChunkIndicesUploadBuffer");
                    m_pSortedChunkIndicesUploadBufferRing[r]->Map(0, nullptr, reinterpret_cast<void**>(&m_pSortedChunkIndicesUploadBufferMappedRing[r]));
                }
                m_pSortedChunkIndicesUploadBuffer = nullptr;
                m_pSortedChunkIndicesUploadBufferMapped = nullptr;

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
                    SetName(m_pGPUSortPairBuffer, "SplatRenderer::m_pGPUSortPairBuffer");
                    m_sortPairBufferCapacityBytes = pairBytes;
                }

                // Splat mode: the first slot of every chunk, same capacity as the chunk list.
                for (uint32_t r = 0; r < kUploadRing; r++)
                {
                    if (m_pChunkSlotBaseUploadRing[r]) { m_pChunkSlotBaseUploadRing[r]->Unmap(0, nullptr); m_pChunkSlotBaseUploadRing[r]->Release(); m_pChunkSlotBaseUploadRing[r] = nullptr; }
                    ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(idxBytes),
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pChunkSlotBaseUploadRing[r])));
                    m_pChunkSlotBaseUploadRing[r]->Map(0, nullptr, reinterpret_cast<void**>(&m_pChunkSlotBaseMappedRing[r]));
                }
                m_pChunkSlotBaseUpload = nullptr;
                m_pChunkSlotBaseMapped = nullptr;
                if (m_pChunkSlotBaseGpu) { m_pChunkSlotBaseGpu->Release(); m_pChunkSlotBaseGpu = nullptr; }
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(idxBytes),
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pChunkSlotBaseGpu)));
                SetName(m_pChunkSlotBaseGpu, "SplatRenderer::m_pChunkSlotBaseGpu");
                m_chunkSlotBaseState = D3D12_RESOURCE_STATE_COPY_DEST;

                m_chunkBufferCapacityBytes = chunkBytes;
                m_sortedChunkIndicesCapacityBytes = idxBytes;
                modelChanged = true;
                needsSort = true;
                m_needUploadToGpu = true;
                m_needUploadChunkIndicesToGpu = true;
                m_gpuSortNeedsRun = true;
            }

            // This frame's upload buffers: the next entry of each ring. The list is written here, before this
            // frame's WaitForSwapChain, so the copies of up to kUploadRing - 1 earlier frames may still be
            // reading their entries. One buffer written every frame tore the list whenever its previous
            // copy was still running (on either queue): that frame drew the chunks past the tear point with
            // another list's offsets and weights, a seam with a patch missing or garbled on one side, seen
            // while the list changed every frame (refinement). The count of copies found still in flight
            // when the next list was written goes to the heartbeat.
            m_uploadRingIndex = (m_uploadRingIndex + 1) % kUploadRing;
            m_pChunkUploadBuffer = m_pChunkUploadBufferRing[m_uploadRingIndex];
            m_pChunkUploadBufferMapped = m_pChunkUploadBufferMappedRing[m_uploadRingIndex];
            m_pChunkSlotBaseUpload = m_pChunkSlotBaseUploadRing[m_uploadRingIndex];
            m_pChunkSlotBaseMapped = m_pChunkSlotBaseMappedRing[m_uploadRingIndex];
            m_pSortedChunkIndicesUploadBuffer = m_pSortedChunkIndicesUploadBufferRing[m_uploadRingIndex];
            m_pSortedChunkIndicesUploadBufferMapped = m_pSortedChunkIndicesUploadBufferMappedRing[m_uploadRingIndex];

            // The app hands over the render list every frame, but between LOD changes it is the same list.
            // Re-uploading it each frame also re-ran both GPU sorts every frame and, on the copy queue,
            // made the CPU wait on the previous frame's copy.
            const bool chunksChanged = modelChanged
                || m_lastUploadedChunks.size() != chunkCount
                || memcmp(m_lastUploadedChunks.data(), pState->pChunks, chunkCount * sizeof(MeshletChunkGPU)) != 0;
            if (chunksChanged && m_pChunkUploadBufferMapped != nullptr && pState->pChunks != nullptr && chunkCount > 0)
            {
                m_lastUploadedChunks.assign(pState->pChunks, pState->pChunks + chunkCount);
                m_heartbeatListUploads++;
                if (m_pCopyFence != nullptr && m_pCopyFence->GetCompletedValue() < m_copyFenceValue) m_heartbeatCopiesInFlight++; // The previous list's copy still running as this one is written
                MeshletChunkGPU* pDstChunks = reinterpret_cast<MeshletChunkGPU*>(m_pChunkUploadBufferMapped);
                memcpy(pDstChunks, pState->pChunks, chunkCount * sizeof(MeshletChunkGPU));
                if (numChunkElements > chunkCount)
                {
                    memset(pDstChunks + chunkCount, 0, (numChunkElements - chunkCount) * sizeof(MeshletChunkGPU));
                }
                m_needUploadChunksToGpu = true;

                // Splat mode: slot layout of this render list (exclusive prefix of the chunks' splat counts).
                if (m_pChunkSlotBaseMapped != nullptr)
                {
                    uint32_t run = 0;
                    for (uint32_t i = 0; i < chunkCount; i++)
                    {
                        m_pChunkSlotBaseMapped[i] = run;
                        run += pState->pChunks[i].splatCount;
                    }
                    m_splatSlotCount = run;
                    m_splatSortNeedsRun = true;
                }

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
    void SplatRenderer::OnCreateWindowSizeDependentResources(SwapChain* pSwapChain, uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;

        DXGI_FORMAT swapFormat = pSwapChain ? pSwapChain->GetFormat() : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        DXGI_FORMAT typelessFormat = (swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM) ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        DXGI_FORMAT unormFormat = (swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

        m_depthBuffer.InitDepthStencil(m_pDevice, "SplatRenderer::m_depthBuffer", &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), 1.0f);
        m_depthBuffer.CreateDSV(0, &m_depthBufferDSV);

        // Optimized Item Buffer (R32_UINT) and Item Depth Buffer (D32_FLOAT) for GPU Silhouette Inversion
        m_itemWidth = std::clamp(width / 4, 320u, 480u);
        m_itemHeight = std::clamp(height / 4, 180u, 270u);

        m_itemBuffer.InitRenderTarget(m_pDevice, "SplatRenderer::m_itemBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, m_itemWidth, m_itemHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        m_itemBuffer.CreateRTV(0, &m_itemRTV);
        m_itemBuffer.CreateSRV(0, &m_itemSRV);

        m_itemDepthBuffer.InitDepthStencil(m_pDevice, "SplatRenderer::m_itemDepthBuffer",
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

        m_sceneColorBuffer.Init(m_pDevice, "SplatRenderer::m_sceneColorBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET, &sceneClear);
        m_sceneColorBuffer.CreateRTV(0, &m_sceneColorRTV, 0, -1, -1, swapFormat);
        m_sceneColorBuffer.CreateRTV(0, &m_sceneColorRTVUnorm, 0, -1, -1, DXGI_FORMAT_R8G8B8A8_UNORM); // Splat mode, display-space blending
        m_sceneColorBuffer.CreateSRV(0, &m_sceneColorSRV);

        m_loupeColor.Init(m_pDevice, "SplatRenderer::m_loupeColor",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET, &sceneClear);
        m_loupeColor.CreateRTV(0, &m_loupeRTV, 0, -1, -1, swapFormat);
        m_loupeColor.CreateRTV(0, &m_loupeRTVUnorm, 0, -1, -1, DXGI_FORMAT_R8G8B8A8_UNORM);
        m_loupeDepth.InitDepthStencil(m_pDevice, "SplatRenderer::m_loupeDepth", &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), 1.0f);
        m_loupeDepth.CreateDSV(0, &m_loupeDSV);

        m_historyColorBuffer.Init(m_pDevice, "SplatRenderer::m_historyColorBuffer",
            &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr);
        m_historyColorBuffer.CreateSRV(0, &m_historyColorSRV);
        m_historyColorBuffer.CreateUAV(0, &m_historyColorUAV);

        m_resolvedColorBuffer.Init(m_pDevice, "SplatRenderer::m_resolvedColorBuffer",
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
    void SplatRenderer::OnDestroyWindowSizeDependentResources()
    {
        m_loupeColor.OnDestroy();
        m_loupeDepth.OnDestroy();
        m_sceneColorBuffer.OnDestroy();
        m_historyColorBuffer.OnDestroy();
        m_resolvedColorBuffer.OnDestroy();
        m_itemBuffer.OnDestroy();
        m_itemDepthBuffer.OnDestroy();
        m_depthBuffer.OnDestroy();
    }

    void SplatRenderer::OnUpdateDisplayDependentResources(SwapChain* /*pSwapChain*/) {}

    // The whole frame: upload splat/chunk/occlusion buffers, run the silhouette prepass and GPU sort
    // if needed, draw the occluder + main splat passes, then resolve TAA and draw ImGui on top.
    void SplatRenderer::OnRender(State* pState, SwapChain* pSwapChain)
    {
        static uint32_t s_frameCounter = 0;
        s_frameCounter++;
        if (s_frameCounter % 60 == 0)
        {
            LogTransitionTrace("Renderer Heartbeat: frame=%u, chunkCount=%u, splatCount=%u, fps=%.1f, gpuWait=%.1fms, record=%.1fms, upload=%.1fms, mode=%u, sorts=%u, listUploads=%u, listCopiesInFlight=%u",
                s_frameCounter, pState->chunkCount, pState->splatCount, m_metrics.frameRate,
                m_smoothGpuWaitMs, m_smoothCommandRecordMs, m_smoothUploadMs, pState->renderMode, m_heartbeatSorts, m_heartbeatListUploads, m_heartbeatCopiesInFlight);
            m_heartbeatCopiesInFlight = 0;
            m_heartbeatSorts = 0;
            m_heartbeatListUploads = 0;
        }

        // Throttle CPU so it does not overwrite in-flight command allocator / dynamic buffers
        const auto tWaitStart = std::chrono::high_resolution_clock::now();
        pSwapChain->WaitForSwapChain();
        const auto tWaitEnd = std::chrono::high_resolution_clock::now();

        // Marks the end of everything submitted to the graphics queue before this frame; this frame's copy
        // waits on it (see the copy-queue upload below)
        if (m_pGraphicsProgressFence != nullptr)
            m_pDevice->GetGraphicsQueue()->Signal(m_pGraphicsProgressFence, ++m_graphicsProgressValue);

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

        // Splat mode with display-space blending draws into a plain UNORM view of the target, so the stored
        // display-space colours blend as they are (the reference viewer's behaviour); every other case uses
        // the sRGB view, whose blending is linear.
        const bool splatDisplayBlend = (SortedGaussians(pState->renderMode) && pState->splatBlendSpace == 0);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = useTemporal ? m_sceneColorRTV.GetCPU() : *pSwapChain->GetCurrentBackBufferRTV();
        if (splatDisplayBlend)
        {
            if (useTemporal)
            {
                rtvHandle = m_sceneColorRTVUnorm.GetCPU();
            }
            else if (m_pBackBufferUnormRtvHeap != nullptr)
            {
                D3D12_RENDER_TARGET_VIEW_DESC unormRtv = {};
                unormRtv.Format = m_unormFormat;
                unormRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvHandle = m_pBackBufferUnormRtvHeap->GetCPUDescriptorHandleForHeapStart();
                m_pDevice->GetDevice()->CreateRenderTargetView(pBackBuffer, &unormRtv, rtvHandle);
            }
        }
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
        UpdateSplatBuffers(pState, eyePos, forwardNorm);
        UpdateOcclusionVoxelBuffer(pState);
        UpdateShadowMeshBuffers(pState);
        UploadMesh(m_occluderMesh, pState->pOccluderMeshVertices, pState->occluderMeshVertexCount, pState->pOccluderMeshIndices,
                   pState->occluderMeshIndexCount, pState->occluderMeshVersion);
        uint32_t splatCount = pState->splatCount;

        // Compute Geometry Optimization & Culling Statistics
        m_cullStats = {};
        uint32_t baseDatasetSplats = (pState->totalDatasetSplats > 0) ? pState->totalDatasetSplats : splatCount;
        uint32_t baseDatasetChunks  = (pState->totalDatasetChunks > 0)  ? pState->totalDatasetChunks  : pState->chunkCount;
        m_cullStats.totalDatasetSplats = baseDatasetSplats;
        m_cullStats.totalDatasetChunks  = baseDatasetChunks;
        m_cullStats.lodActiveSplats    = splatCount;
        m_cullStats.lodActiveChunks     = pState->chunkCount;
        m_cullStats.lodPrunedSplats    = (baseDatasetSplats > splatCount) ? (baseDatasetSplats - splatCount) : 0;

        XMMATRIX activeCullMatrix = pState->detachCullCamera ? cViewProj : viewProj;
        XMFLOAT3 activeCullEye = pState->detachCullCamera ? cullEyePos : eyePos;

        if (pState->useChunkedPipeline && pState->chunkCount > 0 && pState->pChunks != nullptr)
        {
            uint32_t frustumCulledChunks = 0;
            uint32_t frustumCulledSplats = 0;
            uint32_t coneCulledChunks = 0;
            uint32_t coneCulledSplats = 0;
            uint32_t passedChunks = 0;
            uint32_t passedSplats = 0;

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
                    frustumCulledSplats += chunk.splatCount;
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
                        coneCulledSplats += chunk.splatCount;
                    }
                    else
                    {
                        passedChunks++;
                        passedSplats += chunk.splatCount;
                    }
                }
            }

            m_cullStats.asFrustumCulledChunks  = frustumCulledChunks;
            m_cullStats.asFrustumCulledSplats = frustumCulledSplats;
            m_cullStats.asConeCulledChunks     = coneCulledChunks;
            m_cullStats.asConeCulledSplats    = coneCulledSplats;
            m_cullStats.asPassedChunks         = passedChunks;
            m_cullStats.asPassedSplats        = passedSplats;
            m_cullStats.msDrawnSplats         = passedSplats;
        }
        else
        {
            m_cullStats.asPassedChunks  = pState->chunkCount;
            m_cullStats.asPassedSplats = splatCount;
            m_cullStats.msDrawnSplats  = splatCount;
        }

        m_cullStats.generatedVertices  = m_cullStats.msDrawnSplats * 4;
        m_cullStats.generatedTriangles = m_cullStats.msDrawnSplats * 2;

        if (m_cullStats.totalDatasetSplats > 0)
        {
            m_cullStats.totalCullingRatio = ((float)(m_cullStats.totalDatasetSplats - m_cullStats.msDrawnSplats) / (float)m_cullStats.totalDatasetSplats) * 100.0f;
            m_cullStats.lodDecimationRatio = ((float)m_cullStats.lodPrunedSplats / (float)m_cullStats.totalDatasetSplats) * 100.0f;
        }
        if (m_cullStats.lodActiveSplats > 0)
        {
            m_cullStats.asCullingRatio = ((float)(m_cullStats.asFrustumCulledSplats + m_cullStats.asConeCulledSplats) / (float)m_cullStats.lodActiveSplats) * 100.0f;
        }
        float bytesPerSplat = (pState->renderMode == 2) ? (float)sizeof(SplatPoint) : SplatRecords(pState->renderMode) ? (float)(sizeof(PackedSplatGPU) + pState->shRecordBytes) : (float)sizeof(PackedPointGPU);
        m_cullStats.vramBandwidthSavedMB = (float)(m_cullStats.lodPrunedSplats + m_cullStats.asFrustumCulledSplats + m_cullStats.asConeCulledSplats) * bytesPerSplat / (1024.0f * 1024.0f);


        SplatCB* pCB = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = 0;
        if (!m_constantBufferRing.AllocConstantBuffer(sizeof(SplatCB), (void**)&pCB, &cbAddress))
        {
            Trace("SplatRenderer: failed to allocate frame constant buffer, skipping draw\n");

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
        pCB->splatCount = splatCount;
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
        // Vertex-shader paths: per-splat cone culling always on (the task-shader toggle is greyed out). Mode 4:
        // off on every path; the chunk cones are built on the CPU from decoded splat normals, which for a
        // splat package are placeholders (the sorted pipeline never cone-culls either).
        pCB->enableConeCulling = (pState->renderMode != 4 && (pState->enableConeCulling || m_renderPath != RenderPath::MeshShaders)) ? 1 : 0;
        pCB->showOnlyLocked = pState->showOnlyLockedChunks ? 1 : 0;
        pCB->showChunkStream = pState->showChunkStream ? 1 : 0;
        pCB->arrivalGlowIntensity = pState->arrivalGlowIntensity;
        pCB->arrivalGlowHue = pState->arrivalGlowHue;
        pCB->autoSplatSize = pState->autoSplatSize ? 1u : 0u;
        pCB->culledPass = 0;
        pCB->autoSplatPad1[0] = 0.0f; pCB->autoSplatPad1[1] = 0.0f;
        pCB->lodRadiusAlign[0] = 0.0f; pCB->lodRadiusAlign[1] = 0.0f;
        for (int l = 0; l < 8; l++) pCB->lodRadius[l] = pState->lodRadius[l];
        for (int l = 0; l < 8; l++) pCB->splatDiscRadius[l] = pState->splatDiscRadius[l];
        pCB->splatDiscOpacityFloor = pState->splatDiscOpacityFloor;
        pCB->splatViz = std::min(pState->splatViz, 3u);
        pCB->splatDiscPad[0] = 0.0f; pCB->splatDiscPad[1] = 0.0f;
        // Splat mode
        XMStoreFloat4x4(&pCB->view, view);
        pCB->viewportW = (float)m_width;
        pCB->viewportH = (float)m_height;
        pCB->proj00 = XMVectorGetX(proj.r[0]);
        pCB->splatScaleLog2Min = pState->splatScaleLog2Min;
        pCB->splatScaleLog2Max = pState->splatScaleLog2Max;
        pCB->shDegree = pState->shDegree;
        pCB->shRecordBytes = pState->shRecordBytes;
        pCB->splatBlendSpace = pState->splatBlendSpace;
        pCB->camForward = forwardNorm;
        pCB->splatSlotCount = SortedGaussians(pState->renderMode) ? m_splatSlotCount : 0;
        pCB->enableOcclusionCulling = (pState->enableOcclusionCulling && pState->occlusionVoxelCount > 0 && m_pOcclusionVoxelBuffer != nullptr) ? 1 : 0;
        pCB->showOcclusionVolumeOnly = pState->showOcclusionVolumeOnly ? 1 : 0;

        // Interior occlusion volume: the voxel buffer holds the whole mip chain, but only ONE mip is drawn
        // per frame (see SelectOcclusionMip). Hand the shader that mip's block range. A volume with no
        // table (a pre-v6 package, or a table that does not add up) is one mip covering the array.
        uint32_t occluderBlockCount = 0;
        {
            OcclusionMipTable mips = pState->occlusionMips;
            if (mips.mipCount == 0 || mips.mipCount > kMaxOcclusionMips || mips.TotalBlocks() != pState->occlusionVoxelCount)
                mips = OcclusionMipTable::SingleLevel(pState->pOcclusionVoxels, pState->occlusionVoxelCount);
            const uint32_t mip = SelectOcclusionMip(pState, mips, eyePos);
            pCB->occlusionVoxelFirst = mips.FirstBlock(mip);
            pCB->occlusionVoxelCount = (mip < mips.mipCount) ? mips.blockCount[mip] : 0;
            occluderBlockCount = pCB->occlusionVoxelCount;
        }

        ID3D12Resource* pGpuRes = (pState->renderMode == 2) ? m_pRawSplatGpuBuffer : SplatRecords(pState->renderMode) ? m_pSplatGpuBuffer : m_pPointGpuBuffer;
        ID3D12Resource* pGpuOutRes = (pState->renderMode == 2) ? m_pRawSplatGpuOutBuffer : SplatRecords(pState->renderMode) ? nullptr : m_pSplatGpuOutBuffer;
        ID3D12Resource* pUploadRes = (pState->renderMode == 2) ? m_pRawSplatBuffer : SplatRecords(pState->renderMode) ? m_pSplatBuffer : m_pPointBuffer;
        auto dispatchStart = std::chrono::high_resolution_clock::now();
        m_metrics.uploadTimeMs = 0.0f;
        m_metrics.silhouettePrepassTimeMs = 0.0f;
        m_metrics.occluderPassTimeMs = 0.0f;
        m_metrics.mainDispatchTimeMs = 0.0f;
        m_metrics.taaResolveTimeMs = 0.0f;

        // Interior Occlusion Volume: solid depth-writing cubes drawn before the splat pass so far-side
        // splats visible through gaps in the near side get discarded by the main pass's depth test
        // (see m_pPipelineStateOcclusionTest). Reuses the frame's already-allocated SplatCB (cbAddress) --
        // it only needs viewProj/eye and the occlusion-specific fields added to that struct.
        auto DrawOccluderPass = [&]()
        {
            // Drawn whenever culling is enabled OR the user just wants to look at the volume on its
            // own -- "view only" must not require "enable culling" too, or the two together would draw
            // neither the occluder (culling off) nor the splats (view-only skips them): a blank screen.
            // View Shadow Mesh draws the mesh in place of the occlusion volume (and of the splats)
            if (pState->showShadowMesh)
            {
                ID3D12PipelineState* pMeshPSO = splatDisplayBlend ? m_pShadowMeshPSOUnorm : m_pShadowMeshPSO;
                if (pMeshPSO == nullptr || m_shadowMeshIndexCount == 0) return;
                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(pMeshPSO);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, m_pShadowMeshIndexBuffer->GetGPUVirtualAddress());  // t0
                pCmdLst->SetGraphicsRootShaderResourceView(2, m_pShadowMeshVertexBuffer->GetGPUVirtualAddress()); // t1
                pCmdLst->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                pCmdLst->DrawInstanced(m_shadowMeshIndexCount, 1, 0, 0);
                return;
            }
            // Splat mode with display-space blending draws into the UNORM view: use the occluder PSO built for it
            ID3D12PipelineState* pOccluderPSO = splatDisplayBlend ? m_pOccluderPSOUnorm : m_pOccluderPSO;
            if (pOccluderPSO == nullptr) return;
            bool wantOccluderVisible = pState->enableOcclusionCulling || pState->showOcclusionVolumeOnly;
            // Format 13: the occluder mesh holds the depth the splats are tested against, then the shadow mesh fills the gaps
            ID3D12PipelineState* pMeshOccluderPSO = splatDisplayBlend ? m_pShadowMeshPSOUnorm : m_pShadowMeshPSO;
            if (wantOccluderVisible && m_occluderMesh.indexCount > 0 && pMeshOccluderPSO != nullptr)
            {
                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(pMeshOccluderPSO);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, m_occluderMesh.indices->GetGPUVirtualAddress());
                pCmdLst->SetGraphicsRootShaderResourceView(2, m_occluderMesh.vertices->GetGPUVirtualAddress());
                pCmdLst->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                pCmdLst->DrawInstanced(m_occluderMesh.indexCount, 1, 0, 0);
                ID3D12PipelineState* pFill = splatDisplayBlend ? m_pShadowMeshFillPSOUnorm : m_pShadowMeshFillPSO;
                if (!pState->showOcclusionVolumeOnly && pFill != nullptr && m_shadowMeshIndexCount > 0)
                {
                    pCmdLst->SetPipelineState(pFill);
                    pCmdLst->SetGraphicsRootShaderResourceView(1, m_pShadowMeshIndexBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetGraphicsRootShaderResourceView(2, m_pShadowMeshVertexBuffer->GetGPUVirtualAddress());
                    pCmdLst->DrawInstanced(m_shadowMeshIndexCount, 1, 0, 0);
                }
                return;
            }
            if (!wantOccluderVisible || pState->occlusionVoxelCount == 0 || occluderBlockCount == 0 || m_pOcclusionVoxelBuffer == nullptr || m_pOccluderPSO == nullptr)
                return;

            auto occluderStart = std::chrono::high_resolution_clock::now();

            pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
            pCmdLst->SetPipelineState(pOccluderPSO);
            pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
            pCmdLst->SetGraphicsRootShaderResourceView(1, 0); // Unused by occluderMS/PS
            pCmdLst->SetGraphicsRootShaderResourceView(2, 0); // Unused by occluderMS/PS
            pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
            pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);
            pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer->GetGPUVirtualAddress());

            if (m_renderPath == RenderPath::MeshShaders)
            {
                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> occluderCmd6;
                pCmdLst->QueryInterface(IID_PPV_ARGS(&occluderCmd6));
                // One threadgroup per block of the selected mip only (the shader offsets by g_OcclusionVoxelFirst).
                uint32_t dimX = std::min(occluderBlockCount, 32768u);
                uint32_t dimY = (dimX > 0) ? ((occluderBlockCount + dimX - 1) / dimX) : 1;
                occluderCmd6->DispatchMesh(dimX, dimY, 1);
            }
            else
            {
                // Vertex-shader path: one instance per block, 36 vertices (six faces of two triangles); occluderVS drops buried faces.
                pCmdLst->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                pCmdLst->DrawInstanced(36, occluderBlockCount, 0, 0);
            }

            // Gap fill: with a shadow mesh, the mesh's colour covers the cubes (they lie inside it), so gaps between the
            // splats show the model's colour; the cubes still hold the depth the splats are tested against
            ID3D12PipelineState* pFillPSO = splatDisplayBlend ? m_pShadowMeshFillPSOUnorm : m_pShadowMeshFillPSO;
            if (!pState->showOcclusionVolumeOnly && pFillPSO != nullptr && m_shadowMeshIndexCount > 0)
            {
                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(pFillPSO);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, m_pShadowMeshIndexBuffer->GetGPUVirtualAddress());
                pCmdLst->SetGraphicsRootShaderResourceView(2, m_pShadowMeshVertexBuffer->GetGPUVirtualAddress());
                pCmdLst->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                pCmdLst->DrawInstanced(m_shadowMeshIndexCount, 1, 0, 0);
            }

            auto occluderEnd = std::chrono::high_resolution_clock::now();
            m_metrics.occluderPassTimeMs = std::chrono::duration<float, std::milli>(occluderEnd - occluderStart).count();
        };
        ID3D12PipelineState* pMainSplatPSO = (pState->enableOcclusionCulling && m_pPipelineStateOcclusionTest != nullptr)
            ? m_pPipelineStateOcclusionTest
            : m_pPipelineState;
        if (SortedGaussians(pState->renderMode))
        {
            const bool linear = (pState->splatBlendSpace == 1 && m_pSplatPSOLinear != nullptr);
            const bool occlude = pState->enableOcclusionCulling && ((pState->occlusionVoxelCount > 0 && m_pOcclusionVoxelBuffer != nullptr) || m_occluderMesh.indexCount > 0);
            pMainSplatPSO = linear ? ((occlude && m_pSplatPSOLinearDepth) ? m_pSplatPSOLinearDepth : m_pSplatPSOLinear)
                                   : ((occlude && m_pSplatPSODisplayDepth) ? m_pSplatPSODisplayDepth : m_pSplatPSODisplay);
        }

        // Detach Camera: the three-pass sequence needs its PSOs and a second constant buffer that only
        // differs in culledPass (the shader draws either the frozen camera's splats or the culled ones).
        const bool detachedPasses = pState->renderMode != 3 && pState->detachCullCamera && m_pPipelineStateDetachedMain && m_pPipelineStateCulledDepth && m_pPipelineStateCulledColor;
        D3D12_GPU_VIRTUAL_ADDRESS cbCulledAddress = 0;
        if (detachedPasses)
        {
            SplatCB* pCBCulled = nullptr;
            if (m_constantBufferRing.AllocConstantBuffer(sizeof(SplatCB), (void**)&pCBCulled, &cbCulledAddress))
            {
                *pCBCulled = *pCB;
                pCBCulled->culledPass = 1;
            }
            else
            {
                cbCulledAddress = 0;
            }
        }
        // Issues the main splat draw (DispatchMesh or DrawInstanced) with the bound PSO and constant buffer.
        // In Detach Camera mode it runs the three passes described in SplatRenderer.h instead of one.
        auto issueSplatDraws = [&](uint32_t asGroupCount, uint32_t instances)
        {
            auto draw = [&]()
            {
                if (m_renderPath == RenderPath::MeshShaders)
                {
                    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
                    pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
                    // A dispatch dimension is limited to 65535 groups; splat mode can need more (one per
                    // 64 slots), so the count folds over y and the amplification shader unfolds it.
                    const uint32_t gx = std::min<uint32_t>(asGroupCount, 65535u);
                    const uint32_t gy = (asGroupCount + 65534u) / 65535u;
                    cmdList6->DispatchMesh(gx, std::max<uint32_t>(gy, 1u), 1);
                }
                else
                {
                    // Vertex-shader path: one instance per splat slot (64 per sorted chunk), six vertices each (see mainVS).
                    pCmdLst->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    pCmdLst->DrawInstanced(6, instances, 0, 0);
                }
            };
            if (detachedPasses && cbCulledAddress != 0)
            {
                pCmdLst->SetPipelineState(m_pPipelineStateDetachedMain);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                draw();
                pCmdLst->SetPipelineState(m_pPipelineStateCulledDepth);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbCulledAddress);
                draw();
                pCmdLst->SetPipelineState(m_pPipelineStateCulledColor);
                draw();
            }
            else
            {
                pCmdLst->SetPipelineState(pMainSplatPSO);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                draw();
            }
        };

        bool chunkedPassDrawn = false; // The loupe replays the chunked pass (see below)
        if (splatCount > 0 && pGpuRes != nullptr && pUploadRes != nullptr)
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
                        if (SplatRecords(pState->renderMode) && m_pSHGpuBuffer != nullptr && m_pSHBuffer != nullptr)
                            m_pCopyCmdList->CopyResource(m_pSHGpuBuffer, m_pSHBuffer);
                    }
                    if (m_needUploadChunksToGpu && m_pChunkGpuBuffer != nullptr && m_pChunkUploadBuffer != nullptr)
                    {
                        m_pCopyCmdList->CopyResource(m_pChunkGpuBuffer, m_pChunkUploadBuffer);
                        if (m_pChunkSlotBaseGpu != nullptr && m_pChunkSlotBaseUpload != nullptr)
                            m_pCopyCmdList->CopyResource(m_pChunkSlotBaseGpu, m_pChunkSlotBaseUpload);
                    }

                    m_pCopyCmdList->Close();
                    ID3D12CommandList* ppCopyLists[] = { m_pCopyCmdList };
                    // The destinations are the buffers the previous frame's sort and draw read, and that frame
                    // may still be running on the graphics queue: the copy waits for everything submitted to
                    // it before this frame (the progress fence signalled at the top of OnRender). Without the
                    // wait the copy landed mid-draw and the previous frame drew a half-replaced list -- single
                    // splats, patches or the whole model wrong for one frame, while the list changed every frame.
                    if (m_pGraphicsProgressFence != nullptr)
                        m_pCopyQueue->Wait(m_pGraphicsProgressFence, m_graphicsProgressValue);
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
                        m_chunkSlotBaseState = D3D12_RESOURCE_STATE_COMMON; // Copy-queue work leaves buffers in COMMON; they promote on first use
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
                        if (SplatRecords(pState->renderMode) && m_pSHGpuBuffer != nullptr && m_pSHBuffer != nullptr)
                            pCmdLst->CopyResource(m_pSHGpuBuffer, m_pSHBuffer);

                        D3D12_RESOURCE_BARRIER postCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                            pGpuRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        pCmdLst->ResourceBarrier(1, &postCopy);
                    }
                    if (m_needUploadChunksToGpu && m_pChunkSlotBaseGpu != nullptr && m_pChunkSlotBaseUpload != nullptr)
                    {
                        if (m_chunkSlotBaseState != D3D12_RESOURCE_STATE_COPY_DEST && m_chunkSlotBaseState != D3D12_RESOURCE_STATE_COMMON)
                        {
                            D3D12_RESOURCE_BARRIER pre = CD3DX12_RESOURCE_BARRIER::Transition(m_pChunkSlotBaseGpu, m_chunkSlotBaseState, D3D12_RESOURCE_STATE_COPY_DEST);
                            pCmdLst->ResourceBarrier(1, &pre);
                        }
                        pCmdLst->CopyResource(m_pChunkSlotBaseGpu, m_pChunkSlotBaseUpload);
                        m_chunkSlotBaseState = D3D12_RESOURCE_STATE_COMMON; // Promotes to the SRV state on first use
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
                        pCmdLst->SetComputeRootShaderResourceView(1, m_pPointGpuBuffer ? m_pPointGpuBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootShaderResourceView(2, m_pRawSplatGpuBuffer ? m_pRawSplatGpuBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootShaderResourceView(3, m_pChunkGpuBuffer->GetGPUVirtualAddress());
                        pCmdLst->SetComputeRootUnorderedAccessView(4, m_pGPUSortPairBuffer->GetGPUVirtualAddress());
                        pCmdLst->SetComputeRootUnorderedAccessView(5, m_pSplatGpuOutBuffer ? m_pSplatGpuOutBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootUnorderedAccessView(6, m_pRawSplatGpuOutBuffer ? m_pRawSplatGpuOutBuffer->GetGPUVirtualAddress() : 0);
                        pCmdLst->SetComputeRootUnorderedAccessView(7, m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress());

                        struct BitonicCB
                        {
                            XMFLOAT3 camPos;
                            float    pad0;
                            XMFLOAT3 camForward;
                            uint32_t totalSplats;
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
                        baseCB.totalSplats = chunkCount;
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

                D3D12_GPU_VIRTUAL_ADDRESS splatAddr = pGpuRes->GetGPUVirtualAddress();

                if (SortedGaussians(pState->renderMode))
                {
                    XMFLOAT4X4 cullVP;
                    XMStoreFloat4x4(&cullVP, pState->detachCullCamera ? cViewProj : unjitteredViewProj);
                    RunSplatSort(pCmdLst, pState, eyePos, forwardNorm, cullVP);
                }

                DrawOccluderPass();
                chunkedPassDrawn = true;

                if (!pState->showOcclusionVolumeOnly && !pState->showShadowMesh)
                {
                    auto mainDispatchStart = std::chrono::high_resolution_clock::now();

                    pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                    pCmdLst->SetGraphicsRootShaderResourceView(1, splatAddr);
                    pCmdLst->SetGraphicsRootShaderResourceView(2, splatAddr);
                    pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer ? m_pOcclusionVoxelBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(6, m_pSplatGpuBuffer ? m_pSplatGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(7, m_pSHGpuBuffer ? m_pSHGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(8, m_pSplatPairsA ? m_pSplatPairsA->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(9, m_pSlotInfoBuffer ? m_pSlotInfoBuffer->GetGPUVirtualAddress() : 0);

                    const bool splatMode = SortedGaussians(pState->renderMode);
                    const uint32_t asGroupCount = splatMode ? ((m_splatSlotCount + SPLATS_PER_GROUP - 1) / SPLATS_PER_GROUP) : ((chunkCount + AS_GROUP_SIZE - 1) / AS_GROUP_SIZE);
                    const uint32_t instances = splatMode ? m_splatSlotCount : (pState->useChunkedPipeline ? chunkCount * SPLATS_PER_GROUP : pState->splatCount);
                    if (!splatMode || (m_pSplatPairsA != nullptr && m_pSlotInfoBuffer != nullptr && m_splatSlotCount > 0))
                        issueSplatDraws(asGroupCount, instances);

                    auto mainDispatchEnd = std::chrono::high_resolution_clock::now();
                    m_metrics.mainDispatchTimeMs += std::chrono::duration<float, std::milli>(mainDispatchEnd - mainDispatchStart).count();
                }
            }
            else if (SortedGaussians(pState->renderMode))
            {
                // Splat mode expands the chunk list into its sort; without a chunk list there is nothing to draw.
            }
            // Execute Flat GPU LDS Key-Index Sorting Pipeline
            else if (pState->gpuRadixSort && m_pProjectKeysPSO && m_pBitonicLocalSortPSO && m_pBitonicGlobalSortPSO && m_pBitonicLocalMergePSO && m_pGatherSplatsPSO && m_pComputeRootSignature && pGpuOutRes != nullptr && m_pGPUSortPairBuffer != nullptr)
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
                    pCmdLst->SetComputeRootShaderResourceView(1, m_pPointGpuBuffer ? m_pPointGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootShaderResourceView(2, m_pRawSplatGpuBuffer ? m_pRawSplatGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootUnorderedAccessView(4, m_pGPUSortPairBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetComputeRootUnorderedAccessView(5, m_pSplatGpuOutBuffer ? m_pSplatGpuOutBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootUnorderedAccessView(6, m_pRawSplatGpuOutBuffer ? m_pRawSplatGpuOutBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetComputeRootUnorderedAccessView(7, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);

                    uint32_t numElements = 1;
                    while (numElements < splatCount) numElements <<= 1;

                    struct BitonicCB
                    {
                        XMFLOAT3 camPos;
                        float    pad0;
                        XMFLOAT3 camForward;
                        uint32_t totalSplats;
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
                    baseCB.totalSplats = splatCount;
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
                            pCmdLst->SetPipelineState(m_pGatherSplatsPSO);
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
            ID3D12Resource* pDrawSplatRes = isFlatGpuSorted ? pGpuOutRes : pGpuRes;

            D3D12_GPU_VIRTUAL_ADDRESS drawSplatAddr = (pDrawSplatRes != nullptr) ? pDrawSplatRes->GetGPUVirtualAddress() : m_splatBufferGPUAddress;

#if 0 // DYNAMIC SILHOUETTE: GPU silhouette chunk processing switched off for now (re-enable by restoring this block)
            // -------------------------------------------------------------------------
            // GPU ITEM PREPASS & SILHOUETTE EDGE EXTRACTION (Option 2)
            // -------------------------------------------------------------------------
            uint32_t bitmaskDwords = (pState->chunkCount + 31) / 32;
            if (bitmaskDwords < 64) bitmaskDwords = 64;
            uint32_t bitmaskBytes = bitmaskDwords * sizeof(uint32_t);

            bool edgeNeedsRecalc = false;
            if (pState->renderMode != 3 && pState->enableGpuSilhouetteInversion && pState->chunkCount > 0 && m_pItemPrepassPSO != nullptr && m_itemBuffer.GetResource() != nullptr)
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
                    SetName(m_pSilhouetteBitmaskGpuBuffer, "SplatRenderer::m_pSilhouetteBitmaskGpuBuffer");

                    ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
                        D3D12_HEAP_FLAG_NONE,
                        &CD3DX12_RESOURCE_DESC::Buffer(bitmaskBytes),
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr,
                        IID_PPV_ARGS(&m_pSilhouetteReadbackBuffer)));
                    SetName(m_pSilhouetteReadbackBuffer, "SplatRenderer::m_pSilhouetteReadbackBuffer");

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
                pCmdLst->SetGraphicsRootShaderResourceView(1, drawSplatAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(2, drawSplatAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer ? m_pOcclusionVoxelBuffer->GetGPUVirtualAddress() : 0);

                uint32_t totalItemGroups = (pState->useChunkedPipeline && pState->chunkCount > 0) ? pState->chunkCount : ((splatCount + SPLATS_PER_GROUP - 1) / SPLATS_PER_GROUP);
                if (m_renderPath == RenderPath::MeshShaders)
                {
                    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> itemCmd6;
                    pCmdLst->QueryInterface(IID_PPV_ARGS(&itemCmd6));
                    uint32_t dimX = std::min(totalItemGroups, 32768u);
                    uint32_t dimY = (dimX > 0) ? ((totalItemGroups + dimX - 1) / dimX) : 1;
                    itemCmd6->DispatchMesh(dimX, dimY, 1);
                }
                else
                {
                    // Vertex-shader path: 64 instances per chunk (or one per splat, flat), six vertices each (see itemVS).
                    pCmdLst->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    pCmdLst->DrawInstanced(6, totalItemGroups * SPLATS_PER_GROUP, 0, 0);
                }

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
#endif // DYNAMIC SILHOUETTE

            // Flat pipeline only: the chunked branch above has already drawn the occluder and the splats,
            // and splat mode without a chunk list has nothing to draw. Drawing again here doubled the GPU
            // work and, in splat mode, blended the farthest slots a second time over the finished image.
            const bool flatPipeline = !useChunked && !SortedGaussians(pState->renderMode);
            if (flatPipeline)
                DrawOccluderPass();

            // Draw Main Mesh Shader Pass
            if (flatPipeline && !pState->showOcclusionVolumeOnly && !pState->showShadowMesh)
            {
                auto mainDispatchStart2 = std::chrono::high_resolution_clock::now();

                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetGraphicsRootShaderResourceView(1, drawSplatAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(2, drawSplatAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer ? m_pOcclusionVoxelBuffer->GetGPUVirtualAddress() : 0);

                uint32_t groupCount = (pState->useChunkedPipeline && pState->chunkCount > 0)
                    ? ((pState->chunkCount + 32 - 1) / 32)
                    : ((splatCount + SPLATS_PER_GROUP - 1) / SPLATS_PER_GROUP);

                if (pState->chunkCount == 0 || groupCount == 0)
                {
                    LogTransitionTrace("SplatRenderer::OnRender WARNING: chunkCount=%u, splatCount=%u, groupCount=%u, useChunkedPipeline=%d",
                        pState->chunkCount, splatCount, groupCount, pState->useChunkedPipeline ? 1 : 0);
                }

                const uint32_t instances = (pState->useChunkedPipeline && pState->chunkCount > 0) ? pState->chunkCount * SPLATS_PER_GROUP : splatCount;
                issueSplatDraws(groupCount, instances);

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

        // Loupe: the chunked pass again, through a crop of the unjittered projection that blows the box around
        // the loupe centre up to the inset, into its own target, then copied over the back buffer's corner.
        // The sort is the frame's own (same eye). Scaling g_Proj00 with the crop keeps the Gaussians' screen
        // footprint exact: the shader derives the covariance's focal length from it and the viewport.
        if (pState->loupeEnabled && chunkedPassDrawn && m_loupeColor.GetResource() != nullptr && m_width > 0 && m_height > 0)
        {
            SplatCB* pLoupeCB = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS loupeCbAddress = 0;
            if (m_constantBufferRing.AllocConstantBuffer(sizeof(SplatCB), (void**)&pLoupeCB, &loupeCbAddress))
            {
                // The inset's size; its target is window-sized, so the loupe draws into the top-left corner of it.
                const uint32_t lw = std::min(std::max(16u, pState->loupeWidth), m_width);
                const uint32_t lh = std::min(std::max(16u, pState->loupeHeight), m_height);
                const float zoom = std::max(1.0f, pState->loupeZoom);
                const float sx = zoom * (float)m_width / (float)lw;
                const float sy = zoom * (float)m_height / (float)lh;
                const float cx = 2.0f * pState->loupeCenterX / (float)m_width - 1.0f;
                const float cy = 1.0f - 2.0f * pState->loupeCenterY / (float)m_height;
                const XMMATRIX crop(sx, 0.0f, 0.0f, 0.0f,
                                    0.0f, sy, 0.0f, 0.0f,
                                    0.0f, 0.0f, 1.0f, 0.0f,
                                    -sx * cx, -sy * cy, 0.0f, 1.0f);
                const XMMATRIX loupeViewProj = XMMatrixMultiply(unjitteredViewProj, crop);
                *pLoupeCB = *pCB;
                XMStoreFloat4x4(&pLoupeCB->viewProj, loupeViewProj);
                if (!pState->detachCullCamera) XMStoreFloat4x4(&pLoupeCB->cullViewProj, loupeViewProj);
                pLoupeCB->viewportW = (float)lw;
                pLoupeCB->viewportH = (float)lh;
                pLoupeCB->proj00 = XMVectorGetX(proj.r[0]) * sx;
                pLoupeCB->splatViz = std::min(pState->loupeViz, 3u);

                D3D12_CPU_DESCRIPTOR_HANDLE loupeRtv = splatDisplayBlend ? m_loupeRTVUnorm.GetCPU() : m_loupeRTV.GetCPU();
                D3D12_CPU_DESCRIPTOR_HANDLE loupeDsv = m_loupeDSV.GetCPU();
                pCmdLst->OMSetRenderTargets(1, &loupeRtv, FALSE, &loupeDsv);
                CD3DX12_RECT loupeScissor(0, 0, (LONG)lw, (LONG)lh);
                pCmdLst->ClearRenderTargetView(loupeRtv, clearColor, 1, &loupeScissor);
                pCmdLst->ClearDepthStencilView(loupeDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &loupeScissor);
                CD3DX12_VIEWPORT loupeViewport(0.0f, 0.0f, (float)lw, (float)lh);
                pCmdLst->RSSetViewports(1, &loupeViewport);
                pCmdLst->RSSetScissorRects(1, &loupeScissor);

                // The draw helpers read cbAddress by reference; the Detach Camera passes are not replayed.
                const D3D12_GPU_VIRTUAL_ADDRESS mainCbAddress = cbAddress;
                cbAddress = loupeCbAddress;
                cbCulledAddress = 0;
                DrawOccluderPass();
                if (!pState->showOcclusionVolumeOnly && !pState->showShadowMesh)
                {
                    const D3D12_GPU_VIRTUAL_ADDRESS splatAddr = pGpuRes->GetGPUVirtualAddress();
                    pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                    pCmdLst->SetGraphicsRootShaderResourceView(1, splatAddr);
                    pCmdLst->SetGraphicsRootShaderResourceView(2, splatAddr);
                    pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress());
                    pCmdLst->SetGraphicsRootShaderResourceView(5, m_pOcclusionVoxelBuffer ? m_pOcclusionVoxelBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(6, m_pSplatGpuBuffer ? m_pSplatGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(7, m_pSHGpuBuffer ? m_pSHGpuBuffer->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(8, m_pSplatPairsA ? m_pSplatPairsA->GetGPUVirtualAddress() : 0);
                    pCmdLst->SetGraphicsRootShaderResourceView(9, m_pSlotInfoBuffer ? m_pSlotInfoBuffer->GetGPUVirtualAddress() : 0);
                    const bool splatMode = SortedGaussians(pState->renderMode);
                    const uint32_t chunkCount = pState->chunkCount;
                    const uint32_t asGroupCount = splatMode ? ((m_splatSlotCount + SPLATS_PER_GROUP - 1) / SPLATS_PER_GROUP) : ((chunkCount + AS_GROUP_SIZE - 1) / AS_GROUP_SIZE);
                    const uint32_t instances = splatMode ? m_splatSlotCount : chunkCount * SPLATS_PER_GROUP;
                    if (!splatMode || (m_pSplatPairsA != nullptr && m_pSlotInfoBuffer != nullptr && m_splatSlotCount > 0))
                        issueSplatDraws(asGroupCount, instances);
                }
                cbAddress = mainCbAddress;
                pCmdLst->RSSetViewports(1, &viewport);
                pCmdLst->RSSetScissorRects(1, &scissor);

                // Into the back buffer's corner (it is a render target here on both the TAA and the direct path)
                const uint32_t dstX = std::min(pState->loupeDestX, m_width - lw);
                const uint32_t dstY = std::min(pState->loupeDestY, m_height - lh);
                const D3D12_BOX srcBox = { 0, 0, 0, lw, lh, 1 };
                D3D12_RESOURCE_BARRIER pre[2] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(m_loupeColor.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST) };
                pCmdLst->ResourceBarrier(2, pre);
                CD3DX12_TEXTURE_COPY_LOCATION dst(pBackBuffer, 0);
                CD3DX12_TEXTURE_COPY_LOCATION src(m_loupeColor.GetResource(), 0);
                pCmdLst->CopyTextureRegion(&dst, dstX, dstY, 0, &src, &srcBox);
                D3D12_RESOURCE_BARRIER post[2] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(m_loupeColor.GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET) };
                pCmdLst->ResourceBarrier(2, post);
            }
        }

        // Scene capture: copy the finished scene (the back buffer is a render target again on both the
        // TAA and the direct path) before the ImGui overlay lands on it.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFootprint = {};
        bool captureIssued = false;
        if (m_captureRequested)
        {
            m_captureRequested = false;
            const D3D12_RESOURCE_DESC bbDesc = pBackBuffer->GetDesc();
            UINT64 totalBytes = 0;
            m_pDevice->GetDevice()->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFootprint, nullptr, nullptr, &totalBytes);
            if (totalBytes > m_captureReadbackBytes)
            {
                if (m_pCaptureReadback) { m_pCaptureReadback->Release(); m_pCaptureReadback = nullptr; }
                ThrowIfFailed(m_pDevice->GetDevice()->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(totalBytes),
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pCaptureReadback)));
                SetName(m_pCaptureReadback, "SplatRenderer::m_pCaptureReadback");
                m_captureReadbackBytes = totalBytes;
            }
            D3D12_RESOURCE_BARRIER toSrc = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            pCmdLst->ResourceBarrier(1, &toSrc);
            CD3DX12_TEXTURE_COPY_LOCATION dst(m_pCaptureReadback, captureFootprint);
            CD3DX12_TEXTURE_COPY_LOCATION src(pBackBuffer, 0);
            pCmdLst->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER toRt = CD3DX12_RESOURCE_BARRIER::Transition(pBackBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            pCmdLst->ResourceBarrier(1, &toRt);
            captureIssued = true;
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

        if (captureIssued)
        {
            // A test-only path: wait for this frame, then repack the rows as RGBA8.
            m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);
            const DXGI_FORMAT fmt = captureFootprint.Footprint.Format;
            const bool bgra = (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS);
            const uint32_t w = captureFootprint.Footprint.Width, h = captureFootprint.Footprint.Height;
            uint8_t* pData = nullptr;
            D3D12_RANGE readRange = { 0, (SIZE_T)m_captureReadbackBytes };
            if (SUCCEEDED(m_pCaptureReadback->Map(0, &readRange, (void**)&pData)) && pData != nullptr)
            {
                m_capturePixels.resize((size_t)w * h * 4);
                for (uint32_t y = 0; y < h; y++)
                {
                    const uint8_t* pRow = pData + captureFootprint.Offset + (size_t)y * captureFootprint.Footprint.RowPitch;
                    uint8_t* pOut = m_capturePixels.data() + (size_t)y * w * 4;
                    for (uint32_t x = 0; x < w; x++)
                    {
                        pOut[x * 4 + 0] = pRow[x * 4 + (bgra ? 2 : 0)];
                        pOut[x * 4 + 1] = pRow[x * 4 + 1];
                        pOut[x * 4 + 2] = pRow[x * 4 + (bgra ? 0 : 2)];
                        pOut[x * 4 + 3] = 255;
                    }
                }
                D3D12_RANGE noWrite = { 0, 0 };
                m_pCaptureReadback->Unmap(0, &noWrite);
                m_captureWidth = w;
                m_captureHeight = h;
                m_captureReady = true;
            }
        }

        auto now = std::chrono::high_resolution_clock::now();
        const float gpuWaitMs = std::chrono::duration<float, std::milli>(tWaitEnd - tWaitStart).count();
        const float commandRecordMs = std::chrono::duration<float, std::milli>(now - tWaitEnd).count();
        m_lastGpuWaitMs = gpuWaitMs;
        m_lastCommandRecordMs = commandRecordMs;
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
        m_smoothGpuWaitMs = (m_smoothGpuWaitMs > 0.0001f) ? (m_smoothGpuWaitMs * 0.85f + gpuWaitMs * 0.15f) : gpuWaitMs;
        m_smoothCommandRecordMs = (m_smoothCommandRecordMs > 0.0001f) ? (m_smoothCommandRecordMs * 0.85f + commandRecordMs * 0.15f) : commandRecordMs;

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
