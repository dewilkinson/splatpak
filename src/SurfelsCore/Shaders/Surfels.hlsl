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
//  4. Splat mode (g_RenderMode == 3): full 3D Gaussians from 20-byte PackedSplat records, drawn in
//     per-splat depth order (SplatSortCS.hlsl) with the projection, footprint, falloff and colour
//     evaluation of the reference PlayCanvas / SuperSplat viewer.

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
    uint   sourceIndex; // Splat mode bookkeeping (SurfelVertex::sourceIndex); unused by the shaders
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
    float  dilationMorph; // Morph dilation factor for silhouette reconstruction; splat mode (g_RenderMode == 3): the chunk's opacity weight for the level 1 -> 0 hand-over instead (0 = invisible .. 1 = the records' opacity, see BuildGaussianSplat)
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

// Splat mode (g_RenderMode == 3). PackedSplat is PackedSplatGPU (WaveletTypes.h): five words holding
// a 16:16:16 position, three 8-bit log2 scales, 8-bit opacity, 8:8:8 DC colour, flags, a binary16 SH
// scale and a smallest-three quaternion. The SH stream holds g_SHRecordBytes signed bytes per splat.
struct PackedSplat
{
    uint w0; // position x | position y << 16
    uint w1; // position z | scale x << 16 | scale y << 24
    uint w2; // scale z | opacity << 8 | red << 16 | green << 24
    uint w3; // blue | flags << 8 | SH scale (binary16) << 16
    uint w4; // rotation, smallest-three: index (2 bits) | a << 2 | b << 12 | c << 22
};
StructuredBuffer<PackedSplat> g_SplatBuffer  : register(t5);
ByteAddressBuffer             g_SHBuffer     : register(t6);
StructuredBuffer<uint2>       g_SortedSplats : register(t7); // Per-frame depth order, far to near: x = key, y = slot
StructuredBuffer<uint2>       g_SlotInfo     : register(t8); // Per slot: x = splat index, y = chunk index | SLOT_CULLED_BIT
#define SLOT_CULLED_BIT 0x80000000u

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
    uint     g_AutoSplatSize;        // 1 = disc radius from the level's point spacing (g_LodRadius), 0 = size classes x g_Radius
    uint     g_CulledPass;           // Detach Camera: 0 = the splats the frozen camera sees, 1 = only the ones it culled (the red volume)
    float2   g_AutoSplatPad1;
    float4   g_LodRadius[2];         // Auto splat size: disc radius per LOD level (world units), index = lodLevel 0..7; 0 = level unknown
    // Splat mode
    float4x4 g_View;                 // World to view (right-handed: in front of the camera is negative z)
    float2   g_Viewport;             // Render target size in pixels
    float    g_Proj00;               // Projection matrix element [0][0]; focal length in pixels = g_Viewport.x * g_Proj00
    float    g_ScaleLog2Min;         // log2 scale a scale byte of 0 decodes to
    float    g_ScaleLog2Max;         // log2 scale a scale byte of 255 decodes to
    uint     g_SHDegree;             // Spherical-harmonics degree in the SH stream (0 = DC only)
    uint     g_SHRecordBytes;        // Bytes per splat in the SH stream
    uint     g_SplatBlendSpace;      // 0 = blend the stored display-space colours as they are, 1 = decode to linear first (sRGB target)
    float3   g_CamForward;           // Viewer forward (unit)
    uint     g_SplatSlotCount;       // Slots in g_SortedSplats this frame
    float4   g_SplatDiscRadius[2];   // Splat mode: camera-facing disc radius per level for level kSplatDiscMinLevel and up (world units), index = lodLevel 0..7; 0 = draw Gaussians
    float    g_SplatDiscOpacityFloor; // Splat mode: opacity floor for the discs of level 1 and up (Match Level 0 Softness; 0 = the record's opacity as is)
    float3   g_SplatDiscPad;
};

// Auto splat size: every disc of a level takes that level's density-derived radius (its typical point
// spacing times the coverage factor), so a dense patch covers the surface exactly and an isolated point
// -- a scan outlier or a decimation stray -- gets the same small disc instead of the inflated one the
// size classes hand it, which is what made the halo of ghost discs around the model at coarse levels.
// The point's own size class (1, 1.5, 2.5, 4.5: the packer's measure of how far its neighbours are)
// still scales the level radius, capped at 2.5, so a locally sparse patch keeps covering while a lone
// stray cannot balloon to the largest class.
float AutoSplatRadius(uint lod, float classRadius, float classScale)
{
    if (g_AutoSplatSize != 1 || lod >= 8) return classRadius;
    float lr = g_LodRadius[lod >> 2][lod & 3];
    return (lr > 0.0) ? g_Radius * lr * min(classScale, 2.5) : classRadius;
}

// Splat mode: chunks at this level and above draw as camera-facing discs of the level's point spacing
// (g_SplatDiscRadius) instead of projected Gaussians (see BuildGaussianSplat). Fixed by design: no control.
// 0 for now: every level, level 0 included, draws as discs; the Gaussian level 0 is switched off (see
// kSplatGaussianLevel0 in SurfelsApp.cpp). 1 restores it.
static const uint kSplatDiscMinLevel = 0;

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

