// Surfels.hlsl
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The core mesh-shader splat pipeline: every surfel is emitted procedurally by an
// amplification/mesh shader pair each frame, straight from a StructuredBuffer -- there
// is no vertex/index buffer or DrawInstanced anywhere in this file. Also home to the
// GPU silhouette item-prepass (itemMS/itemPS) and the interior occlusion volume pass
// (occluderMS/occluderPS).
//
// Three render paths share the main splat stage (mainAS/mainMS/mainPS):
//  1. Procedural Fibonacci-sphere point-splat generation on-chip (demo/fallback mode).
//  2. High-performance progressive streaming from a StructuredBuffer of 8-byte PackedSurfel structs.
//  3. Normal-oriented tangent-plane discs or camera-facing billboard quads.

#define SURFELS_PER_GROUP 64
#define AS_GROUP_SIZE 32

struct PackedSurfel
{
    uint packedPosRadius;
    uint packedNormalColor; // low 16-bit: oct16 normal, high 16-bit: rgb565 color
};

struct RawSurfel
{
    float3 position;
    float3 normal;
    float3 color;
    float  radius;
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

struct OcclusionVoxel
{
    float3 center;
    float  halfSize;    // Always half a grid cell -- adjacent cubes share faces
    uint   packedColor; // Bits 0..15: RGB565 (see UnpackColorRGB565); bits 16..21: exposed-face mask,
                        // one bit per face in s_occluderFaceIdx order (-Z,+Z,-X,+X,-Y,+Y). 0 = legacy
                        // file with no mask, treated as all faces exposed.
};

StructuredBuffer<PackedSurfel>  g_SurfelBuffer       : register(t0);
StructuredBuffer<RawSurfel>     g_RawSurfelBuffer    : register(t1);
StructuredBuffer<MeshletChunk>  g_ChunkBuffer        : register(t2);
StructuredBuffer<uint>          g_SortedChunkIndices : register(t3);
StructuredBuffer<OcclusionVoxel> g_OcclusionVoxelBuffer : register(t4);

cbuffer SurfelsCB : register(b0)
{
    float4x4 g_ViewProj;
    float3   g_CamRight;
    float    g_Radius;
    float3   g_CamUp;
    float    g_Time;
    float3   g_ViewerEyePos;
    float    g_SphereRadius;
    uint     g_SurfelCount;
    uint     g_RenderMode; // 0 = Procedural Sphere, 1 = Quantized 8-Byte, 2 = Raw Float32 Points
    uint     g_OrientMode; // 0 = Normal-Oriented Discs, 1 = Camera-Facing Billboards
    uint     g_TotalChunks;
    float3   g_AABBMin;
    uint     g_UseChunkedPipeline; // 0 = Flat buffer, 1 = Micro-Chunked Hierarchical
    float3   g_AABBExtents;
    uint     g_UseDetachedCullCam;
    float4x4 g_CullViewProj;
    float3   g_CullEyePos;
    uint     g_EnableDithering;
    uint     g_HighlightSilhouette;
    uint     g_EnableConeCulling;
    uint     g_ShowOnlyLocked;
    uint     g_ShowChunkStream;
    uint     g_EnableOcclusionCulling;
    float    g_OcclusionShrinkCells; // Live shrink: exposed occluder faces are pulled inward by this many cells
    uint     g_ShowOcclusionVolumeOnly;
    uint     g_OcclusionVoxelCount;
};

struct ChunkPayload
{
    uint chunkIndices[AS_GROUP_SIZE];
    uint flatGroupIndex;
};

struct VSOut
{
    float4 pos         : SV_POSITION;
    float2 uv          : TEXCOORD0;
    float3 color       : COLOR0;
    float3 norm        : NORMAL0;
    float  blendWeight : BLENDWEIGHT0;
    float  isSil       : TEXCOORD1;
};

// =========================================================================
// GPU Unpack & Math Helpers
// =========================================================================

float3 UnpackNormalOct16(uint packedOct)
{
    int ix = (int)(packedOct & 0xFF);
    int iy = (int)((packedOct >> 8) & 0xFF);
    float2 e = float2(ix, iy) * (2.0 / 255.0) - 1.0;
    float3 v = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
    {
        float2 signNotZero = float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
        v.xy = (1.0 - abs(v.yx)) * signNotZero;
    }
    return normalize(v);
}

float3 UnpackColorRGB565(uint packedRGB)
{
    float r = ((packedRGB >> 11) & 0x1F) / 31.0;
    float g = ((packedRGB >> 5) & 0x3F) / 63.0;
    float b = (packedRGB & 0x1F) / 31.0;
    return float3(r, g, b);
}

float3 HashColor(uint id)
{
    uint n = id * 2654435761u;
    return float3(
        ((n >> 16) & 0xFF) / 255.0,
        ((n >> 8) & 0xFF) / 255.0,
        (n & 0xFF) / 255.0
    );
}

float3 FibonacciSpherePoint(uint i, uint n)
{
    float phi = 2.399963229728653; // Golden angle in radians
    float y = 1.0 - ((float)i / (float)max(1u, n - 1u)) * 2.0;
    float r = sqrt(max(0.0, 1.0 - y * y));
    float theta = phi * (float)i;
    return float3(r * cos(theta), y, r * sin(theta));
}

// =========================================================================
// Amplification / Task Shader Stage (mainAS)
// =========================================================================

groupshared ChunkPayload s_Payload;

[NumThreads(AS_GROUP_SIZE, 1, 1)]
void mainAS(
    uint3 groupId  : SV_GroupID,
    uint  threadId : SV_GroupThreadID)
{
    if (g_UseChunkedPipeline == 0)
    {
        // Flat Buffer Mode: 1:1 pass-through to Mesh Shader
        uint groupBase = groupId.x * SURFELS_PER_GROUP;
        if (threadId == 0)
        {
            s_Payload.flatGroupIndex = groupId.x;
            if (groupBase < g_SurfelCount)
            {
                DispatchMesh(1, 1, 1, s_Payload);
            }
            else
            {
                DispatchMesh(0, 1, 1, s_Payload);
            }
        }
        return;
    }

    uint globalChunkIdx = groupId.x * AS_GROUP_SIZE + threadId;
    bool isVisible = false;
    uint chunkIdx = 0;

    if (globalChunkIdx < g_TotalChunks)
    {
        chunkIdx = g_SortedChunkIndices[globalChunkIdx];
        if (chunkIdx >= g_TotalChunks)
        {
            chunkIdx = globalChunkIdx;
        }
        MeshletChunk chunk = g_ChunkBuffer[chunkIdx];

        // Conservative AABB Frustum Culling (supports detached debug camera)
        float4x4 cullMatrix = (g_UseDetachedCullCam == 1) ? g_CullViewProj : g_ViewProj;
        float3 bMin = chunk.aabbMin;
        float3 bMax = chunk.aabbMin + chunk.aabbExtents;
        float3 corners[8] = {
            float3(bMin.x, bMin.y, bMin.z), float3(bMax.x, bMin.y, bMin.z),
            float3(bMin.x, bMax.y, bMin.z), float3(bMax.x, bMax.y, bMin.z),
            float3(bMin.x, bMin.y, bMax.z), float3(bMax.x, bMin.y, bMax.z),
            float3(bMin.x, bMax.y, bMax.z), float3(bMax.x, bMax.y, bMax.z)
        };

        int outsideLeft = 0, outsideRight = 0;
        int outsideBottom = 0, outsideTop = 0;
        int outsideNear = 0, outsideFar = 0;

        [unroll]
        for (int i = 0; i < 8; i++)
        {
            float4 c = mul(cullMatrix, float4(corners[i], 1.0));
            if (c.w > 0.0001)
            {
                if (c.x < -c.w) outsideLeft++;
                if (c.x >  c.w) outsideRight++;
                if (c.y < -c.w) outsideBottom++;
                if (c.y >  c.w) outsideTop++;
                if (c.z <  0.0) outsideNear++;
                if (c.z >  c.w) outsideFar++;
            }
            else
            {
                outsideNear++;
            }
        }

        isVisible = (outsideLeft < 8) && (outsideRight < 8) && 
                    (outsideBottom < 8) && (outsideTop < 8) && 
                    (outsideNear < 8) && (outsideFar < 8);

        // Conservative Normal Cone Backface Culling in Task Shader
        // This test approximates every point in the chunk as viewed from one direction (eye -> chunk
        // center), which only holds when the chunk is small relative to its distance from the camera.
        // Once the viewer is close enough that the chunk subtends a large solid angle -- zoomed in close,
        // near-clip range -- a chunk that is genuinely only partially front-facing (e.g. a curved surface
        // near a silhouette) can get misclassified as fully backfacing by this single-sample test and
        // dropped entirely, producing a visible hole with nothing else covering it. This is especially
        // visible during LOD transitions when pulling the camera back: coarser parent chunks have larger
        // boundingRadius and wider (less precise) normal cones, so they are exactly the chunks most prone
        // to this misclassification right as they're being pulled in to replace finer detail. Skip the
        // cone test at close range and fall back to the (already conservative) AABB frustum result.
        if (isVisible && g_EnableConeCulling == 1 && chunk.coneCutoff > -0.99)
        {
            float3 eyePos = (g_UseDetachedCullCam == 1) ? g_CullEyePos : g_ViewerEyePos;
            float3 toChunk = chunk.center - eyePos;
            float dist = length(toChunk);
            if (dist > max(1e-4, chunk.boundingRadius * 3.0))
            {
                float3 viewDir = toChunk / dist; // Ray from camera towards chunk center
                float sinCone = sqrt(max(0.0, 1.0 - chunk.coneCutoff * chunk.coneCutoff));
                float nDotV = dot(chunk.coneAxis, viewDir);
                // When cluster normal cone points in direction of view ray, cluster is backfacing
                if (nDotV > sinCone + 0.02)
                {
                    isVisible = false;
                }
            }
        }

        // Show ONLY Locked Chunks (Transition or Edge)
        if (isVisible && g_ShowOnlyLocked == 1)
        {
            bool isLocked = (chunk.isSilhouette > 0.001) || (abs(chunk.blendWeight) > 0.001 && abs(chunk.blendWeight) < 0.999);
            if (!isLocked)
            {
                isVisible = false;
            }
        }
    }

    uint visibleOffset = WavePrefixCountBits(isVisible);
    uint totalVisible = WaveActiveCountBits(isVisible);

    if (isVisible && visibleOffset < AS_GROUP_SIZE)
    {
        s_Payload.chunkIndices[visibleOffset] = chunkIdx;
    }

    GroupMemoryBarrierWithGroupSync();

    if (threadId == 0)
    {
        s_Payload.flatGroupIndex = 0;
        DispatchMesh(min(totalVisible, (uint)AS_GROUP_SIZE), 1, 1, s_Payload);
    }
}

// =========================================================================
// Mesh Shader Stage (mainMS)
// =========================================================================

[NumThreads(SURFELS_PER_GROUP, 1, 1)]
[OutputTopology("triangle")]
void mainMS(
    uint3 groupId  : SV_GroupID,
    uint  threadId : SV_GroupThreadID,
    in payload   ChunkPayload payload,
    out indices  uint3 tris[SURFELS_PER_GROUP * 2],
    out vertices VSOut verts[SURFELS_PER_GROUP * 4])
{
    uint surfelIndex = 0;
    uint groupSurfelCount = 0;

    float chunkBlendWeight = 1.0;
    float chunkDilationMorph = 0.0;
    float chunkIsSilhouette = 0.0;
    if (g_UseChunkedPipeline == 1)
    {
        uint pIdx = min(groupId.x, (uint)(AS_GROUP_SIZE - 1));
        uint chunkIdx = payload.chunkIndices[pIdx];
        if (chunkIdx >= g_TotalChunks)
        {
            SetMeshOutputCounts(0, 0);
            return;
        }
        MeshletChunk chunk = g_ChunkBuffer[chunkIdx];
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, chunk.surfelCount);
        surfelIndex = chunk.surfelOffset + threadId;
        chunkBlendWeight = chunk.blendWeight;
        chunkDilationMorph = chunk.dilationMorph;
        chunkIsSilhouette = chunk.isSilhouette;
    }
    else
    {
        uint groupBase = payload.flatGroupIndex * SURFELS_PER_GROUP;
        uint remaining = groupBase < g_SurfelCount ? g_SurfelCount - groupBase : 0;
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, remaining);
        surfelIndex = groupBase + threadId;
    }

    if (g_ShowOnlyLocked == 1 && g_UseChunkedPipeline == 1)
    {
        bool isLocked = (chunkIsSilhouette > 0.001) || (abs(chunkBlendWeight) > 0.001 && abs(chunkBlendWeight) < 0.999);
        if (!isLocked)
        {
            SetMeshOutputCounts(0, 0);
            return;
        }
    }

    SetMeshOutputCounts(groupSurfelCount * 4, groupSurfelCount * 2);

    if (threadId >= groupSurfelCount || surfelIndex >= g_SurfelCount)
        return;
    float3 worldPos;
    float3 normal;
    float3 color;
    float3 tangentX, tangentY;

    if (g_RenderMode == 0)
    {
        // 1. Procedural Fibonacci Sphere
        float3 dir = FibonacciSpherePoint(surfelIndex, g_SurfelCount);
        float pulse = 0.02 * sin(g_Time * 2.0 + (float)surfelIndex);
        worldPos = dir * (g_SphereRadius + pulse);
        normal = dir;
        color = HashColor(surfelIndex);

        tangentX = g_CamRight * (g_Radius * 0.05);
        tangentY = g_CamUp * (g_Radius * 0.05);
    }
    else if (g_RenderMode == 1)
    {
        // 2. Quantized 8-Byte Surfels (PackedSurfel)
        PackedSurfel s = g_SurfelBuffer[surfelIndex];

        // Unpack 10:10:10:2 position
        uint qx = s.packedPosRadius & 0x3FF;
        uint qy = (s.packedPosRadius >> 10) & 0x3FF;
        uint qz = (s.packedPosRadius >> 20) & 0x3FF;
        uint re = (s.packedPosRadius >> 30) & 0x3;

        // Dynamic dataset AABB bounds
        worldPos = g_AABBMin + float3(qx / 1023.0, qy / 1023.0, qz / 1023.0) * g_AABBExtents;
        normal = UnpackNormalOct16(s.packedNormalColor & 0xFFFF);
        color = UnpackColorRGB565((s.packedNormalColor >> 16) & 0xFFFF);

        static const float s_radScales[4] = { 1.0f, 1.5f, 2.5f, 4.5f };
        float maxExtent = max(g_AABBExtents.x, max(g_AABBExtents.y, g_AABBExtents.z));
        float baseVoxelRadius = max(0.0005f, (maxExtent / 1024.0f) * 1.35f);
        float splatRadius = g_Radius * baseVoxelRadius * s_radScales[re];

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.00015f;
        splatRadius = max(splatRadius, minCoverageRadius);

        if (g_OrientMode == 0 && abs(normal.x) + abs(normal.y) + abs(normal.z) > 0.1f)
        {
            float3 up = abs(normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 tX = normalize(cross(up, normal));
            float3 tY = cross(normal, tX);
            tangentX = tX * splatRadius;
            tangentY = tY * splatRadius;
        }
        else
        {
            tangentX = g_CamRight * splatRadius;
            tangentY = g_CamUp * splatRadius;
        }
    }
    else if (g_RenderMode == 2)
    {
        // 3. Raw Direct Ingest / Float32 (Clean Tangent Surface Surfels)
        RawSurfel s = g_RawSurfelBuffer[surfelIndex];
        worldPos = s.position;
        normal = s.normal;
        color = s.color;

        float maxExtent = max(g_AABBExtents.x, max(g_AABBExtents.y, g_AABBExtents.z));
        float defaultRadius = max(0.0005f, (maxExtent / 1024.0f) * 1.35f);
        float baseRadius = (s.radius > 0.00001f) ? s.radius : defaultRadius;
        float splatRadius = baseRadius * g_Radius;

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.00015f;
        splatRadius = max(splatRadius, minCoverageRadius);

        if (g_OrientMode == 0 && abs(normal.x) + abs(normal.y) + abs(normal.z) > 0.1f)
        {
            float3 up = abs(normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 tX = normalize(cross(up, normal));
            float3 tY = cross(normal, tX);
            tangentX = tX * splatRadius;
            tangentY = tY * splatRadius;
        }
        else
        {
            tangentX = g_CamRight * splatRadius;
            tangentY = g_CamUp * splatRadius;
        }
    }

    // When Camera is Detached: Render the model as a hollow plaster mold shell with dark AO interior
    if (g_UseDetachedCullCam == 1)
    {
        // 1. Frustum Culling against frozen detached camera
        float4 cullClip = mul(g_CullViewProj, float4(worldPos, 1.0));
        bool outsideFrustum = (cullClip.w <= 0.0001) ||
                              (cullClip.x < -cullClip.w) || (cullClip.x > cullClip.w) ||
                              (cullClip.y < -cullClip.w) || (cullClip.y > cullClip.w) ||
                              (cullClip.z < 0.0) || (cullClip.z > cullClip.w);

        // 2. Normal Culling wrt Detached Camera:
        // Only keep the front-facing shell visible to the original detached camera position
        float3 toCullCam = g_CullEyePos - worldPos;
        float distCull = length(toCullCam);
        float3 normCullDir = distCull > 1e-4 ? (toCullCam / distCull) : float3(0, 0, 1);
        float nDotCull = dot(normal, normCullDir);
        bool isBackFacingToDetached = (dot(normal, normal) > 0.1) && (nDotCull < -0.05);

        if (outsideFrustum || isBackFacingToDetached)
        {
            uint pBase = threadId * 2;
            tris[pBase + 0] = uint3(0, 0, 0);
            tris[pBase + 1] = uint3(0, 0, 0);

            uint vBase = threadId * 4;
            [unroll]
            for (uint c = 0; c < 4; c++)
            {
                VSOut o;
                o.pos = float4(0.0, 0.0, 0.0, 0.0);
                o.uv = float2(0.0, 0.0);
                o.color = float3(0.0, 0.0, 0.0);
                o.norm = float3(0.0, 0.0, 0.0);
                o.blendWeight = 0.0;
                verts[vBase + c] = o;
            }
            return;
        }
        // 3. Hollow Mold Interior Shading when camera is detached
        if (dot(normal, normal) > 0.1)
        {
            float3 toViewer = g_ViewerEyePos - worldPos;
            float distViewer = length(toViewer);
            float3 normViewerDir = distViewer > 1e-4 ? (toViewer / distViewer) : float3(0, 0, 1);
            float nDotViewer = dot(normal, normViewerDir);

            if (nDotViewer < -0.06)
            {
                float innerFade = saturate((-nDotViewer - 0.06) / 0.5);
                float3 darkInterior = float3(0.08, 0.07, 0.09);
                color = lerp(color * 0.45, darkInterior, innerFade * 0.85);
            }
        }
    }

    // Smooth Geometric Dilation Morph along silhouette normals
    float splatRad = length(tangentX);
    if (splatRad < 1e-6) splatRad = 0.02;

    if (chunkDilationMorph > 0.0001 && abs(chunkBlendWeight) < 0.999)
    {
        float w = (chunkBlendWeight >= 0.0) ? (1.0 - chunkBlendWeight) * chunkDilationMorph : (-chunkBlendWeight) * chunkDilationMorph;
        worldPos += normal * (splatRad * w * 1.5);
        tangentX *= (1.0 + w * 0.4);
        tangentY *= (1.0 + w * 0.4);
    }

    // Quad corners in local 2D tangent space: 0(-1,-1) 1(1,-1) 2(-1,1) 3(1,1)
    float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };

    // Two-sided surface directional lighting computed once per surfel
    float3 norm = (dot(normal, normal) > 0.01) ? normalize(normal) : float3(0.0, 1.0, 0.0);
    float3 lightDir = normalize(float3(0.5, 0.8, 0.6));
    float ndl = abs(dot(norm, lightDir));
    float lighting = (dot(normal, normal) > 0.01) ? (0.35 + 0.65 * ndl) : 1.0;
    float3 litColor = color * lighting;

    // Show Chunk Stream & Edge Highlighting
    if (chunkIsSilhouette > 0.001)
    {
        float w = saturate(chunkIsSilhouette);
        if (w >= 0.99)
        {
            // Pure Silhouette Edge Chunks: Crisp Lavender Outline (ONLY when Highlight Edge Chunks toggle is ON)
            if (g_HighlightSilhouette == 1)
            {
                float3 hotLavender = float3(0.85, 0.55, 0.98);
                litColor = lerp(litColor, hotLavender, 0.90);
            }
        }
        else if (g_ShowChunkStream == 1)
        {
            // Wave Sweep: 10% Orange Tint Opacity
            float trailFactor = saturate(w / 0.95);
            float3 orangeTint = float3(1.0, 0.55, 0.1);
            litColor = lerp(litColor, orangeTint * lighting, 0.10 * trailFactor);
        }
    }

    uint vBase = threadId * 4;
    [unroll]
    for (uint c = 0; c < 4; c++)
    {
        float3 offset = tangentX * corners[c].x + tangentY * corners[c].y;

        VSOut o;
        o.pos = mul(g_ViewProj, float4(worldPos + offset, 1.0));
        o.uv = corners[c] * 0.5 + 0.5;
        o.color = litColor;
        o.norm = normal;
        o.blendWeight = chunkBlendWeight;
        o.isSil = (g_HighlightSilhouette == 1) ? chunkIsSilhouette : 0.0;
        verts[vBase + c] = o;
    }

    uint pBase = threadId * 2;
    tris[pBase + 0] = uint3(vBase + 0, vBase + 1, vBase + 2);
    tris[pBase + 1] = uint3(vBase + 1, vBase + 3, vBase + 2);
}

