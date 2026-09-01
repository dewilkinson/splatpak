// Surfels.hlsl - Modern DirectX 12 Mesh Shader Wavelet & Procedural Surfel Renderer
//
// Supports both:
//  1. Procedural Fibonacci-sphere point-splat generation on-chip.
//  2. High-performance progressive streaming from a StructuredBuffer of 8-byte PackedSurfel structs.
//  3. Normal-oriented tangent-plane discs or camera-facing billboard quads.

#define SURFELS_PER_GROUP 32

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

StructuredBuffer<PackedSurfel> g_SurfelBuffer    : register(t0);
StructuredBuffer<RawSurfel>    g_RawSurfelBuffer : register(t1);

cbuffer SurfelsCB : register(b0)
{
    float4x4 g_ViewProj;
    float3   g_CamRight;
    float    g_Radius;
    float3   g_CamUp;
    float    g_Time;
    float3   g_SphereCenter;
    float    g_SphereRadius;
    uint     g_SurfelCount;
    uint     g_RenderMode; // 0 = Procedural Sphere, 1 = Quantized 8-Byte, 2 = Raw Float32 Points
    uint     g_OrientMode; // 0 = Normal-Oriented Discs, 1 = Camera-Facing Billboards
    float    g_Pad0;
    float3   g_AABBMin;
    float    g_Pad1;
    float3   g_AABBExtents;
    float    g_Pad2;
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

float3 UnpackColorRGB565(uint packedColor)
{
    float r = (float)((packedColor >> 11) & 0x1F) / 31.0;
    float g = (float)((packedColor >> 5) & 0x3F) / 63.0;
    float b = (float)(packedColor & 0x1F) / 31.0;
    return float3(r, g, b);
}

float3 HashColor(uint id)
{
    uint n = id * 1664525u + 1013904223u;
    n = (n ^ (n >> 16)) * 1664525u;
    n = (n ^ (n >> 16)) * 1664525u;
    return float3(
        (float)(n & 0xFF) / 255.0,
        (float)((n >> 8) & 0xFF) / 255.0,
        (float)((n >> 16) & 0xFF) / 255.0
    );
}

float3 FibonacciSpherePoint(uint i, uint n)
{
    float goldenAngle = 3.14159265359 * (3.0 - sqrt(5.0));
    float t = (float)i / max((float)n, 1.0);
    float y = 1.0 - 2.0 * t;
    float r = sqrt(saturate(1.0 - y * y));
    float theta = goldenAngle * (float)i;
    return float3(cos(theta) * r, y, sin(theta) * r);
}

// =========================================================================
// Mesh Shader Stage (mainMS)
// =========================================================================

[NumThreads(SURFELS_PER_GROUP, 1, 1)]
[OutputTopology("triangle")]
void mainMS(
    uint3 groupId  : SV_GroupID,
    uint  threadId : SV_GroupThreadID,
    out indices  uint3 tris[SURFELS_PER_GROUP * 2],
    out vertices VSOut verts[SURFELS_PER_GROUP * 4])
{
    uint groupBase = groupId.x * SURFELS_PER_GROUP;
    uint remaining = groupBase < g_SurfelCount ? g_SurfelCount - groupBase : 0;
    uint groupSurfelCount = min((uint)SURFELS_PER_GROUP, remaining);

    SetMeshOutputCounts(groupSurfelCount * 4, groupSurfelCount * 2);

    if (threadId >= groupSurfelCount)
        return;

    uint surfelIndex = groupBase + threadId;
    float3 worldPos;
    float3 normal;
    float3 color;
    float3 tangentX, tangentY;

    if (g_RenderMode == 0)
    {
        // 1. Procedural Fibonacci Sphere
        float3 dir = FibonacciSpherePoint(surfelIndex, g_SurfelCount);
        float pulse = 0.02 * sin(g_Time * 2.0 + (float)surfelIndex);
        worldPos = g_SphereCenter + dir * (g_SphereRadius + pulse);
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
