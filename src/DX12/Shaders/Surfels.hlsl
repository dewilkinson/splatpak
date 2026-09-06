// Surfels.hlsl
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The core splat pipeline. On hardware with D3D12 Mesh Shader Tier 1 every surfel is emitted
// procedurally by an amplification/mesh shader pair each frame, straight from a
// StructuredBuffer -- there is no vertex/index buffer anywhere in that path. Hardware without
// mesh shaders runs the same stages as instanced vertex shaders (one instance per surfel, six
// vertices per instance, DrawInstanced with no input layout), compiled for Shader Model 6.0
// where DXIL is available and through the legacy compiler as Shader Model 5.1 where it is not
// (define SURFELS_NO_MESH_SHADERS to hide the mesh-shader entry points from those compilers).
// Both paths share the per-surfel decode, culling and shading code below, so they draw the same
// picture; the fallback simply spends more vertex work (each corner re-decodes its surfel) and
// leaves chunk frustum culling to the rasterizer.
//
// Also home to the GPU silhouette item-prepass (itemMS/itemVS + itemPS) and the interior
// occlusion volume pass (occluderMS/occluderVS + occluderPS).
//
// Three render modes share the main splat stage:
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
    float  halfSize;    // Half of (cellSize * 2^level): blocks are grid-aligned to their own size, so neighbours tile exactly
    uint   packedColor; // Bits 0..15: RGB565 (see UnpackColorRGB565); bits 16..21: exposed-face mask,
                        // one bit per face in s_occluderFaceIdx order (-Z,+Z,-X,+X,-Y,+Y); bit 22: mask
                        // valid (clear on legacy files, which are treated as all faces exposed). See
                        // OcclusionVoxelGPU in WaveletTypes.h.
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
    uint     g_ShowOcclusionVolumeOnly;
    uint     g_OcclusionVoxelCount; // Blocks in the occlusion volume mip drawn this frame
    uint     g_OcclusionVoxelFirst; // Index of that mip's first block in g_OcclusionVoxelBuffer
    float    g_ArrivalGlowIntensity; // Refinement visualizer strength (1 = default)
    float    g_ArrivalGlowHue;       // Refinement visualizer hue rotation, degrees (0 = orange)
};

// Hue/saturation/value to RGB, hue in degrees (wraps).
float3 HsvToRgb(float h, float s, float v)
{
    float hp = fmod(fmod(h, 360.0) + 360.0, 360.0) / 60.0;
    float c = v * s;
    float x = c * (1.0 - abs(fmod(hp, 2.0) - 1.0));
    float3 rgb = (hp < 1.0) ? float3(c, x, 0) : (hp < 2.0) ? float3(x, c, 0) : (hp < 3.0) ? float3(0, c, x)
               : (hp < 4.0) ? float3(0, x, c) : (hp < 5.0) ? float3(x, 0, c) : float3(c, 0, x);
    return rgb + (v - c);
}

struct VSOut
{
    float4 pos         : SV_POSITION;
    float2 uv          : TEXCOORD0;
    float3 color       : COLOR0;
    float3 norm        : NORMAL0;
    float  blendWeight : BLENDWEIGHT0;
    float  isSil       : TEXCOORD1;
};

// Quad corners in local 2D tangent space: 0(-1,-1) 1(1,-1) 2(-1,1) 3(1,1)
static const float2 s_quadCorners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
// The two triangles of a quad as six vertices, for the instanced vertex-shader path (mesh path: index triples).
static const uint s_quadVertexCorner[6] = { 0, 1, 2, 1, 3, 2 };
// A vertex the rasterizer will drop: behind the near plane. The mesh path zeroes its outputs instead.
static const float4 s_culledClipPos = float4(0.0, 0.0, -2.0, 1.0);

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