// =========================================================================
// Screen-Space Bayer Matrix Stochastic Dithering
// =========================================================================

float GetBayer8x8(uint2 pixelPos)
{
    static const float bayer8x8[8][8] = {
        {  1.0/64.0, 49.0/64.0, 13.0/64.0, 61.0/64.0,  4.0/64.0, 52.0/64.0, 16.0/64.0, 64.0/64.0 },
        { 33.0/64.0, 17.0/64.0, 45.0/64.0, 29.0/64.0, 36.0/64.0, 20.0/64.0, 48.0/64.0, 32.0/64.0 },
        {  9.0/64.0, 57.0/64.0,  5.0/64.0, 53.0/64.0, 12.0/64.0, 60.0/64.0,  8.0/64.0, 56.0/64.0 },
        { 41.0/64.0, 25.0/64.0, 37.0/64.0, 21.0/64.0, 44.0/64.0, 28.0/64.0, 40.0/64.0, 24.0/64.0 },
        {  3.0/64.0, 51.0/64.0, 15.0/64.0, 63.0/64.0,  2.0/64.0, 50.0/64.0, 14.0/64.0, 62.0/64.0 },
        { 35.0/64.0, 19.0/64.0, 47.0/64.0, 31.0/64.0, 34.0/64.0, 18.0/64.0, 46.0/64.0, 30.0/64.0 },
        { 11.0/64.0, 59.0/64.0,  7.0/64.0, 55.0/64.0, 10.0/64.0, 58.0/64.0,  6.0/64.0, 54.0/64.0 },
        { 43.0/64.0, 27.0/64.0, 39.0/64.0, 23.0/64.0, 42.0/64.0, 26.0/64.0, 38.0/64.0, 22.0/64.0 }
    };
    return bayer8x8[pixelPos.y & 7][pixelPos.x & 7];
}

