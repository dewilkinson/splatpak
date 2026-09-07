// GPURadixSortCS.hlsl
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// High-performance GPU bitonic depth sort (LDS local sort + global merge stages, key-index
// pairs) used to keep overlapping alpha-blended splats/chunks in correct back-to-front
// order every frame without a CPU round-trip. Targets Compute Shader 5.1/6.0.

cbuffer BitonicSortCB : register(b0)
{
    float3 g_CamPos;
    float  g_Pad0;
    float3 g_CamForward;
    uint   g_TotalSurfels;
    uint   g_Level;          // Bitonic stage level (2, 4, 8, ... N)
    uint   g_LevelMask;      // Bitonic comparison mask (level/2, level/4, ... 1)
    uint   g_NumElements;    // Padded power-of-2 count
    uint   g_RenderMode;     // 1 = Packed (8-byte), 2 = Raw Float32
    float3 g_AABBMin;
    float  g_Pad1;
    float3 g_AABBExtents;
    float  g_Pad2;
};

struct PackedSurfel
{
    uint packedPosRadius;
    uint packedNormalColor;
};

struct RawSurfel
{
    float3 position;
    float3 normal;
    float3 color;
    float  radius;
    uint   sourceIndex; // Splat mode bookkeeping (SurfelVertex::sourceIndex); unused by the shaders
};

struct SortPair
{
    uint key;   // 32-bit orderable depth key
    uint index; // 32-bit surfel or chunk index
};

struct MeshletChunk
{
    float3 center;
    float  boundingRadius;
    float3 aabbMin;
    uint   surfelOffset;
    float3 aabbExtents;
    uint   surfelCount;
    float  blendWeight;
    uint   lodLevel;
    float  dilationMorph; // Morph dilation factor for silhouette reconstruction
    float  isSilhouette;  // 1.0 if silhouette chunk, 0.0 otherwise
    float3 coneAxis;      // Average unit normal vector of cluster
    float  coneCutoff;    // cos(theta_max) of cluster normal cone (-1.0 = disabled)
};


StructuredBuffer<PackedSurfel>   g_InPackedSurfels : register(t0);
StructuredBuffer<RawSurfel>      g_InRawSurfels    : register(t1);
StructuredBuffer<MeshletChunk>   g_InChunks        : register(t2);

RWStructuredBuffer<SortPair>     g_SortPairs       : register(u0);
RWStructuredBuffer<PackedSurfel> g_OutPackedSurfels: register(u1);
RWStructuredBuffer<RawSurfel>    g_OutRawSurfels   : register(u2);
RWStructuredBuffer<uint>         g_OutSortedChunkIndices : register(u3);

uint FloatToOrderableUint(float f)
{
    uint u = asuint(f);
    uint mask = ((int)(u >> 31) != 0) ? 0xFFFFFFFF : 0x80000000;
    return u ^ mask;
}

// =========================================================================
// Pass 1: Project depth keys ONCE into key-index pairs
// =========================================================================
[numthreads(256, 1, 1)]
void ProjectKeysCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint idx = dispatchThreadId.x;
    if (idx >= g_NumElements)
        return;

    SortPair pair;
    pair.index = idx;

    if (idx >= g_TotalSurfels)
    {
        pair.key = 0; // Dummy padding element sorts to the very end
    }
    else if (g_RenderMode == 2)
    {
        RawSurfel s = g_InRawSurfels[idx];
        if (s.radius < 0.0f)
        {
            pair.key = 0;
        }
        else
        {
            float d = dot(s.position - g_CamPos, g_CamForward);
            pair.key = FloatToOrderableUint(d);
        }
    }
    else
    {
        PackedSurfel s = g_InPackedSurfels[idx];
        if (s.packedPosRadius == 0xFFFFFFFF && s.packedNormalColor == 0xFFFFFFFF)
        {
            pair.key = 0;
        }
        else
        {
            uint qx = s.packedPosRadius & 0x3FF;
            uint qy = (s.packedPosRadius >> 10) & 0x3FF;
            uint qz = (s.packedPosRadius >> 20) & 0x3FF;
            float3 pos = g_AABBMin + float3(qx / 1023.0f, qy / 1023.0f, qz / 1023.0f) * g_AABBExtents;
            float d = dot(pos - g_CamPos, g_CamForward);
            pair.key = FloatToOrderableUint(d);
        }
    }

    g_SortPairs[idx] = pair;
}

// =========================================================================
// Pass 2: LDS Local Block Sort (all stages level 2 up to 1024 in 1 dispatch)
// =========================================================================
#define GROUP_SIZE 512
#define BLOCK_SIZE 1024

groupshared SortPair s_data[BLOCK_SIZE]; // 8 KB of shared memory

