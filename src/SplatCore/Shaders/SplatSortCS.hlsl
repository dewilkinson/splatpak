// SplatSortCS.hlsl
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
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
// g_SlotCount slots and skips culled ones. In Detach Camera mode (g_DetachedCull) nothing is dropped:
// a chunk outside the frozen camera's frustum is sorted like any other and its slots carry
// SLOT_FROZEN_BIT, so the draw paints them red instead of hiding them.
//
// Occlusion culling (g_OcclusionCull): before the expand, OccClearCS and OccRasterCS rasterise the
// package's occluder mesh -- the solid body inside the model -- from the culling camera into a small
// grid (g_OccWidth x g_OccHeight) holding, per pixel, the nearest occluder depth (clip w). Each triangle
// is written at its farthest vertex's depth, so the grid is never nearer than the mesh. ExpandCS then
// drops a chunk when its bounding box, grown by a pixel, lies behind the occluder at every pixel it
// covers: such a chunk cannot show (the splats are depth-tested against the same mesh), so it is
// neither sorted nor drawn. Detach Camera keeps it, flagged like a frustum-culled chunk.

cbuffer SplatSortCB : register(b0)
{
    float4x4 g_CullViewProj;   // Frustum used to drop whole chunks (viewer, or the frozen camera when detached)
    float3   g_CamPos;         // Viewer position: depth keys are always the viewer's
    uint     g_TotalChunks;
    float3   g_CamForward;
    uint     g_SlotCount;      // Real slots (sum of the chunks' splat counts)
    float3   g_AABBMin;
    uint     g_Pass;           // Radix pass 0..3: digit = (key >> (8 * pass)) & 0xFF
    float3   g_AABBExtents;
    uint     g_NumBlocks;      // Padded slots / BLOCK
    uint     g_PaddedSlotCount;
    uint     g_DetachedCull;   // 1 = Detach Camera: keep chunks outside the frozen frustum, flagged SLOT_FROZEN_BIT
    uint     g_OcclusionCull;  // 1 = drop chunks hidden behind the occluder mesh (see the header)
    uint     g_OccTriCount;    // Occluder mesh triangles
    uint     g_OccWidth;       // Occlusion grid size in pixels
    uint     g_OccHeight;
    uint     g_Pad3, g_Pad4;
};

struct PackedSplat { uint w0, w1, w2, w3, w4; };

struct MeshletChunk
{
    float3 center;
    float  boundingRadius;
    float3 aabbMin;
    uint   splatOffset;
    float3 aabbExtents;
    uint   splatCount;
    float  blendWeight;
    uint   lodLevel;
    float  dilationMorph;
    float  isSilhouette;
    float3 coneAxis;
    float  coneCutoff;
};

StructuredBuffer<PackedSplat>   g_Splats        : register(t0);
StructuredBuffer<MeshletChunk>  g_Chunks        : register(t1);
StructuredBuffer<uint>          g_ChunkSlotBase : register(t2); // First slot of each chunk (exclusive prefix of splatCount)
RWStructuredBuffer<uint2>       g_SlotInfo      : register(u0); // x = splat index, y = chunk index | SLOT_CULLED_BIT
RWStructuredBuffer<uint2>       g_PairsIn       : register(u1); // x = key, y = slot
RWStructuredBuffer<uint2>       g_PairsOut      : register(u2);
RWStructuredBuffer<uint>        g_Hist          : register(u3); // [digit * numBlocks + block]

struct OccluderVertex { float3 position; uint colour; uint normal; }; // ShadowMeshDisplay's 20-byte vertex
StructuredBuffer<uint>           g_OccIndices  : register(t3);
StructuredBuffer<OccluderVertex> g_OccVertices : register(t4);
RWStructuredBuffer<uint>         g_OccDepth    : register(u4); // Per grid pixel: nearest occluder depth (clip w) as float bits; OCC_EMPTY = none
RWStructuredBuffer<uint>         g_OccCulled   : register(u5); // [0] chunks, [1] splats dropped by occlusion (statistics)