// =========================================================================
// Pixel Shader Stage (mainPS)
// =========================================================================

float4 mainPS(VSOut i) : SV_Target
{
    float2 centered = i.uv * 2.0 - 1.0;
    float d = dot(centered, centered);
    if (d > 1.0)
        discard;

    // True Screen-Space Bayer Matrix Stochastic Dithering:
    // Performs an exact complementary stochastic cross-dissolve between parent and child chunks in screen space.
    if (g_EnableDithering == 1 && abs(i.blendWeight) < 0.999)
    {
        uint2 screenPixel = (uint2)i.pos.xy;
        float bayerThreshold = GetBayer8x8(screenPixel);

        if (i.blendWeight >= 0.0)
        {
            // Children fading IN (+t): visible on [0, t) -> discard if bayerThreshold >= t
            if (bayerThreshold >= i.blendWeight)
            {
                discard;
            }
        }
        else
        {
            // Parent fading OUT (-t): complementary on [t, 1) -> discard if bayerThreshold < t
            float t = -i.blendWeight;
            if (bayerThreshold < t)
            {
                discard;
            }
        }
    }

    // Continuous 3D Gaussian falloff:
    // With back-to-front depth sorting, overlapping splats melt together into continuous, silky-smooth marble.
    float alpha = saturate(exp(-2.5 * d) * 0.90);

    return float4(i.color * alpha, alpha);
}

