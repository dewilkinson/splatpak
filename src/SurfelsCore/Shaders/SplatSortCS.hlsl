// SplatSortCS.hlsl
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Splat mode's per-splat depth ordering. Every frame the render list's chunks are expanded into
// one slot per Gaussian (ExpandCS), each slot gets a 32-bit depth key measured from the viewer,
// and the slots are sorted far-to-near by a 4-pass least-significant-digit radix sort
// (HistogramCS, ScanCS, ScatterCS per 8-bit digit). The mesh/vertex shaders then draw the slots
// in sorted order, which is the same back-to-front per-splat order a reference splat viewer uses.
//
// Slots are padded to a multiple of BLOCK (1024) with sentinel pairs (key and value 0xFFFFFFFF);
// culled slots also carry the maximum key. LSD radix sorting is stable, so the culled slots (all
// below the real count in the input) end up ahead of the sentinels; the draw walks the first
// g_SlotCount slots and skips culled ones.

cbuffer SplatSortCB : register(b0)
{
    float4x4 g_CullViewProj;   // Frustum used to drop whole chunks (viewer, or the frozen camera when detached)
    float3   g_CamPos;         // Viewer position: depth keys are always the viewer's
    uint     g_TotalChunks;
    float3   g_CamForward;
    uint     g_SlotCount;      // Real slots (sum of the chunks' surfel counts)
    float3   g_AABBMin;
    uint     g_Pass;           // Radix pass 0..3: digit = (key >> (8 * pass)) & 0xFF
    float3   g_AABBExtents;
    uint     g_NumBlocks;      // Padded slots / BLOCK
    uint     g_PaddedSlotCount;
    uint     g_Pad0, g_Pad1, g_Pad2;
};

struct PackedSplat { uint w0, w1, w2, w3, w4; };

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
    float  dilationMorph;
    float  isSilhouette;
    float3 coneAxis;
    float  coneCutoff;
};

StructuredBuffer<PackedSplat>   g_Splats        : register(t0);
StructuredBuffer<MeshletChunk>  g_Chunks        : register(t1);
StructuredBuffer<uint>          g_ChunkSlotBase : register(t2); // First slot of each chunk (exclusive prefix of surfelCount)
RWStructuredBuffer<uint2>       g_SlotInfo      : register(u0); // x = surfel index, y = chunk index | SLOT_CULLED_BIT
RWStructuredBuffer<uint2>       g_PairsIn       : register(u1); // x = key, y = slot
RWStructuredBuffer<uint2>       g_PairsOut      : register(u2);
RWStructuredBuffer<uint>        g_Hist          : register(u3); // [digit * numBlocks + block]

#define SLOT_CULLED_BIT 0x80000000u
#define BLOCK 1024
#define THREADS 256
#define PER_THREAD 4

uint FloatToOrderableUint(float f)
{
    uint u = asuint(f);
    uint mask = ((int)(u >> 31) != 0) ? 0xFFFFFFFF : 0x80000000;
    return u ^ mask;
}

bool ChunkInFrustum(MeshletChunk chunk)
{
    float3 bMin = chunk.aabbMin;
    float3 bMax = chunk.aabbMin + chunk.aabbExtents;
    int outsideLeft = 0, outsideRight = 0, outsideBottom = 0, outsideTop = 0, outsideNear = 0, outsideFar = 0;
    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float3 corner = float3((i & 1) ? bMax.x : bMin.x, (i & 2) ? bMax.y : bMin.y, (i & 4) ? bMax.z : bMin.z);
        float4 c = mul(g_CullViewProj, float4(corner, 1.0));
        if (c.w > 0.0001)
        {
            if (c.x < -c.w) outsideLeft++;
            if (c.x >  c.w) outsideRight++;
            if (c.y < -c.w) outsideBottom++;
            if (c.y >  c.w) outsideTop++;
            if (c.z <  0.0) outsideNear++;
            if (c.z >  c.w) outsideFar++;
        }
        else outsideNear++;
    }
    return (outsideLeft < 8) && (outsideRight < 8) && (outsideBottom < 8) && (outsideTop < 8) && (outsideNear < 8) && (outsideFar < 8);
}

// One 64-thread group per chunk of the render list: fills the chunk's slots with their surfel
// index, chunk index and depth key. A chunk outside the frustum has every slot flagged culled.
[numthreads(64, 1, 1)]
void ExpandCS(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID)
{
    uint chunkIdx = groupId.x;
    if (chunkIdx >= g_TotalChunks) return;
    MeshletChunk chunk = g_Chunks[chunkIdx];
    uint base = g_ChunkSlotBase[chunkIdx];
    bool visible = ChunkInFrustum(chunk);
    for (uint t = threadId; t < chunk.surfelCount; t += 64)
    {
        uint slot = base + t;
        if (slot >= g_SlotCount) break;
        uint surfelIndex = chunk.surfelOffset + t;
        uint key = 0xFFFFFFFFu;
        uint info = chunkIdx | SLOT_CULLED_BIT;
        if (visible)
        {
            PackedSplat s = g_Splats[surfelIndex];
            float3 pos = g_AABBMin + float3((s.w0 & 0xFFFFu) / 65535.0, ((s.w0 >> 16) & 0xFFFFu) / 65535.0, (s.w1 & 0xFFFFu) / 65535.0) * g_AABBExtents;
            float depth = dot(pos - g_CamPos, g_CamForward);
            key = 0xFFFFFFFEu - FloatToOrderableUint(depth); // Far first, and never the sentinel value
            info = chunkIdx;
        }
        g_SlotInfo[slot] = uint2(surfelIndex, info);
        g_PairsIn[slot] = uint2(key, slot);
    }
}

