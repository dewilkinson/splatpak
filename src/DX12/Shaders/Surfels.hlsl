// Surfels.hlsl - Modern DirectX 12 Mesh Shader Wavelet & Procedural Surfel Renderer
//
// Supports both:
//  1. Procedural Fibonacci-sphere point-splat generation on-chip.
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
};

StructuredBuffer<PackedSurfel>  g_SurfelBuffer       : register(t0);
StructuredBuffer<RawSurfel>     g_RawSurfelBuffer    : register(t1);
StructuredBuffer<MeshletChunk>  g_ChunkBuffer        : register(t2);
StructuredBuffer<uint>          g_SortedChunkIndices : register(t3);

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
    float    g_Pad3;
};

struct ChunkPayload
{
    uint chunkIndices[AS_GROUP_SIZE];
    uint flatGroupIndex;
};

struct VSOut
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float3 color : COLOR0;
    float3 norm  : NORMAL0;
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
            ChunkPayload payload;
            payload.flatGroupIndex = groupId.x;
            if (groupBase < g_SurfelCount)
            {
                DispatchMesh(1, 1, 1, payload);
            }
            else
            {
                DispatchMesh(0, 1, 1, payload);
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

        bool allOutLeft = true, allOutRight = true;
        bool allOutBottom = true, allOutTop = true;
        bool allOutNear = true, allOutFar = true;

        [unroll]
        for (int i = 0; i < 8; i++)
        {
            float4 c = mul(cullMatrix, float4(corners[i], 1.0));
            if (c.x >= -c.w) allOutLeft = false;
            if (c.x <=  c.w) allOutRight = false;
            if (c.y >= -c.w) allOutBottom = false;
            if (c.y <=  c.w) allOutTop = false;
            if (c.z >=  0.0) allOutNear = false;
            if (c.z <=  c.w) allOutFar = false;
        }

        isVisible = !(allOutLeft || allOutRight || allOutBottom || allOutTop || allOutNear || allOutFar);
    }

    uint visibleOffset = WavePrefixCountBits(isVisible);
    uint totalVisible = WaveActiveCountBits(isVisible);

    ChunkPayload payload;
    payload.flatGroupIndex = 0;
    if (isVisible)
    {
        payload.chunkIndices[visibleOffset] = chunkIdx;
    }

    DispatchMesh(totalVisible, 1, 1, payload);
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

    if (g_UseChunkedPipeline == 1)
    {
        uint chunkIdx = payload.chunkIndices[groupId.x];
        MeshletChunk chunk = g_ChunkBuffer[chunkIdx];
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, chunk.surfelCount);
        surfelIndex = chunk.surfelOffset + threadId;
    }
    else
    {
        uint groupBase = payload.flatGroupIndex * SURFELS_PER_GROUP;
        uint remaining = groupBase < g_SurfelCount ? g_SurfelCount - groupBase : 0;
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, remaining);
        surfelIndex = groupBase + threadId;
    }

    SetMeshOutputCounts(groupSurfelCount * 4, groupSurfelCount * 2);

    if (threadId >= groupSurfelCount)
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

        tangentX = g_CamRight * g_Radius;
        tangentY = g_CamUp * g_Radius;
    }
    else if (g_RenderMode == 1)
    {
        // 2. Streamed Wavelet Surfel from StructuredBuffer (Quantized 8-byte)
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

        static const float s_radScales[4] = { 0.2f, 0.4f, 0.8f, 1.5f };
        float splatRadius = g_Radius * s_radScales[re] * 0.15f;

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.0003f;
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

        float baseRadius = (s.radius > 0.00001f) ? s.radius : 0.02f;
        float splatRadius = baseRadius * g_Radius;

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.0003f;
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
                verts[vBase + c] = o;
            }
            return;
        }

        // 3. Hollow Mold Interior & Rim Edge Shading from Active Viewer Perspective:
        if (dot(normal, normal) > 0.1)
        {
            float3 toViewer = g_ViewerEyePos - worldPos;
            float distViewer = length(toViewer);
            float3 normViewerDir = distViewer > 1e-4 ? (toViewer / distViewer) : float3(0, 0, 1);
            float nDotViewer = dot(normal, normViewerDir);

            if (nDotViewer < -0.06)
            {
                // Interior cavity of the shell: Dark ambient occlusion, no albedo/texture
                float depthFactor = saturate(-nDotViewer);
                float cavityAO = 0.08 + 0.12 * (1.0 - depthFactor);
                color = float3(0.12, 0.13, 0.16) * (cavityAO * 4.5);
            }
            else if (abs(nDotViewer) <= 0.06)
            {
                // Rim Line: Crisp transition where the front-facing shell turns away into the dark interior
                float rimStrength = 1.0 - (abs(nDotViewer) / 0.06);
                float3 rimHighlight = float3(0.85, 0.90, 1.0);
                color = lerp(color, rimHighlight, rimStrength * 0.92);
            }
        }
    }

    // Quad corners in local 2D tangent space: 0(-1,-1) 1(1,-1) 2(-1,1) 3(1,1)
    float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };

    uint vBase = threadId * 4;
    [unroll]
    for (uint c = 0; c < 4; c++)
    {
        float3 offset = tangentX * corners[c].x + tangentY * corners[c].y;

        VSOut o;
        o.pos = mul(g_ViewProj, float4(worldPos + offset, 1.0));
        o.uv = corners[c] * 0.5 + 0.5;
        o.color = color;
        o.norm = normal;
        verts[vBase + c] = o;
    }

    uint pBase = threadId * 2;
    tris[pBase + 0] = uint3(vBase + 0, vBase + 1, vBase + 2);
    tris[pBase + 1] = uint3(vBase + 1, vBase + 3, vBase + 2);
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

    // Continuous 3D Gaussian falloff:
    // With back-to-front depth sorting, overlapping splats melt together into continuous, silky-smooth marble.
    float alpha = saturate(exp(-2.5 * d) * 0.90);

    return float4(i.color * alpha, alpha);
}