// =========================================================================
// GPU Item Prepass Stage (itemMS & itemPS) for Silhouette Edge Inversion
// =========================================================================

struct ItemVSOut
{
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    nointerpolation uint chunkId : CHUNKID0;
};

[outputtopology("triangle")]
[numthreads(SURFELS_PER_GROUP, 1, 1)]
void itemMS(
    in uint threadId : SV_GroupIndex,
    in uint3 groupId : SV_GroupID,
    out vertices ItemVSOut verts[SURFELS_PER_GROUP * 4],
    out indices uint3 tris[SURFELS_PER_GROUP * 2]
)
{
    uint groupSurfelCount = SURFELS_PER_GROUP;
    uint surfelIndex = 0;
    uint chunkIndex = groupId.y * 32768 + groupId.x;

    if (g_UseChunkedPipeline == 1)
    {
        if (chunkIndex >= g_TotalChunks)
        {
            SetMeshOutputCounts(0, 0);
            return;
        }
        MeshletChunk c = g_ChunkBuffer[chunkIndex];

        // Fast Normal Cone Backface Culling in itemMS
        // See the matching guard in mainAS: this single-sample test misclassifies partially front-facing
        // chunks as fully backfacing once the viewer is close enough for the chunk to subtend a large
        // solid angle, so it is skipped at close range.
        if (g_EnableConeCulling == 1 && c.coneCutoff > -0.99)
        {
            float3 toChunk = c.center - g_ViewerEyePos;
            float dist = length(toChunk);
            if (dist > max(1e-4, c.boundingRadius * 3.0))
            {
                float3 viewDir = toChunk / dist;
                float sinCone = sqrt(max(0.0, 1.0 - c.coneCutoff * c.coneCutoff));
                if (dot(c.coneAxis, viewDir) > sinCone + 0.02)
                {
                    SetMeshOutputCounts(0, 0);
                    return;
                }
            }
        }

        groupSurfelCount = min((uint)SURFELS_PER_GROUP, c.surfelCount);
        surfelIndex = c.surfelOffset + threadId;
    }
    else
    {
        uint groupBase = chunkIndex * SURFELS_PER_GROUP;
        if (groupBase >= g_SurfelCount)
        {
            SetMeshOutputCounts(0, 0);
            return;
        }
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, g_SurfelCount - groupBase);
        surfelIndex = groupBase + threadId;
        chunkIndex = 0;
    }

    SetMeshOutputCounts(groupSurfelCount * 4, groupSurfelCount * 2);

    if (threadId >= groupSurfelCount || surfelIndex >= g_SurfelCount)
        return;

    float3 worldPos;
    float3 normal;
    float3 tangentX, tangentY;

    if (g_RenderMode == 1)
    {
        PackedSurfel s = g_SurfelBuffer[surfelIndex];
        uint qx = s.packedPosRadius & 0x3FF;
        uint qy = (s.packedPosRadius >> 10) & 0x3FF;
        uint qz = (s.packedPosRadius >> 20) & 0x3FF;
        uint re = (s.packedPosRadius >> 30) & 0x3;

        worldPos = g_AABBMin + float3(qx / 1023.0, qy / 1023.0, qz / 1023.0) * g_AABBExtents;
        normal = UnpackNormalOct16(s.packedNormalColor & 0xFFFF);

        static const float s_radScales[4] = { 1.0f, 1.5f, 2.5f, 4.5f };
        float maxExtent = max(g_AABBExtents.x, max(g_AABBExtents.y, g_AABBExtents.z));
        float baseVoxelRadius = max(0.0005f, (maxExtent / 1024.0f) * 1.35f);
        float splatRadius = g_Radius * baseVoxelRadius * s_radScales[re];

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.00015f;
        splatRadius = max(splatRadius, minCoverageRadius);

        if (g_OrientMode == 0 && abs(normal.x) + abs(normal.y) + abs(normal.z) > 0.1f)
        {
            float3 up = abs(normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 tX = normalize(cross(up, normal));
            float3 tY = cross(normal, tX);
            tangentX = tX * splatRadius;
            tangentY = tY * splatRadius;
        }
        else
        {
            tangentX = g_CamRight * splatRadius;
            tangentY = g_CamUp * splatRadius;
        }
    }
    else if (g_RenderMode == 2)
    {
        RawSurfel s = g_RawSurfelBuffer[surfelIndex];
        worldPos = s.position;
        normal = s.normal;
        float splatRadius = max(0.001f, s.radius * g_Radius);
        if (g_OrientMode == 0 && dot(normal, normal) > 0.1f)
        {
            float3 up = abs(normal.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 tX = normalize(cross(up, normal));
            float3 tY = cross(normal, tX);
            tangentX = tX * splatRadius;
            tangentY = tY * splatRadius;
        }
        else
        {
            tangentX = g_CamRight * splatRadius;
            tangentY = g_CamUp * splatRadius;
        }
    }
    else
    {
        float3 dir = FibonacciSpherePoint(surfelIndex, g_SurfelCount);
        worldPos = dir * g_SphereRadius;
        normal = dir;
        tangentX = g_CamRight * (g_Radius * 0.05);
        tangentY = g_CamUp * (g_Radius * 0.05);
    }

    float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };

    uint vBase = threadId * 4;
    [unroll]
    for (uint c = 0; c < 4; c++)
    {
        float3 offset = tangentX * corners[c].x + tangentY * corners[c].y;

        ItemVSOut o;
        o.pos = mul(g_ViewProj, float4(worldPos + offset, 1.0));
        o.uv = corners[c] * 0.5 + 0.5;
        o.chunkId = chunkIndex + 1; // 1-based chunk ID so 0 is background
        verts[vBase + c] = o;
    }

    uint pBase = threadId * 2;
    tris[pBase + 0] = uint3(vBase + 0, vBase + 1, vBase + 2);
    tris[pBase + 1] = uint3(vBase + 1, vBase + 3, vBase + 2);
}

