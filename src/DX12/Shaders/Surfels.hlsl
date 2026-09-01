// Surfels.hlsl - mesh-shader instanced point-splat renderer.
//
// Each "surfel" is a camera-facing quad billboard placed on a Fibonacci sphere.
// The mesh shader (mainMS) builds one small batch of surfels per threadgroup
// entirely from SV_GroupID/SV_GroupThreadID -- there are no vertex/index buffers,
// and unlike the earlier DrawInstanced version there's no draw-level instancing
// either: DispatchMesh() launches one threadgroup per SURFELS_PER_GROUP surfels,
// and each thread in the group emits its own quad (4 vertices, 2 triangles)
// directly into the group's shared output arrays.

#define SURFELS_PER_GROUP 32

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
    float3   g_Pad;
};

struct VSOut
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float3 color : COLOR0;
};

float3 HashColor(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    float h = frac(float(seed) * 2.3283064365386963e-10);
    float3 c = saturate(abs(frac(h + float3(0.0, 1.0 / 3.0, 2.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return lerp(float3(1.0, 1.0, 1.0), c, 0.85);
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

    // Must be called by every thread with the same (group-uniform) value before
    // any thread writes to verts/tris.
    SetMeshOutputCounts(groupSurfelCount * 4, groupSurfelCount * 2);

    if (threadId >= groupSurfelCount)
        return;

    uint surfelIndex = groupBase + threadId;
    float3 dir = FibonacciSpherePoint(surfelIndex, g_SurfelCount);
    float pulse = 0.02 * sin(g_Time * 2.0 + (float)surfelIndex);
    float3 worldPos = g_SphereCenter + dir * (g_SphereRadius + pulse);
    float3 color = HashColor(surfelIndex);

    // Triangle-strip-style quad corners: 0(-1,-1) 1(1,-1) 2(-1,1) 3(1,1)
    float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };

    uint vBase = threadId * 4;
    [unroll]
    for (uint c = 0; c < 4; c++)
    {
        float3 offset = (g_CamRight * corners[c].x + g_CamUp * corners[c].y) * g_Radius;

        VSOut o;
        o.pos = mul(g_ViewProj, float4(worldPos + offset, 1.0));
        o.uv = corners[c] * 0.5 + 0.5;
        o.color = color;
        verts[vBase + c] = o;
    }

    uint pBase = threadId * 2;
    tris[pBase + 0] = uint3(vBase + 0, vBase + 1, vBase + 2);
    tris[pBase + 1] = uint3(vBase + 1, vBase + 3, vBase + 2);
}

float4 mainPS(VSOut i) : SV_Target
{
    float2 centered = i.uv * 2.0 - 1.0;
    float d = dot(centered, centered);
    if (d > 1.0)
        discard;
    float shade = saturate(1.0 - d);
    return float4(i.color * (0.35 + 0.65 * shade), 1.0);
}