// Streaming arrival glow (Refinement Visualizer): w is the chunk's wave, 0.95 (just delivered) -> 0
// (faded). Newly added chunks -- the leading edge of the growing model -- flash bright, then settle to
// a regular semi-transparent fill that fades out at the end of its life. Shared by the disc modes
// (BuildSplat, with the surfel's directional lighting) and the splat mode's colour (SplatColor, lighting 1),
// so the Gaussians and the coarse-level discs take the same tint.
void ArrivalGlow(inout float3 color, float w, float lighting)
{
    float life = saturate(w / 0.95);                 // 1 = just arrived, 0 = expired
    float glow = life * life;                        // Bright peak at the leading edge, still strong at mid-life
    // Colours are orange by default (hue ~24 degrees); the Hue slider rotates both the regular
    // fill and the hot leading-edge colour together, Intensity scales the fill opacity and bloom.
    float3 regularTint = HsvToRgb(24.0 + g_ArrivalGlowHue, 0.96, 1.0);
    float3 hotTint     = HsvToRgb(24.0 + g_ArrivalGlowHue, 0.55, 1.0);
    float3 tint = lerp(regularTint, hotTint, glow);
    float opacity = saturate(0.60 * g_ArrivalGlowIntensity) * smoothstep(0.0, 0.25, life); // Semi-transparent fill; fades out over the last quarter
    color = lerp(color, tint * (lighting + 0.35 * glow), opacity);
    color += hotTint * (glow * 0.55 * g_ArrivalGlowIntensity); // Emissive bloom on the newest chunks
}

struct VSOut
{
    float4 pos         : SV_POSITION;
    float2 uv          : TEXCOORD0;
    float3 color       : COLOR0;
    float3 norm        : NORMAL0;
    float  blendWeight : BLENDWEIGHT0;
    float  isSil       : TEXCOORD1;
    float  solid       : TEXCOORD2; // 1 = sub-pixel splat drawn as an opaque dot (see SolidDot)
    float  culledTint  : TEXCOORD3; // 1 = Detach Camera: the frozen camera would have culled this splat (drawn faint)
    float  splatAlpha  : TEXCOORD4; // Splat mode: the Gaussian's opacity (peak alpha)
    float  disc        : TEXCOORD5; // Splat mode: 1 = camera-facing disc of a coarse level (see BuildGaussianSplat), 0 = Gaussian
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
    float  solid;    // 1 = drawn as an opaque dot (see SolidDot)
    float  culledTint; // 1 = Detach Camera: culled by the frozen camera, drawn as a translucent red layer
};

// About one screen pixel of world radius at this distance (the same constant the item prepass uses).
static const float kPixelRadiusPerDistance = 0.00075;

// Splats that project below ~1.5 pixels cannot cover as discs: a normal-oriented disc seen at a
// grazing angle is a sliver, and the Gaussian falloff leaves each dot mostly transparent, so a far
// (coarse) level reads as a translucent speckle cloud around whatever is behind it. Such a splat is
// drawn instead as a camera-facing, fully opaque dot of at least one pixel: a dense level becomes a
// solid surface and an isolated stray stays a single dot rather than an inflated disc.
void SolidDot(float distToCam, inout float3 tangentX, inout float3 tangentY, out float solid)
{
    float onePixel = distToCam * kPixelRadiusPerDistance;
    float radius = max(length(tangentX), length(tangentY));
    if (radius < onePixel * 1.5)
    {
        float r = max(radius, onePixel);
        tangentX = g_CamRight * r;
        tangentY = g_CamUp * r;
        solid = 1.0;
    }
    else
    {
        solid = 0.0;
    }
}