// Pass 0 reads the expanded pairs; slots past g_SlotCount are sentinels that no kernel wrote.
uint2 LoadPair(uint idx)
{
    if (idx >= g_PaddedSlotCount) return uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    if (g_Pass == 0 && idx >= g_SlotCount) return uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    return g_PairsIn[idx];
}

uint Digit(uint key) { return (key >> (8u * g_Pass)) & 0xFFu; }

groupshared uint s_hist[256];

[numthreads(THREADS, 1, 1)]
void HistogramCS(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID)
{
    s_hist[threadId] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint block = groupId.x;
    [unroll]
    for (uint j = 0; j < PER_THREAD; j++)
    {
        uint idx = block * BLOCK + threadId * PER_THREAD + j;
        uint2 p = LoadPair(idx);
        InterlockedAdd(s_hist[Digit(p.x)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    g_Hist[threadId * g_NumBlocks + block] = s_hist[threadId];
}

// Exclusive prefix sum over the whole digit-major histogram (256 * numBlocks entries), one group.
groupshared uint s_scan[1024];

[numthreads(1024, 1, 1)]
void ScanCS(uint threadId : SV_GroupThreadID)
{
    uint total = 256u * g_NumBlocks;
    uint carry = 0;
    for (uint tile = 0; tile < total; tile += 1024)
    {
        uint idx = tile + threadId;
        uint v = (idx < total) ? g_Hist[idx] : 0u;
        s_scan[threadId] = v;
        GroupMemoryBarrierWithGroupSync();
        [unroll]
        for (uint offset = 1; offset < 1024; offset <<= 1)
        {
            uint add = (threadId >= offset) ? s_scan[threadId - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            s_scan[threadId] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        uint inclusive = s_scan[threadId];
        if (idx < total) g_Hist[idx] = carry + inclusive - v;
        uint tileTotal = s_scan[1023];
        GroupMemoryBarrierWithGroupSync();
        carry += tileTotal;
    }
}

// Sorts one block of 1024 pairs by the current digit (stable), then scatters each pair to its
// global position: the scanned histogram gives where this block's run of each digit starts.
groupshared uint s_key[BLOCK];
groupshared uint s_val[BLOCK];
groupshared uint s_key2[BLOCK];
groupshared uint s_val2[BLOCK];
groupshared uint s_zeroScan[THREADS];
groupshared uint s_digitStart[256];

[numthreads(THREADS, 1, 1)]
void ScatterCS(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID)
{
    uint block = groupId.x;
    uint myBase = threadId * PER_THREAD;
    [unroll]
    for (uint j = 0; j < PER_THREAD; j++)
    {
        uint2 p = LoadPair(block * BLOCK + myBase + j);
        s_key[myBase + j] = p.x;
        s_val[myBase + j] = p.y;
    }
    GroupMemoryBarrierWithGroupSync();

    // Eight stable 1-bit partitions on the digit's bits, least significant first.
    for (uint bit = 0; bit < 8; bit++)
    {
        uint shift = 8u * g_Pass + bit;
        uint zeros = 0;
        [unroll]
        for (uint j1 = 0; j1 < PER_THREAD; j1++)
            zeros += 1u - ((s_key[myBase + j1] >> shift) & 1u);
        s_zeroScan[threadId] = zeros;
        GroupMemoryBarrierWithGroupSync();
        [unroll]
        for (uint offset = 1; offset < THREADS; offset <<= 1)
        {
            uint add = (threadId >= offset) ? s_zeroScan[threadId - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            s_zeroScan[threadId] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        uint totalZeros = s_zeroScan[THREADS - 1];
        uint zerosBefore = s_zeroScan[threadId] - zeros; // Exclusive
        [unroll]
        for (uint j2 = 0; j2 < PER_THREAD; j2++)
        {
            uint i = myBase + j2;
            uint k = s_key[i];
            uint isOne = (k >> shift) & 1u;
            uint dest = isOne ? (totalZeros + (i - zerosBefore)) : zerosBefore;
            s_key2[dest] = k;
            s_val2[dest] = s_val[i];
            zerosBefore += 1u - isOne;
        }
        GroupMemoryBarrierWithGroupSync();
        [unroll]
        for (uint j3 = 0; j3 < PER_THREAD; j3++)
        {
            s_key[myBase + j3] = s_key2[myBase + j3];
            s_val[myBase + j3] = s_val2[myBase + j3];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Where each digit's run starts inside the sorted block.
    s_digitStart[threadId] = 0;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint j4 = 0; j4 < PER_THREAD; j4++)
    {
        uint i = myBase + j4;
        uint d = Digit(s_key[i]);
        if (i == 0 || Digit(s_key[i - 1]) != d) s_digitStart[d] = i;
    }
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint j5 = 0; j5 < PER_THREAD; j5++)
    {
        uint i = myBase + j5;
        uint d = Digit(s_key[i]);
        uint dest = g_Hist[d * g_NumBlocks + block] + (i - s_digitStart[d]);
        g_PairsOut[dest] = uint2(s_key[i], s_val[i]);
    }
}