#define SLOT_CULLED_BIT 0x80000000u
#define SLOT_FROZEN_BIT 0x40000000u
#define OCC_EMPTY 0x7F7FFFFFu  // FLT_MAX: no occluder in this pixel
#define OCC_NEAR 0.1           // The projection's near plane: geometry nearer than this is never treated as occluded or occluding
#define OCC_MAX_TRI_PIXELS 8192u   // A triangle covering more of the grid than this is skipped (conservative: occludes nothing)
#define OCC_MAX_CHUNK_PIXELS 4096u // A chunk covering more of the grid than this is never culled
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

// Projects a world point with the culling camera into occlusion-grid pixels: x, y and the clip w (depth)
float3 OccProject(float3 p)
{
    float4 c = mul(g_CullViewProj, float4(p, 1.0));
    float iw = 1.0 / max(c.w, 1e-6);
    return float3((c.x * iw * 0.5 + 0.5) * g_OccWidth, (-c.y * iw * 0.5 + 0.5) * g_OccHeight, c.w);
}

// Empties the occlusion grid and the statistics counters
[numthreads(256, 1, 1)]
void OccClearCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_OccWidth * g_OccHeight) g_OccDepth[id.x] = OCC_EMPTY;
    if (id.x < 2) g_OccCulled[id.x] = 0;
}

float OccEdge(float2 a, float2 b, float2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

// One thread per occluder triangle: writes the triangle's farthest depth into every grid pixel whose
// centre it covers, keeping the nearest per pixel. A triangle crossing the near plane is skipped.
[numthreads(64, 1, 1)]
void OccRasterCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_OccTriCount) return;
    float3 a = OccProject(g_OccVertices[g_OccIndices[id.x * 3 + 0]].position);
    float3 b = OccProject(g_OccVertices[g_OccIndices[id.x * 3 + 1]].position);
    float3 c = OccProject(g_OccVertices[g_OccIndices[id.x * 3 + 2]].position);
    if (min(a.z, min(b.z, c.z)) <= OCC_NEAR) return;
    float area = OccEdge(a.xy, b.xy, c.xy);
    if (abs(area) < 1e-8) return;
    if (area < 0.0) { float3 s = b; b = c; c = s; } // Either winding: make it counter-clockwise in grid space
    int x0 = max(0, (int)floor(min(a.x, min(b.x, c.x))));
    int y0 = max(0, (int)floor(min(a.y, min(b.y, c.y))));
    int x1 = min((int)g_OccWidth - 1, (int)floor(max(a.x, max(b.x, c.x))));
    int y1 = min((int)g_OccHeight - 1, (int)floor(max(a.y, max(b.y, c.y))));
    if (x1 < x0 || y1 < y0 || (uint)((x1 - x0 + 1) * (y1 - y0 + 1)) > OCC_MAX_TRI_PIXELS) return;
    uint depthBits = asuint(max(a.z, max(b.z, c.z))); // Positive floats order like their bits
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
        {
            float2 p = float2(x + 0.5, y + 0.5);
            if (OccEdge(a.xy, b.xy, p) >= 0.0 && OccEdge(b.xy, c.xy, p) >= 0.0 && OccEdge(c.xy, a.xy, p) >= 0.0)
            {
                uint prev;
                InterlockedMin(g_OccDepth[y * g_OccWidth + x], depthBits, prev);
            }
        }
}

groupshared uint gs_OccSeen;