// Tangent frame of a splat: a disc in the surfel's tangent plane, or a camera-facing billboard.
void SplatTangents(float3 normal, float splatRadius, out float3 tangentX, out float3 tangentY)
{
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

// =========================================================================
// Chunk-level culling shared by the amplification shader and the vertex-shader path
// =========================================================================

// Conservative normal-cone backface test. It approximates every point in the chunk as viewed from one
// direction (eye -> chunk centre), which only holds when the chunk is small relative to its distance
// from the camera. Once the viewer is close enough that the chunk subtends a large solid angle --
// zoomed in close, near-clip range -- a chunk that is genuinely only partially front-facing (e.g. a
// curved surface near a silhouette) can get misclassified as fully backfacing by this single-sample
// test and dropped entirely, producing a visible hole with nothing else covering it. This is
// especially visible during LOD transitions when pulling the camera back: coarser parent chunks have
// larger boundingRadius and wider (less precise) normal cones, so they are exactly the chunks most
// prone to this misclassification right as they're being pulled in to replace finer detail. So the
// cone test is skipped at close range.
bool ChunkConeBackfacing(MeshletChunk chunk, float3 eyePos)
{
    if (g_EnableConeCulling != 1 || chunk.coneCutoff <= -0.99)
        return false;
    float3 toChunk = chunk.center - eyePos;
    float dist = length(toChunk);
    if (dist <= max(1e-4, chunk.boundingRadius * 3.0))
        return false;
    float3 viewDir = toChunk / dist; // Ray from camera towards chunk centre
    float sinCone = sqrt(max(0.0, 1.0 - chunk.coneCutoff * chunk.coneCutoff));
    float nDotV = dot(chunk.coneAxis, viewDir);
    // When the cluster normal cone points in the direction of the view ray, the cluster is backfacing
    return nDotV > sinCone + 0.02;
}

// "Show ONLY Locked Chunks": transition-locked or edge chunks pass, everything else is hidden.
bool ChunkIsLocked(MeshletChunk chunk)
{
    return (chunk.isSilhouette > 0.001) || (abs(chunk.blendWeight) > 0.001 && abs(chunk.blendWeight) < 0.999);
}

// Conservative AABB frustum test against the active culling camera (supports the detached debug camera).
bool ChunkInFrustum(MeshletChunk chunk)
{
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

    return (outsideLeft < 8) && (outsideRight < 8) &&
           (outsideBottom < 8) && (outsideTop < 8) &&
           (outsideNear < 8) && (outsideFar < 8);
}

// =========================================================================
// Per-surfel splat construction shared by mainMS and mainVS
// =========================================================================

struct SplatData
{
    float3 worldPos;
    float3 normal;   // Zeroed for the flat grey back sides in detached-camera mode
    float3 litColor;
    float3 tangentX;
    float3 tangentY;
};

// Decodes surfel surfelIndex for the active render mode, applies detached-camera culling (returns
// false when the surfel is dropped), the dilation morph, lighting and the edge / arrival tints.
bool BuildSplat(uint surfelIndex, float chunkBlendWeight, float chunkDilationMorph, float chunkIsSilhouette, out SplatData sd)
{
    float3 worldPos = float3(0.0, 0.0, 0.0);
    float3 normal = float3(0.0, 1.0, 0.0);
    float3 color = float3(0.0, 0.0, 0.0);
    float3 tangentX = float3(0.0, 0.0, 0.0), tangentY = float3(0.0, 0.0, 0.0);

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

        float radScale = (re == 0) ? 1.0f : (re == 1) ? 1.5f : (re == 2) ? 2.5f : 4.5f;
        float maxExtent = max(g_AABBExtents.x, max(g_AABBExtents.y, g_AABBExtents.z));
        float baseVoxelRadius = max(0.0005f, (maxExtent / 1024.0f) * 1.35f);
        float splatRadius = g_Radius * baseVoxelRadius * radScale;

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.00015f;
        splatRadius = max(splatRadius, minCoverageRadius);

        SplatTangents(normal, splatRadius, tangentX, tangentY);
    }
    else
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

        SplatTangents(normal, splatRadius, tangentX, tangentY);
    }

    // When the camera is detached: render the model as a hollow plaster mold shell
    if (g_UseDetachedCullCam == 1)
    {
        // 1. Frustum culling against the frozen detached camera
        float4 cullClip = mul(g_CullViewProj, float4(worldPos, 1.0));
        bool outsideFrustum = (cullClip.w <= 0.0001) ||
                              (cullClip.x < -cullClip.w) || (cullClip.x > cullClip.w) ||
                              (cullClip.y < -cullClip.w) || (cullClip.y > cullClip.w) ||
                              (cullClip.z < 0.0) || (cullClip.z > cullClip.w);

        // 2. Normal culling wrt the detached camera: keep only the front-facing shell it could see
        float3 toCullCam = g_CullEyePos - worldPos;
        float distCull = length(toCullCam);
        float3 normCullDir = distCull > 1e-4 ? (toCullCam / distCull) : float3(0, 0, 1);
        float nDotCull = dot(normal, normCullDir);
        bool isBackFacingToDetached = (dot(normal, normal) > 0.1) && (nDotCull < -0.05);

        if (outsideFrustum || isBackFacingToDetached)
        {
            sd.worldPos = worldPos; sd.normal = normal; sd.litColor = color; sd.tangentX = tangentX; sd.tangentY = tangentY;
            return false;
        }
        // 3. Back sides in solid mid grey: a surfel whose normal faces away from the VIEWER is being
        // looked at from behind, so it is painted a flat, unlit grey. That makes it obvious which side of
        // the model (relative to the frozen camera) the viewer is looking at. Zeroing the normal removes
        // the lighting term below, so the grey is uniform.
        if (dot(normal, normal) > 0.1)
        {
            float3 toViewer = g_ViewerEyePos - worldPos;
            float distViewer = length(toViewer);
            float3 normViewerDir = distViewer > 1e-4 ? (toViewer / distViewer) : float3(0, 0, 1);
            float nDotViewer = dot(normal, normViewerDir);

            if (nDotViewer < -0.02)
            {
                color = float3(0.5, 0.5, 0.5);
                normal = float3(0.0, 0.0, 0.0);
            }
        }
    }

    // Smooth geometric dilation morph along silhouette normals
    float splatRad = length(tangentX);
    if (splatRad < 1e-6) splatRad = 0.02;

    if (chunkDilationMorph > 0.0001 && abs(chunkBlendWeight) < 0.999)
    {
        float w = (chunkBlendWeight >= 0.0) ? (1.0 - chunkBlendWeight) * chunkDilationMorph : (-chunkBlendWeight) * chunkDilationMorph;
        worldPos += normal * (splatRad * w * 1.5);
        tangentX *= (1.0 + w * 0.4);
        tangentY *= (1.0 + w * 0.4);
    }

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
            // Pure silhouette edge chunks: crisp lavender outline (ONLY when Highlight Edge Chunks is on)
            if (g_HighlightSilhouette == 1)
            {
                float3 hotLavender = float3(0.85, 0.55, 0.98);
                litColor = lerp(litColor, hotLavender, 0.90);
            }
        }
        else if (g_ShowChunkStream == 1)
        {
            // Streaming arrival glow (Refinement Visualizer): w runs 0.95 (just delivered) -> 0 (faded).
            // Newly added chunks -- the leading edge of the growing model -- flash bright, then settle to
            // a regular semi-transparent fill that fades out at the end of its life.
            float life = saturate(w / 0.95);                 // 1 = just arrived, 0 = expired
            float glow = life * life;                        // Bright peak at the leading edge, still strong at mid-life
            // Colours are orange by default (hue ~24 degrees); the Hue slider rotates both the regular
            // fill and the hot leading-edge colour together, Intensity scales the fill opacity and bloom.
            float3 regularTint = HsvToRgb(24.0 + g_ArrivalGlowHue, 0.96, 1.0);
            float3 hotTint     = HsvToRgb(24.0 + g_ArrivalGlowHue, 0.55, 1.0);
            float3 tint = lerp(regularTint, hotTint, glow);
            float opacity = saturate(0.60 * g_ArrivalGlowIntensity) * smoothstep(0.0, 0.25, life); // Semi-transparent fill; fades out over the last quarter
            litColor = lerp(litColor, tint * (lighting + 0.35 * glow), opacity);
            litColor += hotTint * (glow * 0.55 * g_ArrivalGlowIntensity); // Emissive bloom on the newest chunks
        }
    }

    sd.worldPos = worldPos;
    sd.normal = normal;
    sd.litColor = litColor;
    sd.tangentX = tangentX;
    sd.tangentY = tangentY;
    return true;
}