// Decodes surfel surfelIndex for the active render mode, applies the detached-camera shading (what the
// frozen camera would have culled is kept, flat dark grey), the dilation morph, lighting and the edge /
// arrival tints. frozenCulled: the whole chunk was outside the frozen camera's frustum or normal cone.
bool BuildSplat(uint surfelIndex, uint lod, float chunkBlendWeight, float chunkDilationMorph, float chunkIsSilhouette, bool frozenCulled, out SplatData sd)
{
    bool frozenCulledSplat = false; // Drawn as translucent red at the end (own depth-tested pass): the frozen camera would not draw this surfel
    float3 worldPos = float3(0.0, 0.0, 0.0);
    float3 normal = float3(0.0, 1.0, 0.0);
    float3 color = float3(0.0, 0.0, 0.0);
    float3 tangentX = float3(0.0, 0.0, 0.0), tangentY = float3(0.0, 0.0, 0.0);
    float solid = 0.0;

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
        float splatRadius = AutoSplatRadius(lod, g_Radius * baseVoxelRadius * radScale, radScale);

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.00015f;
        splatRadius = max(splatRadius, minCoverageRadius);

        SplatTangents(normal, splatRadius, tangentX, tangentY);
        SolidDot(distToCam, tangentX, tangentY, solid);
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
        float splatRadius = AutoSplatRadius(lod, baseRadius * g_Radius, 1.0);

        float4 clipCenter = mul(g_ViewProj, float4(worldPos, 1.0));
        float distToCam = max(0.1f, clipCenter.w);
        float minCoverageRadius = distToCam * 0.00015f;
        splatRadius = max(splatRadius, minCoverageRadius);

        SplatTangents(normal, splatRadius, tangentX, tangentY);
        SolidDot(distToCam, tangentX, tangentY, solid);
    }

    // When the camera is detached: everything the frozen camera would draw keeps its colour; everything
    // it would have culled stays on screen as flat dark grey, so the observer sees the frozen frustum and
    // the front shell in colour against the rest of the model in grey instead of against a void.
    if (g_UseDetachedCullCam == 1)
    {
        // 1. Frustum test against the frozen detached camera
        float4 cullClip = mul(g_CullViewProj, float4(worldPos, 1.0));
        bool outsideFrustum = (cullClip.w <= 0.0001) ||
                              (cullClip.x < -cullClip.w) || (cullClip.x > cullClip.w) ||
                              (cullClip.y < -cullClip.w) || (cullClip.y > cullClip.w) ||
                              (cullClip.z < 0.0) || (cullClip.z > cullClip.w);

        // 2. Far-side test: the half of the model beyond its centre, as seen from the frozen camera, is
        // what that camera cannot see. Decided by position, not by the surfel normal: splat scans carry
        // arbitrary normal signs, so a normal test speckles both halves instead of splitting them.
        float3 modelCentre = g_AABBMin + g_AABBExtents * 0.5;
        float3 frozenForward = modelCentre - g_CullEyePos;
        float fwdLen = length(frozenForward);
        frozenForward = fwdLen > 1e-4 ? frozenForward / fwdLen : float3(0, 0, -1);
        bool farSideOfDetached = dot(worldPos - modelCentre, frozenForward) > 0.0;

        if (frozenCulled || outsideFrustum || farSideOfDetached)
        {
            frozenCulledSplat = true;
            normal = float3(0.0, 0.0, 0.0); // No lighting term: uniform tint
        }
        // The two halves are drawn by separate passes (see SurfelsRenderer.h): pass 0 draws what the frozen
        // camera sees, pass 1 (twice: depth prepass, then the 10% blend) draws only what it culled.
        if ((g_CulledPass == 1) != frozenCulledSplat)
        {
            sd.worldPos = worldPos; sd.normal = normal; sd.litColor = color; sd.tangentX = tangentX; sd.tangentY = tangentY; sd.solid = 0.0; sd.culledTint = 0.0;
            return false;
        }
        // 3. Back sides of the visible shell in solid mid grey: a surfel whose normal faces away from the
        // VIEWER is being looked at from behind, so it is painted a flat, unlit grey. That makes it
        // obvious which side of the model (relative to the frozen camera) the viewer is looking at.
        else if (dot(normal, normal) > 0.1)
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
            // Streaming arrival glow (Refinement Visualizer): w runs 0.95 (just delivered) -> 0 (faded)
            ArrivalGlow(litColor, w, lighting);
        }
    }

    if (frozenCulledSplat)
        // Detach Camera: culled by the frozen camera -- a dim red at 10% opacity per splat (mainPS). Dozens of
        // splats stack per pixel, so even 10% alpha adds up to near-full coverage; the dim colour caps what
        // that stack can reach at a dark, see-through tint instead of a bright fill.
        litColor = float3(1.0, 0.12, 0.10); // Detach Camera: culled by the frozen camera -- red, drawn once (depth-tested against the viewer) at 10% opacity in mainPS

    sd.worldPos = worldPos;
    sd.normal = normal;
    sd.litColor = litColor;
    sd.tangentX = tangentX;
    sd.tangentY = tangentY;
    sd.solid = solid;
    sd.culledTint = frozenCulledSplat ? 1.0 : 0.0;
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
    o.solid = sd.solid;
    o.culledTint = sd.culledTint;
    o.splatAlpha = 1.0;
    o.disc = 0.0;
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
    o.solid = 0.0;
    o.culledTint = 0.0;
    o.splatAlpha = 0.0;
    o.disc = 0.0;
    return o;
}

// =========================================================================
// Splat mode (g_RenderMode == 3): full 3D Gaussians, PlayCanvas / SuperSplat technique
// =========================================================================

struct GaussianSplat
{
    float4 clipCenter; // Centre in clip space (z clamped into the depth range)
    float2 axis1;      // Clip-space offset of the quad's first half-axis (already scaled by uvScale)
    float2 axis2;      // Clip-space offset of the second half-axis
    float  uvScale;    // The corner uv's shrink factor (clipCorner): the quad is cut where alpha falls below 1/255
    float3 color;      // Display-space colour (DC + harmonics), or linear when g_SplatBlendSpace == 1
    float  alpha;      // Opacity
    float  disc;       // 1 = camera-facing disc of a coarse level (kSplatDiscMinLevel and up), 0 = Gaussian
    float  solid;      // Disc: 1 = sub-pixel disc drawn as an opaque dot (see SolidDot)
};

// Splat mode falloff over the quad: the reference viewer's normalised Gaussian of the squared distance x
// in the shrunken corner space, reaching zero at the quad edge (x = 1). Shared by the Gaussians and the
// coarse-level discs.
float SplatFalloff(float x)
{
    const float EXP4 = 0.01831563888873418; // exp(-4)
    return (exp(-4.0 * x) - EXP4) / (1.0 - EXP4);
}

// Rotation matrix of a unit quaternion (x, y, z, w): mul(R, v) rotates v.
float3x3 QuatToMat3(float4 q)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return float3x3(
        1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z),       2.0 * (x * z + w * y),
        2.0 * (x * y + w * z),       1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - w * x),
        2.0 * (x * z - w * y),       2.0 * (y * z + w * x),       1.0 - 2.0 * (x * x + y * y));
}

