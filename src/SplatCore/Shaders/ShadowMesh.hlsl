// ShadowMesh.hlsl
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The View Shadow Mesh debug view: the package's shadow mesh (ShadowMesh.h) as a solid, flat-shaded surface in its
// vertex colours (the splats' base colour), lit like the occlusion volume's cubes. A classic vertex/pixel pipeline with
// no input layout, so it runs on every render path (mesh shaders, VS SM6, and VS SM5 under FXC): the vertex shader
// fetches the index list and the vertices from structured buffers bound at t0 and t1 of the shared root signature.

// The leading members of SplatCB (Splat.hlsl); only these are read here
cbuffer SplatCB : register(b0)
{
    float4x4 g_ViewProj;
    float3   g_CamRight;
    float    g_Radius;
    float3   g_CamUp;
    float    g_Time;
    float3   g_ViewerEyePos;
};

struct ShadowMeshVertex
{
    float3 position;
    uint   colour; // 0xAABBGGRR
};

StructuredBuffer<uint>             g_MeshIndices  : register(t0);
StructuredBuffer<ShadowMeshVertex> g_MeshVertices : register(t1);

struct ShadowMeshVSOut
{
    float4 pos   : SV_Position;
    float3 world : TEXCOORD0;
    float3 color : COLOR0;
};

ShadowMeshVSOut shadowMeshVS(uint vertexId : SV_VertexID)
{
    const ShadowMeshVertex v = g_MeshVertices[g_MeshIndices[vertexId]];
    ShadowMeshVSOut o;
    o.world = v.position;
    o.pos = mul(g_ViewProj, float4(v.position, 1.0));
    o.color = float3(v.colour & 0xFF, (v.colour >> 8) & 0xFF, (v.colour >> 16) & 0xFF) / 255.0;
    return o;
}

float4 shadowMeshPS(ShadowMeshVSOut i) : SV_Target0
{
    // Flat shading: the face normal from the screen-space derivatives of the world position
    const float3 n = normalize(cross(ddx(i.world), ddy(i.world)));
    const float3 lightDir = normalize(float3(0.5, 0.8, 0.6));
    const float lighting = 0.35 + 0.65 * abs(dot(n, lightDir));
    return float4(i.color * lighting, 1.0);
}