// One corner (0..3) of a splat quad.
VSOut SplatCornerVertex(SplatData sd, uint corner, float chunkBlendWeight, float chunkIsSilhouette)
{
    float2 cc = s_quadCorners[corner];
    float3 offset = sd.tangentX * cc.x + sd.tangentY * cc.y;

    VSOut o;
    o.pos = mul(g_ViewProj, float4(sd.worldPos + offset, 1.0));
    o.uv = cc * 0.5 + 0.5;
    o.color = sd.litColor;
    o.norm = sd.normal;
    o.blendWeight = chunkBlendWeight;
    o.isSil = (g_HighlightSilhouette == 1) ? chunkIsSilhouette : 0.0;
    return o;
}

VSOut CulledSplatVertex()
{
    VSOut o;
    o.pos = s_culledClipPos;
    o.uv = float2(0.0, 0.0);
    o.color = float3(0.0, 0.0, 0.0);
    o.norm = float3(0.0, 0.0, 0.0);
    o.blendWeight = 0.0;
    o.isSil = 0.0;
    return o;
}

// =========================================================================
// Per-surfel item-prepass disc shared by itemMS and itemVS
// =========================================================================

void BuildItemSplat(uint surfelIndex, out float3 worldPos, out float3 tangentX, out float3 tangentY)
{
    worldPos = float3(0.0, 0.0, 0.0);
    tangentX = float3(0.0, 0.0, 0.0);
    tangentY = float3(0.0, 0.0, 0.0);
    float3 normal = float3(0.0, 1.0, 0.0);

    if (g_RenderMode == 1)
    {
        PackedSurfel s = g_SurfelBuffer[surfelIndex];
        uint qx = s.packedPosRadius & 0x3FF;
        uint qy = (s.packedPosRadius >> 10) & 0x3FF;
        uint qz = (s.packedPosRadius >> 20) & 0x3FF;
        uint re = (s.packedPosRadius >> 30) & 0x3;

        worldPos = g_AABBMin + float3(qx / 1023.0, qy / 1023.0, qz / 1023.0) * g_AABBExtents;
        normal = UnpackNormalOct16(s.packedNormalColor & 0xFFFF);

        float radScale = (re == 0) ? 1.0f : (re == 1) ? 1.5f : (re == 2) ? 2.5f : 4.5f;
        float maxExtent = max(g_AABBExtents.x, max(g_AABBExtents.y, g_AABBExtents.z));
        float baseVoxelRadius = max(0.0005f, (maxExtent / 1024.0f) * 1.35f);
        float splatRadius = g_Radius * baseVoxelRadius * radScale;

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        // Item prepass only: never let a disc drop below about one screen pixel. At distance the
        // visible render's discs are ~0.2 px, which is fine for the picture but leaves the item buffer
        // full of background holes -- so nearly every chunk "touched background" and was flagged as a
        // silhouette edge, and a rotation (which re-runs detection) then pulled the whole model two
        // levels finer. One-pixel discs give a solid item buffer, so only the true rim is an edge.
        float minCoverageRadius = distToCam * 0.00075f;
        splatRadius = max(splatRadius, minCoverageRadius);

        SplatTangents(normal, splatRadius, tangentX, tangentY);
    }
    else if (g_RenderMode == 2)
    {
        RawSurfel s = g_RawSurfelBuffer[surfelIndex];
        worldPos = s.position;
        normal = s.normal;
        float splatRadius = max(0.001f, s.radius * g_Radius);
        {
            float4 clipCenterRaw = mul(g_ViewProj, float4(worldPos, 1.0));
            splatRadius = max(splatRadius, max(0.1f, clipCenterRaw.w) * 0.00075f); // ~1 px minimum, item prepass only (see above)
        }
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
}

struct ItemVSOut
{
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    nointerpolation uint chunkId : CHUNKID0;
};

ItemVSOut ItemCornerVertex(float3 worldPos, float3 tangentX, float3 tangentY, uint corner, uint chunkIndex)
{
    float2 cc = s_quadCorners[corner];
    float3 offset = tangentX * cc.x + tangentY * cc.y;

    ItemVSOut o;
    o.pos = mul(g_ViewProj, float4(worldPos + offset, 1.0));
    o.uv = cc * 0.5 + 0.5;
    o.chunkId = chunkIndex + 1; // 1-based chunk ID so 0 is background
    return o;
}

// =========================================================================
// Interior occlusion volume cubes shared by occluderMS and occluderVS
//
// Solid depth-writing cubes baked at preprocessing time (see PreprocessApp::
// BuildOcclusionVolume) so the splat pass can depth-test against them and discard
// far-side surfels visible through gaps in a sparse near side.
//
// The volume is an adaptive octree of grid-aligned cubes (see OcclusionVoxelGPU): fine
// cubes along the surface, larger merged cubes inside, all tiling exactly. Its erosion
// ("shave") is baked at preprocessing time, so there is nothing to tune here -- the
// viewer draws precisely what was packaged. Only a cube's EXPOSED faces (those with
// some non-solid fine cell across them, per the mask baked into packedColor) are
// emitted; fully buried faces are never visible and are skipped.
//
// g_OcclusionVoxelBuffer holds the volume's whole mip chain back to back (mip 0, the
// OcclusionMipTable). The CPU picks ONE mip per frame from the projected cell size at
// the model's nearest point and passes its block range as g_OcclusionVoxelFirst /
// g_OcclusionVoxelCount, so only that mip is drawn.
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

static const uint OCCLUDER_FACE_MASK_SHIFT = 16;
static const uint OCCLUDER_FACE_MASK_ALL   = 0x3F;
static const uint OCCLUDER_MASK_VALID_BIT  = 22;

// Exposed-face mask of a cube after the detached-camera rule: the volume obeys the same rule as the
// surfels -- only what the frozen camera could see is drawn. A cube outside its frustum is dropped
// whole; of a cube inside, only the faces turned toward it survive, so from the side the volume reads
// as the open shell the detached camera would have seen, not a closed solid filling in the culled far
// side.
uint OccluderExposedFaces(OcclusionVoxel v)
{
    uint exposed = (v.packedColor >> OCCLUDER_FACE_MASK_SHIFT) & OCCLUDER_FACE_MASK_ALL;
    if (((v.packedColor >> OCCLUDER_MASK_VALID_BIT) & 1) == 0)
        exposed = OCCLUDER_FACE_MASK_ALL; // Legacy file baked before the mask existed

    if (g_UseDetachedCullCam == 1 && exposed != 0)
    {
        float4 cc = mul(g_CullViewProj, float4(v.center, 1.0));
        float slack = v.halfSize * 1.75; // Corner reach: keep cubes straddling the frustum edge
        bool outside = (cc.w <= 0.0001) ||
                       (cc.x < -cc.w - slack) || (cc.x > cc.w + slack) ||
                       (cc.y < -cc.w - slack) || (cc.y > cc.w + slack) ||
                       (cc.z < 0.0) || (cc.z > cc.w);
        if (outside)
        {
            exposed = 0;
        }
        else
        {
            [unroll]
            for (uint f = 0; f < 6; f++)
            {
                if ((exposed & (1u << f)) == 0) continue;
                float3 faceCenter = v.center + s_occluderFaceNormal[f] * v.halfSize;
                if (dot(s_occluderFaceNormal[f], g_CullEyePos - faceCenter) <= 0.0)
                    exposed &= ~(1u << f); // Back-facing to the detached camera
            }
        }
    }
    return exposed;
}

OccluderVSOut OccluderCornerVertex(OcclusionVoxel v, uint face, uint corner)
{
    float3 worldPos = v.center + s_occluderCubeCorners[s_occluderFaceIdx[face][corner]] * v.halfSize;

    OccluderVSOut o;
    o.pos = mul(g_ViewProj, float4(worldPos, 1.0));
    o.color = UnpackColorRGB565(v.packedColor);
    o.norm = s_occluderFaceNormal[face];
    return o;
}

#ifndef SURFELS_NO_MESH_SHADERS

// =========================================================================
// Amplification / Task Shader Stage (mainAS)
// =========================================================================

struct ChunkPayload
{
    uint chunkIndices[AS_GROUP_SIZE];
    uint flatGroupIndex;
};

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

        isVisible = ChunkInFrustum(chunk);

        // Conservative normal-cone backface culling in the task shader
        if (isVisible)
        {
            float3 eyePos = (g_UseDetachedCullCam == 1) ? g_CullEyePos : g_ViewerEyePos;
            if (ChunkConeBackfacing(chunk, eyePos))
                isVisible = false;
        }

        // Show ONLY Locked Chunks (Transition or Edge)
        if (isVisible && g_ShowOnlyLocked == 1 && !ChunkIsLocked(chunk))
            isVisible = false;
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

        if (g_ShowOnlyLocked == 1 && !ChunkIsLocked(chunk))
        {
            SetMeshOutputCounts(0, 0);
            return;
        }
    }
    else
    {
        uint groupBase = payload.flatGroupIndex * SURFELS_PER_GROUP;
        uint remaining = groupBase < g_SurfelCount ? g_SurfelCount - groupBase : 0;
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, remaining);
        surfelIndex = groupBase + threadId;
    }

    SetMeshOutputCounts(groupSurfelCount * 4, groupSurfelCount * 2);

    if (threadId >= groupSurfelCount || surfelIndex >= g_SurfelCount)
        return;

    uint vBase = threadId * 4;
    uint pBase = threadId * 2;

    SplatData sd;
    if (!BuildSplat(surfelIndex, chunkBlendWeight, chunkDilationMorph, chunkIsSilhouette, sd))
    {
        tris[pBase + 0] = uint3(0, 0, 0);
        tris[pBase + 1] = uint3(0, 0, 0);
        [unroll]
        for (uint z = 0; z < 4; z++)
        {
            VSOut o = CulledSplatVertex();
            o.pos = float4(0.0, 0.0, 0.0, 0.0);
            verts[vBase + z] = o;
        }
        return;
    }

    [unroll]
    for (uint c = 0; c < 4; c++)
    {
        verts[vBase + c] = SplatCornerVertex(sd, c, chunkBlendWeight, chunkIsSilhouette);
    }

    tris[pBase + 0] = uint3(vBase + 0, vBase + 1, vBase + 2);
    tris[pBase + 1] = uint3(vBase + 1, vBase + 3, vBase + 2);
}