float3 SRGBToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(max(c + 0.055, 0.0) / 1.055, 2.4);
    float3 r; // Component-wise (a vector condition is not a valid ternary in every compiler)
    r.x = (c.x <= 0.04045) ? lo.x : hi.x;
    r.y = (c.y <= 0.04045) ? lo.y : hi.y;
    r.z = (c.z <= 0.04045) ? lo.z : hi.z;
    return r;
}

// One signed byte of the SH stream: coefficient k (0..44, coefficient-major, three channels each) of a splat.
float LoadSHByte(uint splatIndex, uint k)
{
    uint addr = splatIndex * g_SHRecordBytes + k;
    uint word = g_SHBuffer.Load(addr & ~3u);
    uint b = (word >> ((addr & 3u) * 8u)) & 0xFFu;
    return (float)((int)(b << 24) >> 24);
}

// Spherical harmonics above the DC term, the 3DGS basis and order (see graphdeco-inria sh_utils.py),
// evaluated for a unit direction from the camera to the splat in model space.
float3 EvalSH(uint splatIndex, float shScale, float3 dir)
{
    const float SH_C1 = 0.4886025119029199;
    const float SH_C2_0 = 1.0925484305920792, SH_C2_1 = -1.0925484305920792, SH_C2_2 = 0.31539156525252005, SH_C2_3 = -1.0925484305920792, SH_C2_4 = 0.5462742152960396;
    const float SH_C3_0 = -0.5900435899266435, SH_C3_1 = 2.890611442640554, SH_C3_2 = -0.4570457994644658, SH_C3_3 = 0.3731763325901154, SH_C3_4 = -0.4570457994644658, SH_C3_5 = 1.445305721320277, SH_C3_6 = -0.5900435899266435;
    float x = dir.x, y = dir.y, z = dir.z;
    float3 sh0 = float3(LoadSHByte(splatIndex, 0), LoadSHByte(splatIndex, 1), LoadSHByte(splatIndex, 2)) * shScale;
    float3 sh1 = float3(LoadSHByte(splatIndex, 3), LoadSHByte(splatIndex, 4), LoadSHByte(splatIndex, 5)) * shScale;
    float3 sh2 = float3(LoadSHByte(splatIndex, 6), LoadSHByte(splatIndex, 7), LoadSHByte(splatIndex, 8)) * shScale;
    float3 result = SH_C1 * (-sh0 * y + sh1 * z - sh2 * x);
    if (g_SHDegree > 1)
    {
        float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
        float3 c[5];
        [unroll]
        for (uint k = 0; k < 5; k++)
            c[k] = float3(LoadSHByte(splatIndex, 9 + 3 * k), LoadSHByte(splatIndex, 10 + 3 * k), LoadSHByte(splatIndex, 11 + 3 * k)) * shScale;
        result += c[0] * (SH_C2_0 * xy) + c[1] * (SH_C2_1 * yz) + c[2] * (SH_C2_2 * (2.0 * zz - xx - yy)) + c[3] * (SH_C2_3 * xz) + c[4] * (SH_C2_4 * (xx - yy));
        if (g_SHDegree > 2)
        {
            float3 d[7];
            [unroll]
            for (uint k = 0; k < 7; k++)
                d[k] = float3(LoadSHByte(splatIndex, 24 + 3 * k), LoadSHByte(splatIndex, 25 + 3 * k), LoadSHByte(splatIndex, 26 + 3 * k)) * shScale;
            result += d[0] * (SH_C3_0 * y * (3.0 * xx - yy)) + d[1] * (SH_C3_1 * xy * z) + d[2] * (SH_C3_2 * y * (4.0 * zz - xx - yy))
                    + d[3] * (SH_C3_3 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy)) + d[4] * (SH_C3_4 * x * (4.0 * zz - xx - yy))
                    + d[5] * (SH_C3_5 * z * (xx - yy)) + d[6] * (SH_C3_6 * x * (xx - 3.0 * yy));
        }
    }
    return result;
}

// A splat's colour for this view: DC plus the harmonics for the view direction, clamped at black,
// decoded to linear when blending in linear space. Shared by the Gaussian and the disc path. wave is
// the chunk's arrival-glow wave (MeshletChunk.isSilhouette, 0 when not chunked): with Show Streaming
// Arrivals on, a chunk that has just been delivered (0 < wave < 0.99) takes the disc modes' arrival
// glow in display space, before the linear conversion; exactly 1 is the edge highlight, which splat
// mode does not draw. Otherwise the colour is the reference viewer's, untouched.
float3 SplatColor(uint splatIndex, float3 pos, float3 dc, float shScale, float wave)
{
    float3 color = dc;
    if (g_SHDegree > 0 && shScale > 0.0 && g_SHRecordBytes > 0)
    {
        float3 dir = normalize(pos - g_ViewerEyePos);
        color += EvalSH(splatIndex, shScale, dir);
    }
    color = max(color, 0.0);
    if (g_ShowChunkStream == 1 && wave > 0.0 && wave < 0.99) ArrivalGlow(color, wave, 1.0);
    if (g_SplatBlendSpace == 1) color = SRGBToLinear(color);
    return color;
}