// Whether the chunk lies behind the occluder at every grid pixel its bounding box covers (grown by one
// pixel, which absorbs the half pixel by which the raster's centre sampling can overreach a triangle's
// edge). The group's 64 threads scan the rectangle together; every thread of the group must call it.
bool ChunkOccluded(MeshletChunk chunk, uint threadId)
{
    float3 bMin = chunk.aabbMin;
    float3 bMax = chunk.aabbMin + chunk.aabbExtents;
    float nearW = 1e30;
    float2 lo = float2(1e30, 1e30), hi = float2(-1e30, -1e30);
    bool crossesNear = false;
    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float3 p = OccProject(float3((i & 1) ? bMax.x : bMin.x, (i & 2) ? bMax.y : bMin.y, (i & 4) ? bMax.z : bMin.z));
        if (p.z <= OCC_NEAR) crossesNear = true;
        nearW = min(nearW, p.z);
        lo = min(lo, p.xy);
        hi = max(hi, p.xy);
    }
    int x0 = max(0, (int)floor(lo.x) - 1), y0 = max(0, (int)floor(lo.y) - 1);
    int x1 = min((int)g_OccWidth - 1, (int)floor(hi.x) + 1), y1 = min((int)g_OccHeight - 1, (int)floor(hi.y) + 1);
    uint w = (uint)max(0, x1 - x0 + 1), h = (uint)max(0, y1 - y0 + 1);
    bool candidate = !crossesNear && w > 0 && h > 0 && w * h <= OCC_MAX_CHUNK_PIXELS;

    if (threadId == 0) gs_OccSeen = 0;
    GroupMemoryBarrierWithGroupSync();
    if (candidate)
    {
        for (uint k = threadId; k < w * h; k += 64)
        {
            uint idx = (uint)(y0 + (int)(k / w)) * g_OccWidth + (uint)(x0 + (int)(k % w));
            if (asfloat(g_OccDepth[idx]) >= nearW) { InterlockedOr(gs_OccSeen, 1u); break; }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    return candidate && gs_OccSeen == 0;
}

// One 64-thread group per chunk of the render list: fills the chunk's slots with their splat
// index, chunk index and depth key. A chunk outside the frustum has every slot flagged culled (or, in
// Detach Camera mode, flagged frozen-culled and still sorted); so has a chunk hidden behind the occluder.
[numthreads(64, 1, 1)]
void ExpandCS(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID)
{
    uint chunkIdx = groupId.x;
    if (chunkIdx >= g_TotalChunks) return;
    MeshletChunk chunk = g_Chunks[chunkIdx];
    uint base = g_ChunkSlotBase[chunkIdx];
    bool inFrustum = ChunkInFrustum(chunk);
    // An if, not ?: or &&: HLSL before 2021 evaluates both sides of those, and the grid is only valid when culling is on.
    // The condition is uniform across the group, as ChunkOccluded's barriers need.
    bool occluded = false;
    if (g_OcclusionCull == 1 && inFrustum)
        occluded = ChunkOccluded(chunk, threadId);
    if (occluded && threadId == 0)
    {
        InterlockedAdd(g_OccCulled[0], 1u);
        InterlockedAdd(g_OccCulled[1], chunk.splatCount);
    }
    bool kept = inFrustum && !occluded;
    bool visible = kept || g_DetachedCull == 1;
    for (uint t = threadId; t < chunk.splatCount; t += 64)
    {
        uint slot = base + t;
        if (slot >= g_SlotCount) break;
        uint splatIndex = chunk.splatOffset + t;
        uint key = 0xFFFFFFFFu;
        uint info = chunkIdx | SLOT_CULLED_BIT;
        if (visible)
        {
            PackedSplat s = g_Splats[splatIndex];
            float3 pos = g_AABBMin + float3((s.w0 & 0xFFFFu) / 65535.0, ((s.w0 >> 16) & 0xFFFFu) / 65535.0, (s.w1 & 0xFFFFu) / 65535.0) * g_AABBExtents;
            float depth = dot(pos - g_CamPos, g_CamForward);
            key = 0xFFFFFFFEu - FloatToOrderableUint(depth); // Far first, and never the sentinel value
            info = kept ? chunkIdx : (chunkIdx | SLOT_FROZEN_BIT);
        }
        g_SlotInfo[slot] = uint2(splatIndex, info);
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