// =========================================================================
// GPU Item Prepass Stage (itemMS) for Silhouette Edge Inversion
// =========================================================================

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

        // Fast normal-cone backface culling (see ChunkConeBackfacing for the close-range guard)
        if (ChunkConeBackfacing(c, g_ViewerEyePos))
        {
            SetMeshOutputCounts(0, 0);
            return;
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

    float3 worldPos, tangentX, tangentY;
    BuildItemSplat(surfelIndex, worldPos, tangentX, tangentY);

    uint vBase = threadId * 4;
    [unroll]
    for (uint c = 0; c < 4; c++)
    {
        verts[vBase + c] = ItemCornerVertex(worldPos, tangentX, tangentY, c, chunkIndex);
    }

    uint pBase = threadId * 2;
    tris[pBase + 0] = uint3(vBase + 0, vBase + 1, vBase + 2);
    tris[pBase + 1] = uint3(vBase + 1, vBase + 3, vBase + 2);
}

// =========================================================================
// Interior Occlusion Volume Stage (occluderMS): one threadgroup per voxel, direct-dispatched
// with no amplification shader stage (like itemMS).
// =========================================================================

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
    // final face count first (0 for an out-of-range index), set it once, and only then bail out.
    uint voxelIndex = groupId.y * 32768 + groupId.x;
    bool valid = voxelIndex < g_OcclusionVoxelCount;

    OcclusionVoxel v = g_OcclusionVoxelBuffer[valid ? (g_OcclusionVoxelFirst + voxelIndex) : 0];
    uint exposed = OccluderExposedFaces(v);

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

    if (slot < faceCount)
    {
        verts[threadId] = OccluderCornerVertex(v, face, threadId % 4);
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

#endif // SURFELS_NO_MESH_SHADERS

// =========================================================================
// Vertex-shader fallback stages (no mesh shader support)
//
// DrawInstanced(6, instances): instance = one surfel (chunked pipeline: instance / 64 is the slot in
// the back-to-front sorted chunk list, instance % 64 the surfel within the chunk, so instances
// rasterize in the same sorted order the mesh path dispatches in); vertex = one of the quad's six
// corners. Chunk frustum culling is left to the rasterizer (a culled corner sits behind the near
// plane); the normal-cone and locked-chunk filters and every per-surfel rule are the same as the
// mesh path. Compiles for vs_6_0 and, through the legacy compiler, vs_5_1.
// =========================================================================

VSOut mainVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint surfelIndex = 0;
    float chunkBlendWeight = 1.0;
    float chunkDilationMorph = 0.0;
    float chunkIsSilhouette = 0.0;

    if (g_UseChunkedPipeline == 1)
    {
        uint slot = instanceId / SURFELS_PER_GROUP;
        uint local = instanceId - slot * SURFELS_PER_GROUP;
        if (slot >= g_TotalChunks)
            return CulledSplatVertex();
        uint chunkIdx = g_SortedChunkIndices[slot];
        if (chunkIdx >= g_TotalChunks)
            chunkIdx = slot;
        MeshletChunk chunk = g_ChunkBuffer[chunkIdx];
        if (local >= chunk.surfelCount)
            return CulledSplatVertex();
        float3 eyePos = (g_UseDetachedCullCam == 1) ? g_CullEyePos : g_ViewerEyePos;
        if (ChunkConeBackfacing(chunk, eyePos))
            return CulledSplatVertex();
        if (g_ShowOnlyLocked == 1 && !ChunkIsLocked(chunk))
            return CulledSplatVertex();
        surfelIndex = chunk.surfelOffset + local;
        chunkBlendWeight = chunk.blendWeight;
        chunkDilationMorph = chunk.dilationMorph;
        chunkIsSilhouette = chunk.isSilhouette;
    }
    else
    {
        surfelIndex = instanceId;
    }

    if (surfelIndex >= g_SurfelCount)
        return CulledSplatVertex();

    SplatData sd;
    if (!BuildSplat(surfelIndex, chunkBlendWeight, chunkDilationMorph, chunkIsSilhouette, sd))
        return CulledSplatVertex();

    return SplatCornerVertex(sd, s_quadVertexCorner[vertexId % 6], chunkBlendWeight, chunkIsSilhouette);
}