// Decodes one Gaussian and projects it the way the reference viewer does: the 3D covariance from
// rotation and scales, the perspective Jacobian at the splat's view-space position, a 0.3 pixel
// dilation, the 2D eigen-decomposition, quad half-axes of 2 * sqrt(2 * lambda) pixels, and the
// alpha-dependent shrink of the quad. Returns false when the splat is behind the camera, off screen,
// or too faint to draw. lod is the chunk's level: at kSplatDiscMinLevel and above the splat is a
// camera-facing disc of the level's point spacing instead (see below); level 0 is always a Gaussian.
// fade is the chunk's opacity weight for the level 1 -> 0 hand-over (MeshletChunk.dilationMorph in
// splat mode): the level 1 parent's discs carry 1 - t and the level 0 children's Gaussians t while
// the cross-fade runs, so the two layers' weights sum to one at every pixel (no stipple, no loss of
// coverage) and the Gaussians' tails grow in with t instead of appearing at full strength. At fade 1
// (every chunk outside that hand-over) the paths run exactly as before. wave is the chunk's
// arrival-glow wave for SplatColor (0 when not chunked or not just delivered: no glow).
bool BuildGaussianSplat(uint splatIndex, uint lod, float fade, float wave, out GaussianSplat gs)
{
    gs.clipCenter = s_culledClipPos; gs.axis1 = float2(0.0, 0.0); gs.axis2 = float2(0.0, 0.0); gs.uvScale = 0.0; gs.color = float3(0.0, 0.0, 0.0); gs.alpha = 0.0; gs.disc = 0.0; gs.solid = 0.0;

    PackedSplat s = g_SplatBuffer[splatIndex];
    float3 pos = g_AABBMin + float3((s.w0 & 0xFFFFu) / 65535.0, ((s.w0 >> 16) & 0xFFFFu) / 65535.0, (s.w1 & 0xFFFFu) / 65535.0) * g_AABBExtents;
    float3 scaleBytes = float3((s.w1 >> 16) & 0xFFu, (s.w1 >> 24) & 0xFFu, s.w2 & 0xFFu);
    float3 scale = exp2(g_ScaleLog2Min + (scaleBytes / 255.0) * (g_ScaleLog2Max - g_ScaleLog2Min));
    float opacity = ((s.w2 >> 8) & 0xFFu) / 255.0;
    float3 dc = float3((s.w2 >> 16) & 0xFFu, (s.w2 >> 24) & 0xFFu, s.w3 & 0xFFu) / 255.0;
    uint flags = (s.w3 >> 8) & 0xFFu;
    float shScale = (flags & 1u) ? f16tof32(s.w3 >> 16) : 0.0;

    if (opacity <= 1.0 / 255.0) return false;

    // Centre: behind the camera is dropped; depth is clamped into the near/far range so the quad is never clipped
    float4 viewPos = mul(g_View, float4(pos, 1.0));
    if (viewPos.z > 0.0) return false;
    float4 clip = mul(g_ViewProj, float4(pos, 1.0));
    clip.z = clamp(clip.z, 0.0, abs(clip.w));

    // Coarse levels (kSplatDiscMinLevel and up): a camera-facing disc of the level's point spacing in
    // place of the projected Gaussian, so a coarse level has no Gaussian bloom past the model's rim.
    // The record's scale and rotation are not used. Both tangents are perpendicular to the view
    // direction, so every corner shares the centre's clip w and z and the corner placement in
    // GaussianCornerVertex (centre plus clip-space half-axes) is exact. A level without a measured
    // spacing (radius 0) falls through to the Gaussian.
    float discR = (lod >= kSplatDiscMinLevel && lod < 8) ? g_SplatDiscRadius[lod >> 2][lod & 3] : 0.0;
    if (discR > 0.0)
    {
        float3 tX = g_CamRight * discR;
        float3 tY = g_CamUp * discR;
        float solid;
        SolidDot(max(0.1f, clip.w), tX, tY, solid); // The disc modes' call: distance = the clip w (view depth)
        float2 axis1 = mul(g_ViewProj, float4(tX, 0.0)).xy;
        float2 axis2 = mul(g_ViewProj, float4(tY, 0.0)).xy;
        if (any((abs(clip.xy) - (abs(axis1) + abs(axis2))) > clip.w)) return false;

        gs.clipCenter = clip;
        gs.axis1 = axis1;
        gs.axis2 = axis2;
        gs.uvScale = 1.0;
        gs.color = SplatColor(splatIndex, pos, dc, shScale, wave);
        // Match Level 0 Softness: the discs (level 1 and up) take the opacity floor so a low-opacity record
        // does not read as a hole. A level 1 parent handing over to level 0 fades out by the chunk weight
        // (a sub-pixel disc drops its opaque-dot rule while fading so it can fade at all).
        float discAlpha = max(opacity, g_SplatDiscOpacityFloor);
        if (fade < 0.999) { discAlpha *= saturate(fade); solid = 0.0; }
        gs.alpha = discAlpha;
        gs.disc = 1.0;
        gs.solid = solid;
        return true;
    }

    // Level 1 -> 0 hand-over: a level 0 child's Gaussians fade in by the chunk weight; the alpha-dependent
    // quad shrink below trims the quad with it. Every other level 0 chunk (fade 1) keeps the record's opacity.
    if (fade < 0.999)
    {
        opacity *= saturate(fade);
        if (opacity <= 1.0 / 255.0) return false;
    }

    // Smallest-three quaternion
    uint largest = s.w4 & 3u;
    float comps[4] = { 0.0, 0.0, 0.0, 0.0 };
    float sumSq = 0.0;
    uint shift = 2;
    [unroll]
    for (uint i = 0; i < 4; i++)
    {
        if (i != largest)
        {
            float t = ((s.w4 >> shift) & 0x3FFu) / 1023.0;
            float v = (t * 2.0 - 1.0) * 0.70710678118;
            comps[i] = v;
            sumSq += v * v;
            shift += 10;
        }
    }
    comps[largest] = sqrt(saturate(1.0 - sumSq));
    float4 q = float4(comps[0], comps[1], comps[2], comps[3]);

    // 3D covariance in world (= model) space, then in view space
    float3x3 R = QuatToMat3(q);
    float3x3 M = mul(R, float3x3(scale.x, 0.0, 0.0, 0.0, scale.y, 0.0, 0.0, 0.0, scale.z));
    float3x3 Sigma = mul(M, transpose(M));
    float3x3 W = (float3x3)g_View;
    float3x3 Sv = mul(mul(W, Sigma), transpose(W));

    // Perspective Jacobian at the splat, focal length in pixels = viewport width * proj[0][0]
    float focal = g_Viewport.x * g_Proj00;
    float3 v = viewPos.xyz;
    float J1 = focal / v.z;
    float2 J2 = -J1 / v.z * v.xy;
    float3 r0 = float3(J1, 0.0, J2.x);
    float3 r1 = float3(0.0, J1, J2.y);
    float3 t0 = mul(Sv, r0);
    float3 t1 = mul(Sv, r1);
    float c00 = dot(r0, t0), c01 = dot(r0, t1), c11 = dot(r1, t1);

    float diagonal1 = c00 + 0.3;
    float offDiagonal = c01;
    float diagonal2 = c11 + 0.3;
    float mid = 0.5 * (diagonal1 + diagonal2);
    float radius = length(float2((diagonal1 - diagonal2) * 0.5, offDiagonal));
    float lambda1 = mid + radius;
    float lambda2 = max(mid - radius, 0.1);

    float vmin = min(1024.0, min(g_Viewport.x, g_Viewport.y));
    float l1 = 2.0 * min(sqrt(2.0 * lambda1), vmin);
    float l2 = 2.0 * min(sqrt(2.0 * lambda2), vmin);

    float2 c = clip.w / g_Viewport; // One pixel in clip units at this depth
    if (any((abs(clip.xy) - max(l1, l2) * c) > clip.w)) return false;

    float2 diagonalVector = float2(offDiagonal, lambda1 - diagonal1);
    float dvLen = length(diagonalVector);
    diagonalVector = (dvLen > 1e-6) ? diagonalVector / dvLen : float2(1.0, 0.0);
    float2 v1 = l1 * diagonalVector;
    float2 v2 = l2 * float2(diagonalVector.y, -diagonalVector.x);

    // Colour: DC plus the harmonics for this view direction, clamped at black
    float3 color = SplatColor(splatIndex, pos, dc, shScale, wave);

    // Shrink the quad to where the Gaussian times opacity falls below 1/255
    float clipS = min(1.0, sqrt(max(0.0, log(opacity * 255.0))) * 0.5);

    gs.clipCenter = clip;
    gs.axis1 = v1 * c * clipS;
    gs.axis2 = v2 * c * clipS;
    gs.uvScale = clipS;
    gs.color = color;
    gs.alpha = opacity;
    return true;
}