uint itemPS(ItemVSOut i) : SV_Target0
{
    float2 centered = i.uv * 2.0 - 1.0;
    if (dot(centered, centered) > 1.0)
        discard;
    return i.chunkId;
}

// =========================================================================
// Interior Occlusion Volume Stage (occluderMS & occluderPS)
//
// Solid depth-writing cubes baked at preprocessing time (see PreprocessApp::
// BuildOcclusionVolume) so the splat pass can depth-test against them and discard
// far-side surfels visible through gaps in a sparse near side. One threadgroup
// per voxel, direct-dispatched with no amplification shader stage (like itemMS).
//
// Only the cube's EXPOSED faces (those bordering a non-cube cell, per the mask baked
// into packedColor) are emitted -- faces shared with a neighbouring cube are never
// visible. The live shrink (g_OcclusionShrinkCells) shaves the outer skin off the
// whole volume by pulling every exposed face inward by that many cells while leaving
// shared faces untouched, so the solid shrinks as one watertight body instead of
// each cube shrinking about its own centre and opening seams between neighbours. A
// cube whose opposing exposed faces would cross (a one-cell-thick shell shaved from
// both sides) is clamped to a thin slab through the cell centre rather than removed,
// so hollow/open scans keep a closed occluder shell at any shrink setting.
// =========================================================================

