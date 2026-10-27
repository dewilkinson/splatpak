// ShadowMesh.hlsl
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The View Shadow Mesh debug view: the package's shadow mesh (ShadowMesh.h) as a solid, smooth-shaded surface in its
// vertex colours (the splats' base colour), lit like the occlusion volume's cubes; also the occluder mesh and the gap
// fill. SplatPak subdivides the mesh and gives each vertex a smooth normal (ShadowMeshDisplay.h). A classic vertex/pixel pipeline with
// no input layout, so it runs on every render path (mesh shaders, VS SM6, and VS SM5 under FXC): the vertex shader
// fetches the index list and the vertices from structured buffers bound at t0 and t1 of the shared root signature.

// The members of SplatCB (Splat.hlsl) read here, at their offsets (g_OccluderPush: SplatRenderer.h asserts byte 500)
cbuffer SplatCB : register(b0)
{
    float4x4 g_ViewProj      : packoffset(c0);
    float3   g_CamRight      : packoffset(c4);
    float    g_Radius        : packoffset(c4.w);
    float3   g_CamUp         : packoffset(c5);
    float    g_Time          : packoffset(c5.w);
    float3   g_ViewerEyePos  : packoffset(c6);
    float    g_OccluderPush  : packoffset(c31.y);
};

struct ShadowMeshVertex
{
    float3 position;
    uint   colour; // 0xAABBGGRR
    uint   normal; // Octahedral, two snorm16 (x low, y high)
};

StructuredBuffer<uint>             g_MeshIndices  : register(t0);
StructuredBuffer<ShadowMeshVertex> g_MeshVertices : register(t1);

struct ShadowMeshVSOut
{
    float4 pos   : SV_Position;
    float3 world : TEXCOORD0;
    float3 color : COLOR0;
    float3 normal : NORMAL0;
};

float3 DecodeOctNormal(uint packed)
{
    float2 e = float2((float)(int)(packed << 16) / 2147418112.0, (float)(int)(packed & 0xFFFF0000u) / 2147418112.0);
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

ShadowMeshVSOut shadowMeshVS(uint vertexId : SV_VertexID)
{
    const ShadowMeshVertex v = g_MeshVertices[g_MeshIndices[vertexId]];
    ShadowMeshVSOut o;
    o.world = v.position;
    o.pos = mul(g_ViewProj, float4(v.position, 1.0));
    o.color = float3(v.colour & 0xFF, (v.colour >> 8) & 0xFF, (v.colour >> 16) & 0xFF) / 255.0;
    o.normal = v.normal != 0 ? DecodeOctNormal(v.normal) : float3(0, 0, 0);
    return o;
}

// The occluder mesh under the splats: pushed g_OccluderPush back along the view ray. A splat is drawn as a camera-facing
// quad at its centre's depth, so where the inset occluder is seen edge-on (the top of a head) its near surface can lie in
// front of a front splat's centre over part of that quad: the depth test cut holes in the shell there, and the occluder
// showed in them. The push keeps it behind the shell's splats; the far side stays far behind it.
ShadowMeshVSOut occluderMeshVS(uint vertexId : SV_VertexID)
{
    ShadowMeshVSOut o = shadowMeshVS(vertexId);
    const float3 pushed = o.world + normalize(o.world - g_ViewerEyePos) * g_OccluderPush;
    o.pos = mul(g_ViewProj, float4(pushed, 1.0));
    return o;
}

float4 shadowMeshPS(ShadowMeshVSOut i) : SV_Target0
{
    // Smooth shading from the interpolated vertex normal; the face normal where a vertex has none
    const float len = length(i.normal);
    const float3 n = len > 1e-4 ? i.normal / len : normalize(cross(ddx(i.world), ddy(i.world)));
    const float3 lightDir = normalize(float3(0.5, 0.8, 0.6));
    const float lighting = 0.35 + 0.65 * abs(dot(n, lightDir));
    return float4(i.color * lighting, 1.0);
}

// The occluder and the gap fill under the splats: the splats' base colour, unlit. Where the front splats leave some
// transmittance, this shows through in place of the far side (the same surface colour); shaded, it read as dark
// patches with the mesh's triangles in them.
float4 shadowMeshFlatPS(ShadowMeshVSOut i) : SV_Target0
{
    return float4(i.color, 1.0);
}
