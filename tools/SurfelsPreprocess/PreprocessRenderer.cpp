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

        CD3DX12_ROOT_PARAMETER rootParams[5];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // b0 - SurfelsCB
        rootParams[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // t0 - g_SurfelBuffer
        rootParams[2].InitAsShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_ALL); // t1 - g_RawSurfelBuffer
        rootParams[3].InitAsShaderResourceView(2, 0, D3D12_SHADER_VISIBILITY_ALL); // t2 - g_ChunkBuffer
        rootParams[4].InitAsShaderResourceView(3, 0, D3D12_SHADER_VISIBILITY_ALL); // t3 - g_SortedChunkIndices

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 5;
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

        Microsoft::WRL::ComPtr<ID3D12Device2> device2;
        m_pDevice->GetDevice()->QueryInterface(IID_PPV_ARGS(&device2));
        device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&m_pPipelineState));

        // Create ID3D12CommandSignature for ExecuteIndirect Mesh Shader Dispatches
        D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;

        D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
        sigDesc.ByteStride = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS); // 12 bytes
        sigDesc.NumArgumentDescs = 1;
        sigDesc.pArgumentDescs = &argDesc;

        m_pDevice->GetDevice()->CreateCommandSignature(&sigDesc, m_pRootSignature, IID_PPV_ARGS(&m_pCommandSignature));
    }

    void PreprocessRenderer::OnDestroy()
    {
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
        if (m_pRootSignature) { m_pRootSignature->Release(); m_pRootSignature = nullptr; }

        m_depthBuffer.OnDestroy();
        m_imGui.OnDestroy();
        m_commandListRing.OnDestroy();
        m_constantBufferRing.OnDestroy();
        m_uploadHeap.OnDestroy();
        m_resourceViewHeaps.OnDestroy();
    }

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
        m_lastGpuRadixSort = pState->gpuRadixSort;
        m_lastUseChunkedPipeline = pState->useChunkedPipeline;

        bool modelChanged = (m_lastSurfelsPtr != activePtr) || (m_lastSurfelCount != surfelCount) || (m_lastRenderMode != renderMode) || pipelineModeChanged;
        float camMoved = std::abs(eyePos.x - m_lastSortEye.x) + std::abs(eyePos.y - m_lastSortEye.y) + std::abs(eyePos.z - m_lastSortEye.z);
        float forwardMoved = std::abs(forward.x - m_lastSortForward.x) + std::abs(forward.y - m_lastSortForward.y) + std::abs(forward.z - m_lastSortForward.z);
        bool needsSort = modelChanged || (camMoved > 0.05f) || (forwardMoved > 0.02f);

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
                if (pState->useChunkedPipeline || pState->gpuRadixSort)
                {
                    m_metrics.isGPUSortActive = true;
                    m_metrics.cpuSortTimeMs = 0.0f;

                    if (modelChanged)
                    {
                        SurfelVertex* pDst = reinterpret_cast<SurfelVertex*>(m_pRawSurfelBufferMapped);
                        #pragma omp parallel for
                        for (int i = 0; i < (int)surfelCount; i++)
                        {
                            pDst[i] = pRawSurfels[i];
                        }
                        for (uint32_t i = surfelCount; i < numElements; i++)
                        {
                            pDst[i].position = XMFLOAT3(0.0f, 0.0f, 0.0f);
                            pDst[i].normal = XMFLOAT3(0, 1, 0);
                            pDst[i].color = XMFLOAT3(0, 0, 0);
                            pDst[i].radius = -1.0f;
                        }
                        m_needUploadToGpu = true;
                        m_gpuSortNeedsRun = true;
                    }

                    if (needsSort)
                    {
                        m_gpuSortNeedsRun = true;
                        m_lastSortEye = eyePos;
                        m_lastSortForward = forward;
                    }
                }
                else if (needsSort)
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

                    // 1. Parallel Projected Distance Calculation (OpenMP multi-threaded)
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
                    SurfelVertex* pDst = reinterpret_cast<SurfelVertex*>(m_pRawSurfelBufferMapped);
                    #pragma omp parallel for
                    for (int i = 0; i < (int)surfelCount; i++)
                    {
                        pDst[i] = pRawSurfels[m_sortIndicesA[i]];
                    }

                    m_needUploadToGpu = true;

                    auto sortEnd = std::chrono::high_resolution_clock::now();
                    m_metrics.cpuSortTimeMs = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();
                    m_metrics.gpuSortTimeMs = 0.0f;
                }
                else
                {
                    m_metrics.wasSortedThisFrame = false;
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
                if (pState->useChunkedPipeline || pState->gpuRadixSort)
                {
                    m_metrics.isGPUSortActive = true;
                    m_metrics.cpuSortTimeMs = 0.0f;

                    if (modelChanged)
                    {
                        PackedSurfelGPU* pDst = reinterpret_cast<PackedSurfelGPU*>(m_pSurfelBufferMapped);
                        #pragma omp parallel for
                        for (int i = 0; i < (int)surfelCount; i++)
                        {
                            pDst[i] = pSurfels[i];
                        }
                        for (uint32_t i = surfelCount; i < numElements; i++)
                        {
                            pDst[i].packedPosRadius = 0xFFFFFFFF;
                            pDst[i].packedNormal = 0xFFFF;
                            pDst[i].packedColor = 0xFFFF;
                        }
                        m_needUploadToGpu = true;
                        m_gpuSortNeedsRun = true;
                    }

                    if (needsSort)
                    {
                        m_gpuSortNeedsRun = true;
                        m_lastSortEye = eyePos;
                        m_lastSortForward = forward;
                    }
                }
                else if (needsSort)
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

                    for (uint32_t i = 0; i < surfelCount; i++)
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

                    // 5. Gather sorted packed surfels directly into mapped GPU upload buffer
                    PackedSurfelGPU* pDst = reinterpret_cast<PackedSurfelGPU*>(m_pSurfelBufferMapped);
                    #pragma omp parallel for
                    for (int i = 0; i < (int)surfelCount; i++)
                    {
                        pDst[i] = pSurfels[m_sortIndicesA[i]];
                    }

                    m_needUploadToGpu = true;

                    auto sortEnd = std::chrono::high_resolution_clock::now();
                    m_metrics.cpuSortTimeMs = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();
                    m_metrics.gpuSortTimeMs = 0.0f;
                }
                else
                {
                    m_metrics.wasSortedThisFrame = false;
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
            if (numChunkElements < 1024) numChunkElements = 1024;

            uint32_t chunkBytes = numChunkElements * sizeof(MeshletChunkGPU);
            if (chunkBytes > m_chunkBufferCapacityBytes)
            {
                m_pDevice->GPUFlush(D3D12_COMMAND_LIST_TYPE_DIRECT);

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

                m_pChunkUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pChunkUploadBufferMapped));
                m_chunkBufferCapacityBytes = chunkBytes;
                m_sortedChunkIndicesCapacityBytes = idxBytes;
                modelChanged = true;
            }

            if (modelChanged && m_pChunkUploadBufferMapped != nullptr)
            {
                MeshletChunkGPU* pDstChunks = reinterpret_cast<MeshletChunkGPU*>(m_pChunkUploadBufferMapped);
                for (uint32_t i = 0; i < chunkCount; i++)
                {
                    pDstChunks[i] = pState->pChunks[i];
                }
                for (uint32_t i = chunkCount; i < numChunkElements; i++)
                {
                    pDstChunks[i] = {};
                }
            }

            // If CPU sort is active in Chunked Pipeline, compute chunk sorting on CPU and prepare upload
            if (pState->useChunkedPipeline && !pState->gpuRadixSort && needsSort && m_pSortedChunkIndicesUploadBufferMapped != nullptr)
            {
                auto sortStart = std::chrono::high_resolution_clock::now();
                std::vector<std::pair<float, uint32_t>> chunkDists(chunkCount);
                #pragma omp parallel for
                for (int i = 0; i < (int)chunkCount; i++)
                {
                    const auto& c = pState->pChunks[i];
                    float d = (c.center.x - eyePos.x) * forward.x + (c.center.y - eyePos.y) * forward.y + (c.center.z - eyePos.z) * forward.z;
                    chunkDists[i] = { d, (uint32_t)i };
                }

                // Descending sort (far to near)
                std::sort(chunkDists.begin(), chunkDists.end(), [](const auto& a, const auto& b) {
                    return a.first > b.first;
                });

                for (uint32_t i = 0; i < chunkCount; i++)
                {
                    m_pSortedChunkIndicesUploadBufferMapped[i] = chunkDists[i].second;
                }
                for (uint32_t i = chunkCount; i < numChunkElements; i++)
                {
                    m_pSortedChunkIndicesUploadBufferMapped[i] = 0;
                }

                m_needUploadChunkIndicesToGpu = true;
                auto sortEnd = std::chrono::high_resolution_clock::now();
                m_metrics.cpuSortTimeMs = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();
                m_metrics.wasSortedThisFrame = true;
            }
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

        auto frameStart = std::chrono::high_resolution_clock::now();

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
        UpdateSurfelBuffers(pState, eyePos, forwardNorm);
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
        pCB->totalChunks = pState->chunkCount;
        pCB->aabbMin = pState->aabbMin;
        pCB->useChunkedPipeline = (pState->useChunkedPipeline && pState->chunkCount > 0 && m_pChunkGpuBuffer != nullptr) ? 1 : 0;
        pCB->aabbExtents = pState->aabbExtents;
        pCB->pad2 = 0.0f;

        ID3D12Resource* pGpuRes = (pState->renderMode == 2) ? m_pRawSurfelGpuBuffer : m_pSurfelGpuBuffer;
        ID3D12Resource* pGpuOutRes = (pState->renderMode == 2) ? m_pRawSurfelGpuOutBuffer : m_pSurfelGpuOutBuffer;
        ID3D12Resource* pUploadRes = (pState->renderMode == 2) ? m_pRawSurfelBuffer : m_pSurfelBuffer;
        auto dispatchStart = std::chrono::high_resolution_clock::now();

        if (surfelCount > 0 && pGpuRes != nullptr && pUploadRes != nullptr)
        {
            if (m_needUploadToGpu)
            {
                // Copy canonical unsorted data to GPU input buffer only when modified/loaded!
                pCmdLst->CopyResource(pGpuRes, pUploadRes);
                if (m_pChunkGpuBuffer != nullptr && m_pChunkUploadBuffer != nullptr)
                {
                    pCmdLst->CopyResource(m_pChunkGpuBuffer, m_pChunkUploadBuffer);
                    D3D12_RESOURCE_BARRIER chunkToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pChunkGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    pCmdLst->ResourceBarrier(1, &chunkToSrv);
                }
                m_needUploadToGpu = false;
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

                        D3D12_RESOURCE_BARRIER preBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                            m_pSortedChunkIndicesGpuBuffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        pCmdLst->ResourceBarrier(1, &preBarrier);

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

                        auto gpuSortEnd = std::chrono::high_resolution_clock::now();
                        m_metrics.gpuSortTimeMs = std::chrono::duration<float, std::milli>(gpuSortEnd - gpuSortStart).count();
                        m_metrics.isGPUSortActive = true;
                        m_metrics.wasSortedThisFrame = true;
                        m_gpuSortNeedsRun = false;
                    }
                }
                else if (m_needUploadChunkIndicesToGpu && m_pSortedChunkIndicesGpuBuffer && m_pSortedChunkIndicesUploadBuffer)
                {
                    pCmdLst->CopyResource(m_pSortedChunkIndicesGpuBuffer, m_pSortedChunkIndicesUploadBuffer);
                    D3D12_RESOURCE_BARRIER idxToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pSortedChunkIndicesGpuBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    pCmdLst->ResourceBarrier(1, &idxToSrv);
                    m_needUploadChunkIndicesToGpu = false;
                }

                // Transition surfel buffer to ALL_SHADER_RESOURCE for rendering
                D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    pGpuRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                pCmdLst->ResourceBarrier(1, &toSrv);

                D3D12_GPU_VIRTUAL_ADDRESS surfelAddr = pGpuRes->GetGPUVirtualAddress();
                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(m_pPipelineState);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, surfelAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(2, surfelAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer->GetGPUVirtualAddress());
                pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress());

                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
                pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
                uint32_t asGroupCount = (chunkCount + AS_GROUP_SIZE - 1) / AS_GROUP_SIZE;
                cmdList6->DispatchMesh(asGroupCount, 1, 1);

                // Transition back to COPY_DEST for next frame
                D3D12_RESOURCE_BARRIER barriersToDest[2] = {};
                barriersToDest[0] = CD3DX12_RESOURCE_BARRIER::Transition(
                    pGpuRes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                uint32_t barrierCount = 1;
                if (!pState->gpuRadixSort)
                {
                    barriersToDest[1] = CD3DX12_RESOURCE_BARRIER::Transition(
                        m_pSortedChunkIndicesGpuBuffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                    barrierCount = 2;
                }
                pCmdLst->ResourceBarrier(barrierCount, barriersToDest);
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
                    postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(pGpuRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
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

                // Draw Mesh Shader using Sorted Output Buffer
                D3D12_GPU_VIRTUAL_ADDRESS outAddr = pGpuOutRes->GetGPUVirtualAddress();
                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(m_pPipelineState);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, outAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(2, outAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);

                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
                pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
                uint32_t groupCount = (surfelCount + SURFELS_PER_GROUP - 1) / SURFELS_PER_GROUP;
                cmdList6->DispatchMesh(groupCount, 1, 1);
            }
            else
            {
                // CPU sorted: Transition COPY_DEST -> ALL_SHADER_RESOURCE
                D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    pGpuRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                pCmdLst->ResourceBarrier(1, &toSrv);

                D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = pGpuRes->GetGPUVirtualAddress();
                pCmdLst->SetGraphicsRootSignature(m_pRootSignature);
                pCmdLst->SetPipelineState(m_pPipelineState);
                pCmdLst->SetGraphicsRootConstantBufferView(0, cbAddress);
                pCmdLst->SetGraphicsRootShaderResourceView(1, gpuAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(2, gpuAddr);
                pCmdLst->SetGraphicsRootShaderResourceView(3, m_pChunkGpuBuffer ? m_pChunkGpuBuffer->GetGPUVirtualAddress() : 0);
                pCmdLst->SetGraphicsRootShaderResourceView(4, m_pSortedChunkIndicesGpuBuffer ? m_pSortedChunkIndicesGpuBuffer->GetGPUVirtualAddress() : 0);

                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
                pCmdLst->QueryInterface(IID_PPV_ARGS(&cmdList6));
                uint32_t groupCount = (surfelCount + SURFELS_PER_GROUP - 1) / SURFELS_PER_GROUP;
                cmdList6->DispatchMesh(groupCount, 1, 1);

                // Transition back to COPY_DEST for next frame
                D3D12_RESOURCE_BARRIER toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
                    pGpuRes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                pCmdLst->ResourceBarrier(1, &toCopyDest);
            }
        }
        auto dispatchEnd = std::chrono::high_resolution_clock::now();
        m_metrics.gpuDispatchTimeMs = std::chrono::duration<float, std::milli>(dispatchEnd - dispatchStart).count();

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
    }
}