struct OccluderVSOut
{
    float4 pos   : SV_POSITION;
    float3 color : COLOR0;
    float3 norm  : NORMAL0;
};

static const float3 s_occluderCubeCorners[8] = {
    float3(-1,-1,-1), float3( 1,-1,-1), float3(-1, 1,-1), float3( 1, 1,-1),
    float3(-1,-1, 1), float3( 1,-1, 1), float3(-1, 1, 1), float3( 1, 1, 1)
};

// Index quads (per face) into s_occluderCubeCorners. Winding is not load-bearing since every PSO in
// this file rasterizes with CullMode = NONE. Face order here defines the bit order of the exposed-face
// mask in OcclusionVoxel::packedColor, so keep it in sync with PreprocessApp::BuildOcclusionVolume.
static const uint s_occluderFaceIdx[6][4] = {
    { 0, 1, 2, 3 }, // -Z
    { 5, 4, 7, 6 }, // +Z
    { 4, 0, 6, 2 }, // -X
    { 1, 5, 3, 7 }, // +X
    { 4, 5, 0, 1 }, // -Y
    { 2, 3, 6, 7 }  // +Y
};

static const float3 s_occluderFaceNormal[6] = {
    float3(0,0,-1), float3(0,0,1), float3(-1,0,0), float3(1,0,0), float3(0,-1,0), float3(0,1,0)
};

