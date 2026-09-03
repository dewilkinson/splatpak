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
    float  blendWeight;
    uint   lodLevel;
    float  dilationMorph; // Morph dilation factor for silhouette reconstruction
    float  isSilhouette;  // 1.0 if silhouette chunk, 0.0 otherwise
    float3 coneAxis;      // Average unit normal vector of cluster
    float  coneCutoff;    // cos(theta_max) of cluster normal cone (-1.0 = disabled)
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
    uint     g_EnableDithering;
    uint     g_HighlightSilhouette;
    uint     g_EnableConeCulling;
    float    g_PadCB;
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
        if (isVisible && g_EnableConeCulling == 1 && chunk.coneCutoff > -0.99)
        {
            float3 eyePos = (g_UseDetachedCullCam == 1) ? g_CullEyePos : g_ViewerEyePos;
            float3 toChunk = chunk.center - eyePos;
            float dist = length(toChunk);
            if (dist > 1e-4)
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
    }

    uint visibleOffset = WavePrefixCountBits(isVisible);
    uint totalVisible = WaveActiveCountBits(isVisible);

    if (isVisible)
    {
        s_Payload.chunkIndices[visibleOffset] = chunkIdx;
    }

    GroupMemoryBarrierWithGroupSync();

    if (threadId == 0)
    {
        s_Payload.flatGroupIndex = 0;
        DispatchMesh(totalVisible, 1, 1, s_Payload);
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
        uint chunkIdx = payload.chunkIndices[groupId.x];
        MeshletChunk chunk = g_ChunkBuffer[chunkIdx];
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, chunk.surfelCount);
        surfelIndex = chunk.surfelOffset + threadId;
        chunkBlendWeight = (abs(chunk.blendWeight) > 0.001f) ? chunk.blendWeight : 1.0f;
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

    // Show Chunk Stream - Lavender Wavefront & Retained Alpha Tint Dissipation
    if (g_HighlightSilhouette == 1 && chunkIsSilhouette > 0.001)
    {
        float w = saturate(chunkIsSilhouette);
        if (w >= 0.65)
        {
            // 1. Advancing Leading Edge (Vibrant Lavender Wave Crest)
            float leadFactor = saturate((w - 0.65) / 0.35);
            float3 hotLavender = float3(0.85, 0.55, 0.98);
            litColor = lerp(litColor, hotLavender, 0.85 + leadFactor * 0.15);
        }
        else
        {
            // 2. Trailing Wake of Retained Alpha Tint that Dissolves and Blends Over Time
            float trailFactor = saturate(w / 0.65);
            float3 trailLavender = float3(0.78, 0.48, 0.92);
            litColor = lerp(litColor, trailLavender * lighting, trailFactor * 0.65);
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