// Item prepass: DrawInstanced(6, chunkCount * 64) (chunked, chunk index = instance / 64, unsorted like
// itemMS) or DrawInstanced(6, surfelCount) (flat).
ItemVSOut itemVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    ItemVSOut culled;
    culled.pos = s_culledClipPos;
    culled.uv = float2(0.0, 0.0);
    culled.chunkId = 0;

    uint surfelIndex = 0;
    uint chunkIndex = 0;

    if (g_UseChunkedPipeline == 1)
    {
        chunkIndex = instanceId / SURFELS_PER_GROUP;
        uint local = instanceId - chunkIndex * SURFELS_PER_GROUP;
        if (chunkIndex >= g_TotalChunks)
            return culled;
        MeshletChunk c = g_ChunkBuffer[chunkIndex];
        if (ChunkConeBackfacing(c, g_ViewerEyePos))
            return culled;
        if (local >= c.surfelCount)
            return culled;
        surfelIndex = c.surfelOffset + local;
    }
    else
    {
        surfelIndex = instanceId;
        chunkIndex = 0;
    }

    if (surfelIndex >= g_SurfelCount)
        return culled;

    float3 worldPos, tangentX, tangentY;
    BuildItemSplat(surfelIndex, worldPos, tangentX, tangentY);
    return ItemCornerVertex(worldPos, tangentX, tangentY, s_quadVertexCorner[vertexId % 6], chunkIndex);
}