static const uint  OCCLUDER_FACE_MASK_SHIFT = 16;
static const uint  OCCLUDER_FACE_MASK_ALL   = 0x3F;
static const float OCCLUDER_MIN_HALF_EXTENT = 0.05; // In halfSize units: thinnest slab a shaved cube clamps to (5% of a cell)

[outputtopology("triangle")]
[numthreads(24, 1, 1)]
void occluderMS(
    in uint threadId : SV_GroupIndex,
    in uint3 groupId : SV_GroupID,
    out vertices OccluderVSOut verts[24],
    out indices uint3 tris[12]
)
{
    // DXIL validation requires exactly one SetMeshOutputCounts call per invocation, so work out the
    // final face count first (0 for an out-of-range index or a cube shaved away entirely), set it once,
    // and only then bail out.
    uint voxelIndex = groupId.y * 32768 + groupId.x;
    bool valid = voxelIndex < g_OcclusionVoxelCount;

    OcclusionVoxel v = g_OcclusionVoxelBuffer[valid ? voxelIndex : 0];
    uint exposed = (v.packedColor >> OCCLUDER_FACE_MASK_SHIFT) & OCCLUDER_FACE_MASK_ALL;
    if (exposed == 0) exposed = OCCLUDER_FACE_MASK_ALL; // Legacy file baked before the mask existed

    // Shave: pull each exposed face inward. Per axis, the low/high extents (in units of halfSize).
    // Mask bits: 0:-Z 1:+Z 2:-X 3:+X 4:-Y 5:+Y  -> lo/hi per axis in (x, y, z) order.
    float inset = max(0.0, g_OcclusionShrinkCells) * 2.0; // cells -> halfSize units (1 cell = 2 halfSizes)
    float3 lo = float3(-1, -1, -1) + float3((exposed >> 2) & 1, (exposed >> 4) & 1, (exposed >> 0) & 1) * inset;
    float3 hi = float3( 1,  1,  1) - float3((exposed >> 3) & 1, (exposed >> 5) & 1, (exposed >> 1) & 1) * inset;

    // Opposing exposed faces that would cross: clamp to a thin slab about their midpoint instead.
    float3 mid = (lo + hi) * 0.5;
    bool3 crossed = lo + OCCLUDER_MIN_HALF_EXTENT * 2.0 > hi;
    lo = crossed ? mid - OCCLUDER_MIN_HALF_EXTENT : lo;
    hi = crossed ? mid + OCCLUDER_MIN_HALF_EXTENT : hi;

    uint faceCount = valid ? countbits(exposed) : 0;
    SetMeshOutputCounts(faceCount * 4, faceCount * 2);
    if (faceCount == 0)
        return;

    // Map this thread's output face slot (0..faceCount-1) to the n-th set bit of the mask.
    uint slot = threadId / 4;
    uint face = 0;
    if (slot < faceCount)
    {
        uint remaining = exposed;
        for (uint n = 0; n < slot; n++)
            remaining &= remaining - 1; // Clear lowest set bit
        face = firstbitlow(remaining);
    }

    float3 color = UnpackColorRGB565(v.packedColor);

    if (slot < faceCount)
    {
        uint corner = threadId % 4;
        float3 c = s_occluderCubeCorners[s_occluderFaceIdx[face][corner]];
        float3 ext = float3(c.x < 0 ? lo.x : hi.x, c.y < 0 ? lo.y : hi.y, c.z < 0 ? lo.z : hi.z);
        float3 worldPos = v.center + ext * v.halfSize;

        OccluderVSOut o;
        o.pos = mul(g_ViewProj, float4(worldPos, 1.0));
        o.color = color;
        o.norm = s_occluderFaceNormal[face];
        verts[threadId] = o;
    }

    if (threadId < faceCount * 2)
    {
        uint triSlot = threadId / 2;
        uint vBase = triSlot * 4;
        tris[threadId] = (threadId % 2 == 0)
            ? uint3(vBase + 0, vBase + 1, vBase + 2)
            : uint3(vBase + 1, vBase + 3, vBase + 2);
    }
}

float4 occluderPS(OccluderVSOut i) : SV_Target0
{
    float3 lightDir = normalize(float3(0.5, 0.8, 0.6));
    float ndl = abs(dot(normalize(i.norm), lightDir));
    float lighting = 0.35 + 0.65 * ndl;
    return float4(i.color * lighting, 1.0);
}