// The slot's Gaussian: sorted pair -> slot info -> chunk (blend weight for the LOD dither) -> splat.
bool SplatFromSlot(uint slot, out GaussianSplat gs, out float blendWeight)
{
    blendWeight = 1.0;
    gs.clipCenter = s_culledClipPos; gs.axis1 = float2(0.0, 0.0); gs.axis2 = float2(0.0, 0.0); gs.uvScale = 0.0; gs.color = float3(0.0, 0.0, 0.0); gs.alpha = 0.0; gs.disc = 0.0; gs.solid = 0.0;
    if (slot >= g_SplatSlotCount) return false;
    uint2 pair = g_SortedSplats[slot];
    if (pair.y >= g_SplatSlotCount) return false;
    uint2 info = g_SlotInfo[pair.y];
    if (info.y & SLOT_CULLED_BIT) return false;
    uint lod = 0;     // The chunk's level (0 when the pipeline is not chunked): coarse levels draw as discs
    float fade = 1.0; // The chunk's opacity weight for the level 1 -> 0 hand-over (1 when not chunked)
    float wave = 0.0; // The chunk's arrival-glow wave (0 when not chunked; see SplatColor)
    if (g_UseChunkedPipeline == 1 && info.y < g_TotalChunks)
    {
        MeshletChunk chunk = g_ChunkBuffer[info.y];
        blendWeight = chunk.blendWeight;
        lod = chunk.lodLevel;
        fade = chunk.dilationMorph;
        wave = chunk.isSilhouette;
        if (g_ShowOnlyLocked == 1 && !ChunkIsLocked(chunk)) return false;
    }
    if (info.x >= g_SurfelCount) return false;
    return BuildGaussianSplat(info.x, lod, fade, wave, gs);
}