// Occlusion volume: DrawInstanced(36, blocksInMip): instance = block, vertex / 6 = face, the rest the corner.
OccluderVSOut occluderVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    OccluderVSOut culled;
    culled.pos = s_culledClipPos;
    culled.color = float3(0.0, 0.0, 0.0);
    culled.norm = float3(0.0, 0.0, 1.0);

    if (instanceId >= g_OcclusionVoxelCount)
        return culled;

    OcclusionVoxel v = g_OcclusionVoxelBuffer[g_OcclusionVoxelFirst + instanceId];
    uint exposed = OccluderExposedFaces(v);
    uint face = vertexId / 6;
    if (face >= 6 || (exposed & (1u << face)) == 0)
        return culled;

    return OccluderCornerVertex(v, face, s_quadVertexCorner[vertexId - face * 6]);
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

    // True screen-space Bayer matrix stochastic dithering:
    // an exact complementary stochastic cross-dissolve between parent and child chunks in screen space.
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

uint itemPS(ItemVSOut i) : SV_Target0
{
    float2 centered = i.uv * 2.0 - 1.0;
    if (dot(centered, centered) > 1.0)
        discard;
    return i.chunkId;
}

float4 occluderPS(OccluderVSOut i) : SV_Target0
{
    float3 lightDir = normalize(float3(0.5, 0.8, 0.6));
    float ndl = abs(dot(normalize(i.norm), lightDir));
    float lighting = 0.35 + 0.65 * ndl;
    return float4(i.color * lighting, 1.0);
}
