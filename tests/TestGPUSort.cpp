// tests/TestGPUSort.cpp - Standalone GPU Bitonic / Radix Depth Sort Verification Tool
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct RawSurfel
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT3 color;
    float    radius;
};

struct PackedSurfel
{
    uint32_t packedPosRadius;
    uint32_t packedNormalColor;
};

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

int main(int argc, char** argv)
{
    uint32_t numPoints = 100000;
    uint32_t testMode = 1; // 1 = Packed 8-Byte, 2 = Raw Float32
    if (argc > 1)
    {
        numPoints = (uint32_t)atoi(argv[1]);
    }
    if (argc > 2)
    {
        testMode = (uint32_t)atoi(argv[2]);
    }

    std::cout << "========================================================\n";
    std::cout << "   STANDALONE GPU DEPTH SORT ACCURACY VERIFICATION TOOL \n";
    std::cout << "========================================================\n";
    std::cout << "Testing with " << numPoints << " points (Mode " << testMode << ": " << (testMode == 1 ? "Packed 8-Byte" : "Raw Float32") << ")...\n\n";

    // 1. Initialize DX12 Device & Command Queue
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter1> fallbackAdapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!fallbackAdapter) fallbackAdapter = adapter;
        if (wcsstr(desc.Description, L"NVIDIA") || wcsstr(desc.Description, L"RTX") || wcsstr(desc.Description, L"AMD") || wcsstr(desc.Description, L"Radeon"))
        {
            fallbackAdapter = adapter;
            break;
        }
    }
    adapter = fallbackAdapter;
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    std::wcout << L"Selected GPU: " << desc.Description << L"\n";

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
    {
        std::cerr << "Failed to create D3D12 device!\n";
        return -1;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> commandQueue;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));

    ComPtr<ID3D12GraphicsCommandList> commandList;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));

    // 2. Generate Random Dataset
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> distPos(-40.0f, 40.0f);

    XMFLOAT3 aabbMin(-50.0f, -50.0f, -50.0f);
    XMFLOAT3 aabbMax(50.0f, 50.0f, 50.0f);
    XMFLOAT3 aabbExtents(100.0f, 100.0f, 100.0f);

    std::vector<RawSurfel> rawData(numPoints);
    std::vector<PackedSurfel> packedData(numPoints);

    for (uint32_t i = 0; i < numPoints; i++)
    {
        rawData[i].position = XMFLOAT3(distPos(rng), distPos(rng), distPos(rng));
        rawData[i].normal = XMFLOAT3(0, 1, 0);
        rawData[i].color = XMFLOAT3(1, 1, 1);
        rawData[i].radius = 0.05f;

        float nx = (rawData[i].position.x - aabbMin.x) / aabbExtents.x;
        float ny = (rawData[i].position.y - aabbMin.y) / aabbExtents.y;
        float nz = (rawData[i].position.z - aabbMin.z) / aabbExtents.z;
        uint32_t qx = (uint32_t)std::max(0.0f, std::min(1023.0f, std::round(nx * 1023.0f)));
        uint32_t qy = (uint32_t)std::max(0.0f, std::min(1023.0f, std::round(ny * 1023.0f)));
        uint32_t qz = (uint32_t)std::max(0.0f, std::min(1023.0f, std::round(nz * 1023.0f)));

        packedData[i].packedPosRadius = qx | (qy << 10) | (qz << 20);
        packedData[i].packedNormalColor = 0;

        // Ensure rawData exactly matches quantized coordinates when testing Mode 1
        if (testMode == 1)
        {
            rawData[i].position.x = aabbMin.x + (qx / 1023.0f) * aabbExtents.x;
            rawData[i].position.y = aabbMin.y + (qy / 1023.0f) * aabbExtents.y;
            rawData[i].position.z = aabbMin.z + (qz / 1023.0f) * aabbExtents.z;
        }
    }

    XMFLOAT3 camPos = XMFLOAT3(15.0f, 25.0f, -35.0f);
    XMFLOAT3 camTarget = XMFLOAT3(0.0f, 0.0f, 0.0f);
    XMVECTOR eye = XMLoadFloat3(&camPos);
    XMVECTOR at = XMLoadFloat3(&camTarget);
    XMVECTOR fwdVec = XMVector3Normalize(XMVectorSubtract(at, eye));
    XMFLOAT3 camForward;
    XMStoreFloat3(&camForward, fwdVec);

    std::cout << "Camera Pos:     (" << camPos.x << ", " << camPos.y << ", " << camPos.z << ")\n";
    std::cout << "Camera Forward: (" << camForward.x << ", " << camForward.y << ", " << camForward.z << ")\n\n";

    // 3. Benchmark CPU Reference Sorts
    // 3A. CPU std::sort (single-threaded comparator)
    std::vector<RawSurfel> expectedCPU = rawData;
    auto cpuStdStart = std::chrono::high_resolution_clock::now();
    std::sort(expectedCPU.begin(), expectedCPU.end(), [&](const RawSurfel& a, const RawSurfel& b) {
        float dA = (a.position.x - camPos.x) * camForward.x + (a.position.y - camPos.y) * camForward.y + (a.position.z - camPos.z) * camForward.z;
        float dB = (b.position.x - camPos.x) * camForward.x + (b.position.y - camPos.y) * camForward.y + (b.position.z - camPos.z) * camForward.z;
        return dA > dB; // Descending (back-to-front)
    });
    auto cpuStdEnd = std::chrono::high_resolution_clock::now();
    double cpuStdTimeMs = std::chrono::duration<double, std::milli>(cpuStdEnd - cpuStdStart).count();

    // 3B. CPU Parallel 16-Bit Radix Sort (OpenMP multi-threaded)
    std::vector<uint32_t> sortIndicesA(numPoints);
    std::vector<uint32_t> sortIndicesB(numPoints);
    std::vector<uint32_t> sortKeys(numPoints);
    std::vector<float>    sortDists(numPoints);
    for (uint32_t i = 0; i < numPoints; i++) sortIndicesA[i] = i;

    auto cpuRadixStart = std::chrono::high_resolution_clock::now();
    float minD = 1e9f, maxD = -1e9f;
    #pragma omp parallel for reduction(min:minD) reduction(max:maxD)
    for (int i = 0; i < (int)numPoints; i++)
    {
        const auto& pos = rawData[i].position;
        float d = (pos.x - camPos.x) * camForward.x + (pos.y - camPos.y) * camForward.y + (pos.z - camPos.z) * camForward.z;
        sortDists[i] = d;
        if (d < minD) minD = d;
        if (d > maxD) maxD = d;
    }
    float range = std::max(0.001f, maxD - minD);
    float scale = 65535.0f / range;

    #pragma omp parallel for
    for (int i = 0; i < (int)numPoints; i++)
    {
        uint32_t k = (uint32_t)((sortDists[i] - minD) * scale);
        if (k > 65535) k = 65535;
        sortKeys[i] = 65535 - k;
        sortIndicesA[i] = i;
    }

    uint32_t count0[256] = {};
    for (uint32_t i = 0; i < numPoints; i++) count0[sortKeys[i] & 0xFF]++;
    uint32_t offset0[256] = {};
    for (uint32_t i = 1; i < 256; i++) offset0[i] = offset0[i - 1] + count0[i - 1];
    for (uint32_t i = 0; i < numPoints; i++)
    {
        uint32_t idx = sortIndicesA[i];
        uint8_t byte0 = (uint8_t)(sortKeys[idx] & 0xFF);
        sortIndicesB[offset0[byte0]++] = idx;
    }

    uint32_t count1[256] = {};
    for (uint32_t i = 0; i < numPoints; i++) count1[(sortKeys[sortIndicesB[i]] >> 8) & 0xFF]++;
    uint32_t offset1[256] = {};
    for (uint32_t i = 1; i < 256; i++) offset1[i] = offset1[i - 1] + count1[i - 1];
    for (uint32_t i = 0; i < numPoints; i++)
    {
        uint32_t idx = sortIndicesB[i];
        uint8_t byte1 = (uint8_t)((sortKeys[idx] >> 8) & 0xFF);
        sortIndicesA[offset1[byte1]++] = idx;
    }
    auto cpuRadixEnd = std::chrono::high_resolution_clock::now();
    double cpuRadixTimeMs = std::chrono::duration<double, std::milli>(cpuRadixEnd - cpuRadixStart).count();

    // 4. Create GPU Buffers (Padded power of 2)
    uint32_t numElements = 1;
    while (numElements < numPoints) numElements <<= 1;
    uint32_t stride = (testMode == 1) ? sizeof(PackedSurfel) : sizeof(RawSurfel);
    uint64_t surfelBufferSize = (uint64_t)numElements * stride;
    uint64_t pairBufferSize = (uint64_t)numElements * sizeof(uint32_t) * 2; // SortPair = 8 bytes

    std::cout << "Allocating GPU Buffers for " << numElements << " elements (" << (surfelBufferSize / (1024 * 1024)) << " MB surfels + " << (pairBufferSize / (1024 * 1024)) << " MB keys)...\n";

    D3D12_HEAP_PROPERTIES heapDefault = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES heapUpload = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES heapReadback = { D3D12_HEAP_TYPE_READBACK };
    
    D3D12_RESOURCE_DESC uavBufDesc = {};
    uavBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavBufDesc.Width = surfelBufferSize;
    uavBufDesc.Height = 1;
    uavBufDesc.DepthOrArraySize = 1;
    uavBufDesc.MipLevels = 1;
    uavBufDesc.SampleDesc.Count = 1;
    uavBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uavBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC pairBufDesc = uavBufDesc;
    pairBufDesc.Width = pairBufferSize;

    D3D12_RESOURCE_DESC transferBufDesc = uavBufDesc;
    transferBufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> gpuInputBuffer;
    device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &uavBufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpuInputBuffer));

    ComPtr<ID3D12Resource> gpuPairBuffer;
    device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &pairBufDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&gpuPairBuffer));

    ComPtr<ID3D12Resource> gpuOutputBuffer;
    device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &uavBufDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&gpuOutputBuffer));

    ComPtr<ID3D12Resource> uploadBuffer;
    device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &transferBufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    ComPtr<ID3D12Resource> readbackBuffer;
    device->CreateCommittedResource(&heapReadback, D3D12_HEAP_FLAG_NONE, &transferBufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffer));

    // Upload data
    void* pMapped = nullptr;
    uploadBuffer->Map(0, nullptr, &pMapped);
    if (testMode == 1)
    {
        memcpy(pMapped, packedData.data(), numPoints * sizeof(PackedSurfel));
        PackedSurfel* pExt = (PackedSurfel*)pMapped;
        for (uint32_t i = numPoints; i < numElements; i++)
        {
            pExt[i].packedPosRadius = 0xFFFFFFFF;
            pExt[i].packedNormalColor = 0xFFFFFFFF;
        }
    }
    else
    {
        memcpy(pMapped, rawData.data(), numPoints * sizeof(RawSurfel));
        RawSurfel* pExt = (RawSurfel*)pMapped;
        for (uint32_t i = numPoints; i < numElements; i++)
        {
            pExt[i].position = XMFLOAT3(0.0f, 0.0f, 0.0f);
            pExt[i].normal = XMFLOAT3(0, 1, 0);
            pExt[i].color = XMFLOAT3(0, 0, 0);
            pExt[i].radius = -1.0f;
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    commandList->CopyResource(gpuInputBuffer.Get(), uploadBuffer.Get());

    D3D12_RESOURCE_BARRIER toSrv = {};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = gpuInputBuffer.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toSrv);

    // 5. Compile Shaders & Create Root Signature / PSOs
    ComPtr<ID3DBlob> blobProject, blobLocalSort, blobGlobalSort, blobLocalMerge, blobGather, errBlob;
    HRESULT hr = D3DCompileFromFile(L"C:/github/surfels/src/DX12/Shaders/GPURadixSortCS.hlsl", nullptr, nullptr, "ProjectKeysCS", "cs_5_1", 0, 0, &blobProject, &errBlob);
    if (FAILED(hr))
    {
        std::cerr << "Shader compile error (ProjectKeysCS)!\n";
        if (errBlob) std::cerr << (char*)errBlob->GetBufferPointer() << "\n";
        return -1;
    }

    hr = D3DCompileFromFile(L"C:/github/surfels/src/DX12/Shaders/GPURadixSortCS.hlsl", nullptr, nullptr, "BitonicLocalSortCS", "cs_5_1", 0, 0, &blobLocalSort, &errBlob);
    if (FAILED(hr))
    {
        std::cerr << "Shader compile error (BitonicLocalSortCS)!\n";
        if (errBlob) std::cerr << (char*)errBlob->GetBufferPointer() << "\n";
        return -1;
    }

    hr = D3DCompileFromFile(L"C:/github/surfels/src/DX12/Shaders/GPURadixSortCS.hlsl", nullptr, nullptr, "BitonicGlobalSortCS", "cs_5_1", 0, 0, &blobGlobalSort, &errBlob);
    if (FAILED(hr))
    {
        std::cerr << "Shader compile error (BitonicGlobalSortCS)!\n";
        if (errBlob) std::cerr << (char*)errBlob->GetBufferPointer() << "\n";
        return -1;
    }

    hr = D3DCompileFromFile(L"C:/github/surfels/src/DX12/Shaders/GPURadixSortCS.hlsl", nullptr, nullptr, "BitonicLocalMergeCS", "cs_5_1", 0, 0, &blobLocalMerge, &errBlob);
    if (FAILED(hr))
    {
        std::cerr << "Shader compile error (BitonicLocalMergeCS)!\n";
        if (errBlob) std::cerr << (char*)errBlob->GetBufferPointer() << "\n";
        return -1;
    }

    hr = D3DCompileFromFile(L"C:/github/surfels/src/DX12/Shaders/GPURadixSortCS.hlsl", nullptr, nullptr, "GatherSurfelsCS", "cs_5_1", 0, 0, &blobGather, &errBlob);
    if (FAILED(hr))
    {
        std::cerr << "Shader compile error (GatherSurfelsCS)!\n";
        if (errBlob) std::cerr << (char*)errBlob->GetBufferPointer() << "\n";
        return -1;
    }

    D3D12_ROOT_PARAMETER rootParams[6] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0; // b0
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[1].Descriptor.ShaderRegister = 0; // t0 (InPackedSurfels)
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[2].Descriptor.ShaderRegister = 1; // t1 (InRawSurfels)
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[3].Descriptor.ShaderRegister = 0; // u0 (SortPairs)
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[4].Descriptor.ShaderRegister = 1; // u1 (OutPackedSurfels)
    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[5].Descriptor.ShaderRegister = 2; // u2 (OutRawSurfels)

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 6;
    rsDesc.pParameters = rootParams;

    ComPtr<ID3DBlob> rsBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr);
    ComPtr<ID3D12RootSignature> rootSignature;
    device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();

    ComPtr<ID3D12PipelineState> psoProject;
    psoDesc.CS.pShaderBytecode = blobProject->GetBufferPointer();
    psoDesc.CS.BytecodeLength = blobProject->GetBufferSize();
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&psoProject));

    ComPtr<ID3D12PipelineState> psoLocalSort;
    psoDesc.CS.pShaderBytecode = blobLocalSort->GetBufferPointer();
    psoDesc.CS.BytecodeLength = blobLocalSort->GetBufferSize();
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&psoLocalSort));

    ComPtr<ID3D12PipelineState> psoGlobalSort;
    psoDesc.CS.pShaderBytecode = blobGlobalSort->GetBufferPointer();
    psoDesc.CS.BytecodeLength = blobGlobalSort->GetBufferSize();
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&psoGlobalSort));

    ComPtr<ID3D12PipelineState> psoLocalMerge;
    psoDesc.CS.pShaderBytecode = blobLocalMerge->GetBufferPointer();
    psoDesc.CS.BytecodeLength = blobLocalMerge->GetBufferSize();
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&psoLocalMerge));

    ComPtr<ID3D12PipelineState> psoGather;
    psoDesc.CS.pShaderBytecode = blobGather->GetBufferPointer();
    psoDesc.CS.BytecodeLength = blobGather->GetBufferSize();
    device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&psoGather));

    // Create GPU Timestamp Query Heap
    D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryHeapDesc.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap;
    device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&queryHeap));

    D3D12_RESOURCE_DESC queryBufDesc = {};
    queryBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    queryBufDesc.Width = sizeof(uint64_t) * 2;
    queryBufDesc.Height = 1;
    queryBufDesc.DepthOrArraySize = 1;
    queryBufDesc.MipLevels = 1;
    queryBufDesc.SampleDesc.Count = 1;
    queryBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    queryBufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> queryResultBuffer;
    device->CreateCommittedResource(&heapReadback, D3D12_HEAP_FLAG_NONE, &queryBufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&queryResultBuffer));

    // Calculate LDS Dispatches
    uint32_t totalPasses = 1 + 1 + 1; // ProjectKeys (1) + LocalSort (1) + Gather (1)
    for (uint32_t l = 2048; l <= numElements; l <<= 1)
    {
        for (uint32_t lm = l >> 1; lm >= 1024; lm >>= 1)
            totalPasses++; // Global passes
        totalPasses++; // 1 LocalMerge pass
    }

    D3D12_RESOURCE_DESC cbDesc = transferBufDesc;
    cbDesc.Width = (uint64_t)(totalPasses + 10) * 256;
    ComPtr<ID3D12Resource> cbBuffer;
    device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbBuffer));

    uint8_t* pCBData = nullptr;
    cbBuffer->Map(0, nullptr, (void**)&pCBData);

    // 6. Record GPU Sorting Passes & Measure CPU Record Time
    auto cpuRecordStart = std::chrono::high_resolution_clock::now();

    commandList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    commandList->SetComputeRootSignature(rootSignature.Get());
    commandList->SetComputeRootShaderResourceView(1, gpuInputBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(2, gpuInputBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(3, gpuPairBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(4, gpuOutputBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(5, gpuOutputBuffer->GetGPUVirtualAddress());

    uint32_t passIdx = 0;
    uint32_t localGroups = (numElements + 1023) / 1024;
    uint32_t globalGroups = (numElements + 255) / 256;

    BitonicCB baseCB = {};
    baseCB.camPos = camPos;
    baseCB.camForward = camForward;
    baseCB.totalSurfels = numPoints;
    baseCB.numElements = numElements;
    baseCB.renderMode = testMode;
    baseCB.aabbMin = aabbMin;
    baseCB.aabbExtents = aabbExtents;

    D3D12_RESOURCE_BARRIER uavPairBarrier = {};
    uavPairBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavPairBarrier.UAV.pResource = gpuPairBuffer.Get();

    D3D12_RESOURCE_BARRIER uavOutBarrier = {};
    uavOutBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavOutBarrier.UAV.pResource = gpuOutputBuffer.Get();

    // Stage 1: Project depth keys ONCE into key-index pairs
    {
        BitonicCB cb = baseCB;
        memcpy(pCBData + passIdx * 256, &cb, sizeof(BitonicCB));

        commandList->SetPipelineState(psoProject.Get());
        commandList->SetComputeRootConstantBufferView(0, cbBuffer->GetGPUVirtualAddress() + passIdx * 256);
        commandList->Dispatch(globalGroups, 1, 1);
        commandList->ResourceBarrier(1, &uavPairBarrier);
        passIdx++;
    }

    // Stage 2: Local Block Sort in LDS (all stages from level = 2 up to 1024 in 1 single dispatch!)
    {
        BitonicCB cb = baseCB;
        cb.level = std::min(numElements, 1024u);
        cb.levelMask = cb.level >> 1;
        memcpy(pCBData + passIdx * 256, &cb, sizeof(BitonicCB));

        commandList->SetPipelineState(psoLocalSort.Get());
        commandList->SetComputeRootConstantBufferView(0, cbBuffer->GetGPUVirtualAddress() + passIdx * 256);
        commandList->Dispatch(localGroups, 1, 1);
        commandList->ResourceBarrier(1, &uavPairBarrier);
        passIdx++;
    }

    // Stage 3: Outer Levels (level = 2048 up to numElements)
    for (uint32_t level = 2048; level <= numElements; level <<= 1)
    {
        // Global passes for levelMask >= 1024
        for (uint32_t levelMask = level >> 1; levelMask >= 1024; levelMask >>= 1)
        {
            BitonicCB cb = baseCB;
            cb.level = level;
            cb.levelMask = levelMask;
            memcpy(pCBData + passIdx * 256, &cb, sizeof(BitonicCB));

            commandList->SetPipelineState(psoGlobalSort.Get());
            commandList->SetComputeRootConstantBufferView(0, cbBuffer->GetGPUVirtualAddress() + passIdx * 256);
            commandList->Dispatch(globalGroups, 1, 1);
            commandList->ResourceBarrier(1, &uavPairBarrier);
            passIdx++;
        }

        // Local LDS Merge pass for all remaining levels (512 down to 1 in 1 single dispatch!)
        {
            BitonicCB cb = baseCB;
            cb.level = level;
            cb.levelMask = 512;
            memcpy(pCBData + passIdx * 256, &cb, sizeof(BitonicCB));

            commandList->SetPipelineState(psoLocalMerge.Get());
            commandList->SetComputeRootConstantBufferView(0, cbBuffer->GetGPUVirtualAddress() + passIdx * 256);
            commandList->Dispatch(localGroups, 1, 1);
            commandList->ResourceBarrier(1, &uavPairBarrier);
            passIdx++;
        }
    }

    // Stage 4: 1-Pass Gather into output sorted buffer
    {
        BitonicCB cb = baseCB;
        memcpy(pCBData + passIdx * 256, &cb, sizeof(BitonicCB));

        commandList->SetPipelineState(psoGather.Get());
        commandList->SetComputeRootConstantBufferView(0, cbBuffer->GetGPUVirtualAddress() + passIdx * 256);
        commandList->Dispatch(globalGroups, 1, 1);
        commandList->ResourceBarrier(1, &uavOutBarrier);
        passIdx++;
    }

    commandList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    commandList->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryResultBuffer.Get(), 0);

    // Copy to readback buffer for accuracy verification
    D3D12_RESOURCE_BARRIER trans = {};
    trans.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    trans.Transition.pResource = gpuOutputBuffer.Get();
    trans.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    trans.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &trans);

    commandList->CopyResource(readbackBuffer.Get(), gpuOutputBuffer.Get());
    commandList->Close();

    auto cpuRecordEnd = std::chrono::high_resolution_clock::now();
    double cpuRecordTimeMs = std::chrono::duration<double, std::milli>(cpuRecordEnd - cpuRecordStart).count();

    // 7. Execute & Time GPU Execution
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    auto gpuWallStart = std::chrono::high_resolution_clock::now();
    ID3D12CommandList* pCmds[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(1, pCmds);
    commandQueue->Signal(fence.Get(), 1);

    while (fence->GetCompletedValue() < 1)
    {
        Sleep(0);
    }
    auto gpuWallEnd = std::chrono::high_resolution_clock::now();
    double gpuWallTimeMs = std::chrono::duration<double, std::milli>(gpuWallEnd - gpuWallStart).count();

    // Read GPU hardware timestamp
    uint64_t gpuFreq = 0;
    commandQueue->GetTimestampFrequency(&gpuFreq);
    uint64_t* pTimestamps = nullptr;
    queryResultBuffer->Map(0, nullptr, (void**)&pTimestamps);
    double gpuHardwareTimeMs = (double)(pTimestamps[1] - pTimestamps[0]) * 1000.0 / (double)gpuFreq;
    queryResultBuffer->Unmap(0, nullptr);

    // 8. Verify GPU Results Against Expected Reference
    void* pGpuResultsMapped = nullptr;
    readbackBuffer->Map(0, nullptr, &pGpuResultsMapped);

    std::vector<float> gpuSortedDepths(numPoints);
    if (testMode == 1)
    {
        PackedSurfel* pOut = (PackedSurfel*)pGpuResultsMapped;
        for (uint32_t i = 0; i < numPoints; i++)
        {
            uint32_t qx = pOut[i].packedPosRadius & 0x3FF;
            uint32_t qy = (pOut[i].packedPosRadius >> 10) & 0x3FF;
            uint32_t qz = (pOut[i].packedPosRadius >> 20) & 0x3FF;
            float px = aabbMin.x + (qx / 1023.0f) * aabbExtents.x;
            float py = aabbMin.y + (qy / 1023.0f) * aabbExtents.y;
            float pz = aabbMin.z + (qz / 1023.0f) * aabbExtents.z;
            gpuSortedDepths[i] = (px - camPos.x) * camForward.x + (py - camPos.y) * camForward.y + (pz - camPos.z) * camForward.z;
        }
    }
    else
    {
        RawSurfel* pOut = (RawSurfel*)pGpuResultsMapped;
        for (uint32_t i = 0; i < numPoints; i++)
        {
            gpuSortedDepths[i] = (pOut[i].position.x - camPos.x) * camForward.x + (pOut[i].position.y - camPos.y) * camForward.y + (pOut[i].position.z - camPos.z) * camForward.z;
        }
    }
    readbackBuffer->Unmap(0, nullptr);

    uint32_t inversions = 0;
    float maxInversionDiff = 0.0f;
    for (uint32_t i = 0; i < numPoints - 1; i++)
    {
        if (gpuSortedDepths[i] < gpuSortedDepths[i + 1] - 1e-4f)
        {
            float diff = gpuSortedDepths[i + 1] - gpuSortedDepths[i];
            inversions++;
            if (diff > maxInversionDiff) maxInversionDiff = diff;
        }
    }

    // 9. Output Comparison & Performance Regression Report
    std::cout << "========================================================\n";
    std::cout << "             PERFORMANCE & REGRESSION REPORT            \n";
    std::cout << "========================================================\n";
    std::cout << "Total Elements:            " << numPoints << " (" << (testMode == 1 ? "Packed 8-Byte" : "Raw Float32") << ")\n";
    std::cout << "GPU Dispatches:            " << totalPasses << " compute passes\n\n";

    std::cout << "[CPU Reference Sorts]:\n";
    std::cout << "  - CPU std::sort:         " << cpuStdTimeMs << " ms\n";
    std::cout << "  - CPU OpenMP Radix Sort: " << cpuRadixTimeMs << " ms\n\n";

    std::cout << "[GPU Bitonic Sort Timings]:\n";
    std::cout << "  - CPU Recording Time:    " << cpuRecordTimeMs << " ms (Driver API overhead)\n";
    std::cout << "  - GPU Hardware Time:     " << gpuHardwareTimeMs << " ms (Actual VRAM execution)\n";
    std::cout << "  - GPU Frame Latency:     " << gpuWallTimeMs << " ms (Queue submit to completion)\n\n";

    std::cout << "[Comparative Analysis vs CPU OpenMP Radix Sort]:\n";
    double ratioGpuToCpu = (cpuRecordTimeMs + gpuHardwareTimeMs) / cpuRadixTimeMs;
    std::cout << "  - Total GPU Pipeline:    " << (cpuRecordTimeMs + gpuHardwareTimeMs) << " ms\n";
    std::cout << "  - Ratio (GPU / CPU):     " << ratioGpuToCpu << "x ";
    if (ratioGpuToCpu > 1.0)
    {
        std::cout << "[REGRESSION: GPU is " << ratioGpuToCpu << "x SLOWER than CPU Radix Sort]\n";
    }
    else
    {
        std::cout << "[SPEEDUP: GPU is " << (1.0 / ratioGpuToCpu) << "x FASTER than CPU Radix Sort]\n";
    }

    std::cout << "\n[Accuracy Summary]:\n";
    std::cout << "  - Sorting Inversions:    " << inversions << " / " << (numPoints - 1) << " (" << (inversions == 0 ? "0% errors" : "ERRORS DETECTED") << ")\n";
    std::cout << "  - Max Inversion Error:   " << maxInversionDiff << " units\n";
    if (inversions == 0)
    {
        std::cout << ">>> [RESULT: PASS] 100% ACCURACY BACK-TO-FRONT GPU SORT <<<\n";
    }
    else
    {
        std::cout << ">>> [RESULT: FAIL] GPU SORT INACCURACY DETECTED <<<\n";
    }
    std::cout << "========================================================\n";

    return (inversions == 0) ? 0 : 1;
}