VSOut GaussianCornerVertex(GaussianSplat gs, uint corner, float chunkBlendWeight)
{
    float2 cc = s_quadCorners[corner];
    VSOut o;
    o.pos = gs.clipCenter + float4(cc.x * gs.axis1 + cc.y * gs.axis2, 0.0, 0.0);
    o.uv = (cc * gs.uvScale) * 0.5 + 0.5;
    o.color = gs.color;
    o.norm = float3(0.0, 0.0, 0.0);
    o.blendWeight = chunkBlendWeight;
    o.isSil = 0.0;
    o.solid = gs.solid;
    o.culledTint = 0.0;
    o.splatAlpha = gs.alpha;
    o.disc = gs.disc;
    return o;
}

// =========================================================================
// Per-surfel item-prepass disc shared by itemMS and itemVS
// =========================================================================

void BuildItemSplat(uint surfelIndex, uint lod, out float3 worldPos, out float3 tangentX, out float3 tangentY)
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
        float splatRadius = AutoSplatRadius(lod, g_Radius * baseVoxelRadius * radScale, radScale);

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
        float splatRadius = AutoSplatRadius(lod, max(0.001f, s.radius * g_Radius), 1.0);
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

// Detach Camera keeps every chunk and marks the ones the frozen camera would have culled (frustum
// or normal cone) with this bit in the payload index; the mesh shader paints them flat dark grey.
#define CHUNK_FROZEN_CULLED_BIT 0x80000000u

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
    // The validator requires exactly one DispatchMesh per amplification shader, so every path below
    // decides a mesh-group count and the single call at the end issues it.
    uint meshGroups = 0;

    if (g_RenderMode == 3)
    {
        // Splat mode: one mesh group per 64 sorted slots (see SplatSortCS.hlsl); the chunk list only
        // contributes blend weights, culling was done per slot by the sort's expand pass. The dispatch
        // folds group counts above 65535 over y (see issueSplatDraws in SurfelsRenderer.cpp).
        uint linearGroup = groupId.x + groupId.y * 65535u;
        uint groupBase = linearGroup * SURFELS_PER_GROUP;
        if (threadId == 0) s_Payload.flatGroupIndex = linearGroup;
        meshGroups = (groupBase < g_SplatSlotCount) ? 1 : 0;
    }
    else if (g_UseChunkedPipeline == 0)
    {
        // Flat Buffer Mode: 1:1 pass-through to Mesh Shader
        uint groupBase = groupId.x * SURFELS_PER_GROUP;
        if (threadId == 0) s_Payload.flatGroupIndex = groupId.x;
        meshGroups = (groupBase < g_SurfelCount) ? 1 : 0;
    }
    else
    {
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

        // Detach Camera: nothing the frozen camera culled is dropped; it is flagged and drawn dark grey.
        if (g_UseDetachedCullCam == 1 && !isVisible)
        {
            isVisible = true;
            chunkIdx |= CHUNK_FROZEN_CULLED_BIT;
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

    if (threadId == 0) s_Payload.flatGroupIndex = 0;
    meshGroups = min(totalVisible, (uint)AS_GROUP_SIZE);
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(meshGroups, 1, 1, s_Payload);
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

    // The validator requires exactly one SetMeshOutputCounts per mesh shader: every path settles on a
    // count first, the single call below issues it, and a group with nothing to draw emits nothing.
    float chunkBlendWeight = 1.0;
    float chunkDilationMorph = 0.0;
    float chunkIsSilhouette = 0.0;
    uint chunkLod = 0xFF;
    bool frozenCulled = false;
    const bool splatMode = (g_RenderMode == 3);
    if (splatMode)
    {
        uint groupBase = payload.flatGroupIndex * SURFELS_PER_GROUP;
        uint remaining = groupBase < g_SplatSlotCount ? g_SplatSlotCount - groupBase : 0;
        groupSurfelCount = min((uint)SURFELS_PER_GROUP, remaining);
        surfelIndex = groupBase + threadId; // A slot, not a surfel index, in splat mode
    }
    else if (g_UseChunkedPipeline == 1)
    {
        uint pIdx = min(groupId.x, (uint)(AS_GROUP_SIZE - 1));
        uint chunkIdx = payload.chunkIndices[pIdx];
        frozenCulled = (chunkIdx & CHUNK_FROZEN_CULLED_BIT) != 0;
        chunkIdx &= ~CHUNK_FROZEN_CULLED_BIT;
        if (chunkIdx < g_TotalChunks)
        {
            MeshletChunk chunk = g_ChunkBuffer[chunkIdx];
            groupSurfelCount = min((uint)SURFELS_PER_GROUP, chunk.surfelCount);
            surfelIndex = chunk.surfelOffset + threadId;
            chunkBlendWeight = chunk.blendWeight;
            chunkDilationMorph = chunk.dilationMorph;
            chunkIsSilhouette = chunk.isSilhouette;
            chunkLod = chunk.lodLevel;
            if (g_ShowOnlyLocked == 1 && !ChunkIsLocked(chunk))
                groupSurfelCount = 0;
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

    if (threadId >= groupSurfelCount)
        return;

    uint vBase = threadId * 4;
    uint pBase = threadId * 2;

    if (splatMode)
    {
        GaussianSplat gs;
        float bw = 1.0;
        if (!SplatFromSlot(surfelIndex, gs, bw))
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
            verts[vBase + c] = GaussianCornerVertex(gs, c, bw);
        tris[pBase + 0] = uint3(vBase + 0, vBase + 1, vBase + 2);
        tris[pBase + 1] = uint3(vBase + 1, vBase + 3, vBase + 2);
        return;
    }

    SplatData sd;
    if (surfelIndex >= g_SurfelCount || !BuildSplat(surfelIndex, chunkLod, chunkBlendWeight, chunkDilationMorph, chunkIsSilhouette, frozenCulled, sd))
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
    uint itemLod = 0xFF;

    if (g_UseChunkedPipeline == 1)
    {
        if (chunkIndex < g_TotalChunks)
        {
            MeshletChunk c = g_ChunkBuffer[chunkIndex];
            // Fast normal-cone backface culling (see ChunkConeBackfacing for the close-range guard)
            if (!ChunkConeBackfacing(c, g_ViewerEyePos))
            {
                groupSurfelCount = min((uint)SURFELS_PER_GROUP, c.surfelCount);
                surfelIndex = c.surfelOffset + threadId;
                itemLod = c.lodLevel;
            }
        }
    }
    else
    {
        uint groupBase = chunkIndex * SURFELS_PER_GROUP;
        if (groupBase < g_SurfelCount)
            groupSurfelCount = min((uint)SURFELS_PER_GROUP, g_SurfelCount - groupBase);
        surfelIndex = groupBase + threadId;
        chunkIndex = 0;
    }

    // One SetMeshOutputCounts per mesh shader (validator rule); an empty group emits nothing.
    SetMeshOutputCounts(groupSurfelCount * 4, groupSurfelCount * 2);

    if (threadId >= groupSurfelCount || surfelIndex >= g_SurfelCount)
        return;

    float3 worldPos, tangentX, tangentY;
    BuildItemSplat(surfelIndex, itemLod, worldPos, tangentX, tangentY);

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
    if (g_RenderMode == 3)
    {
        // Splat mode: one instance per sorted slot
        GaussianSplat gs;
        float bw = 1.0;
        if (!SplatFromSlot(instanceId, gs, bw))
            return CulledSplatVertex();
        return GaussianCornerVertex(gs, s_quadVertexCorner[vertexId % 6], bw);
    }

    uint surfelIndex = 0;
    float chunkBlendWeight = 1.0;
    float chunkDilationMorph = 0.0;
    float chunkIsSilhouette = 0.0;
    uint chunkLod = 0xFF;
    bool frozenCulled = false;

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
        // The vertex-shader path never frustum-culled chunks (off-screen ones cost nothing); in Detach
        // Camera mode the frozen frustum matters, so test it there to know what to paint grey.
        bool chunkCulled = ChunkConeBackfacing(chunk, eyePos) || (g_UseDetachedCullCam == 1 && !ChunkInFrustum(chunk));
        if (chunkCulled)
        {
            if (g_UseDetachedCullCam == 1) frozenCulled = true; // Detach Camera: kept, painted dark grey
            else return CulledSplatVertex();
        }
        if (g_ShowOnlyLocked == 1 && !ChunkIsLocked(chunk))
            return CulledSplatVertex();
        surfelIndex = chunk.surfelOffset + local;
        chunkBlendWeight = chunk.blendWeight;
        chunkDilationMorph = chunk.dilationMorph;
        chunkIsSilhouette = chunk.isSilhouette;
        chunkLod = chunk.lodLevel;
    }
    else
    {
        surfelIndex = instanceId;
    }

    if (surfelIndex >= g_SurfelCount)
        return CulledSplatVertex();

    SplatData sd;
    if (!BuildSplat(surfelIndex, chunkLod, chunkBlendWeight, chunkDilationMorph, chunkIsSilhouette, frozenCulled, sd))
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
    uint itemLod = 0xFF;

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
        itemLod = c.lodLevel;
    }
    else
    {
        surfelIndex = instanceId;
        chunkIndex = 0;
    }

    if (surfelIndex >= g_SurfelCount)
        return culled;

    float3 worldPos, tangentX, tangentY;
    BuildItemSplat(surfelIndex, itemLod, worldPos, tangentX, tangentY);
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

    if (g_RenderMode == 3)
    {
        // Coarse-level disc (BuildGaussianSplat): the disc modes' falloff across the disc times the
        // record's opacity; a sub-pixel disc is an opaque dot. The d > 1 discard above clips it at the rim.
        // A sub-pixel disc (SolidDot) is an opaque dot so a far level covers as a surface.
        if (i.disc > 0.5 && i.solid > 0.5)
            return float4(i.color, 1.0);

        // Splat mode: the reference viewer's normalised falloff over the quad (A = |uv|^2 in the
        // shrunken corner space), reaching zero at the quad edge, times the Gaussian's opacity.
        // A coarse-level disc uses the same falloff: its quad is the disc itself (uvScale 1), so the
        // alpha reaches zero exactly at the disc edge with no rim ring and nothing past the radius.
        float alphaG = SplatFalloff(d) * i.splatAlpha;
        if (alphaG < 1.0 / 255.0)
            discard;
        return float4(i.color * alphaG, alphaG);
    }

    // Continuous 3D Gaussian falloff:
    // With back-to-front depth sorting, overlapping splats melt together into continuous, silky-smooth marble.
    // A sub-pixel splat (SolidDot) is an opaque dot instead, so a far level covers as a surface.
    float alpha = (i.solid > 0.5) ? 1.0 : saturate(exp(-2.5 * d) * 0.90);
    // Detach Camera: what the frozen camera would have culled is drawn see-through, 10% opacity per splat.
    if (i.culledTint > 0.5) alpha *= 0.10;

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