[numthreads(GROUP_SIZE, 1, 1)]
void BitonicLocalSortCS(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    uint t = groupThreadId.x;
    uint groupBase = groupId.x * BLOCK_SIZE;
    uint idx0 = groupBase + t;
    uint idx1 = groupBase + t + GROUP_SIZE;

    SortPair zeroPair;
    zeroPair.key = 0;
    zeroPair.index = 0;

    if (idx0 < g_NumElements)
        s_data[t] = g_SortPairs[idx0];
    else
        s_data[t] = zeroPair;

    if (idx1 < g_NumElements)
        s_data[t + GROUP_SIZE] = g_SortPairs[idx1];
    else
        s_data[t + GROUP_SIZE] = zeroPair;

    [unroll]
    for (uint level = 2; level <= BLOCK_SIZE; level <<= 1)
    {
        for (uint levelMask = level >> 1; levelMask > 0; levelMask >>= 1)
        {
            GroupMemoryBarrierWithGroupSync();

            uint i = ((t / levelMask) * (levelMask * 2)) + (t % levelMask);
            uint j = i + levelMask;
            uint global_i = groupBase + i;
            bool dir = ((global_i & level) == 0);

            SortPair a = s_data[i];
            SortPair b = s_data[j];

            // Descending order: want a.key >= b.key
            bool swap = dir ? (a.key < b.key) : (a.key > b.key);
            if (swap)
            {
                s_data[i] = b;
                s_data[j] = a;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (idx0 < g_NumElements) g_SortPairs[idx0] = s_data[t];
    if (idx1 < g_NumElements) g_SortPairs[idx1] = s_data[t + GROUP_SIZE];
}

// =========================================================================
// Pass 3: Global Merge Pass (For levelMask >= 1024 across threadgroups)
// =========================================================================
[numthreads(256, 1, 1)]
void BitonicGlobalSortCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint i = dispatchThreadId.x;
    if (i >= g_NumElements)
        return;

    uint j = i ^ g_LevelMask;

    if (j > i)
    {
        bool dir = ((i & g_Level) == 0);

        SortPair a = g_SortPairs[i];
        SortPair b = g_SortPairs[j];

        bool swap = dir ? (a.key < b.key) : (a.key > b.key);
        if (swap)
        {
            g_SortPairs[i] = b;
            g_SortPairs[j] = a;
        }
    }
}

// =========================================================================
// Pass 4: Local LDS Merge Pass (executes inner stages 512 down to 1 in 1 dispatch)
// =========================================================================
[numthreads(GROUP_SIZE, 1, 1)]
void BitonicLocalMergeCS(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    uint t = groupThreadId.x;
    uint groupBase = groupId.x * BLOCK_SIZE;
    uint idx0 = groupBase + t;
    uint idx1 = groupBase + t + GROUP_SIZE;

    SortPair zeroPair;
    zeroPair.key = 0;
    zeroPair.index = 0;

    if (idx0 < g_NumElements)
        s_data[t] = g_SortPairs[idx0];
    else
        s_data[t] = zeroPair;

    if (idx1 < g_NumElements)
        s_data[t + GROUP_SIZE] = g_SortPairs[idx1];
    else
        s_data[t + GROUP_SIZE] = zeroPair;

    for (uint levelMask = 512; levelMask > 0; levelMask >>= 1)
    {
        GroupMemoryBarrierWithGroupSync();

        uint i = ((t / levelMask) * (levelMask * 2)) + (t % levelMask);
        uint j = i + levelMask;
        uint global_i = groupBase + i;
        bool dir = ((global_i & g_Level) == 0);

        SortPair a = s_data[i];
        SortPair b = s_data[j];

        bool swap = dir ? (a.key < b.key) : (a.key > b.key);
        if (swap)
        {
            s_data[i] = b;
            s_data[j] = a;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (idx0 < g_NumElements) g_SortPairs[idx0] = s_data[t];
    if (idx1 < g_NumElements) g_SortPairs[idx1] = s_data[t + GROUP_SIZE];
}

// =========================================================================
// Pass 5: Fast 1-Pass Gather (Permutes original surfels to sorted output buffer)
// =========================================================================
[numthreads(256, 1, 1)]
void GatherSurfelsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint outIdx = dispatchThreadId.x;
    if (outIdx >= g_NumElements)
        return;

    uint origIdx = g_SortPairs[outIdx].index;

    if (g_RenderMode == 2)
    {
        g_OutRawSurfels[outIdx] = g_InRawSurfels[origIdx];
    }
    else
    {
        g_OutPackedSurfels[outIdx] = g_InPackedSurfels[origIdx];
    }
}

// =========================================================================
// Chunk Pass 1: Project Chunk Centers into Key-Index Pairs for Coarse Sorting
// =========================================================================
[numthreads(256, 1, 1)]
void ProjectChunkKeysCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint idx = dispatchThreadId.x;
    if (idx >= g_NumElements)
        return;

    SortPair pair;
    pair.index = idx;

    if (idx >= g_TotalSurfels) // Here g_TotalSurfels stores total chunk count
    {
        pair.key = 0; // Padding chunks sort to the end
    }
    else
    {
        MeshletChunk chunk = g_InChunks[idx];
        float d = dot(chunk.center - g_CamPos, g_CamForward);
        pair.key = FloatToOrderableUint(d);
    }

    g_SortPairs[idx] = pair;
}

// =========================================================================
// Chunk Pass 2: Gather Sorted Chunk Indices
// =========================================================================
[numthreads(256, 1, 1)]
void GatherChunkIndicesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint outIdx = dispatchThreadId.x;
    if (outIdx >= g_NumElements)
        return;

    g_OutSortedChunkIndices[outIdx] = g_SortPairs[outIdx].index;
}

