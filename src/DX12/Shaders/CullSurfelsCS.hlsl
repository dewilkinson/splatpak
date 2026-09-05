// CullSurfelsCS.hlsl
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// GPU frustum culling and 32-bit depth key generation compute shader.

cbuffer CullCB : register(b0)
{
    float4x4 g_ViewProj;
    float3   g_CamPos;
    float    g_Pad0;
    float3   g_CamForward;
    uint     g_TotalSurfels;
    float3   g_AABBMin;
    float    g_Pad1;
    float3   g_AABBExtents;
    float    g_Pad2;
    float4   g_FrustumPlanes[6]; // Left, Right, Bottom, Top, Near, Far
    uint     g_RenderMode;       // 1 = Quantized 8-Byte, 2 = Raw Float32
    uint3    g_Pad3;
};

struct PackedSurfel
{
    uint packedPosRadius;   // 10:10:10:2 position & radius exponent
    uint packedNormalColor; // Oct16 normal & RGB565 color
};

struct RawSurfel
{
    float3 position;
    float3 normal;
    float3 color;
    float  radius;
};

StructuredBuffer<PackedSurfel> g_InputSurfels    : register(t0);
StructuredBuffer<RawSurfel>    g_InputRawSurfels : register(t1);

RWStructuredBuffer<uint>        g_VisibleIndices : register(u0);
RWStructuredBuffer<uint>        g_DepthKeys      : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= g_TotalSurfels)
        return;

    float3 worldPos = float3(0, 0, 0);

    if (g_RenderMode == 2)
    {
        // Mode 2: Raw Float32 Points
        RawSurfel rawS = g_InputRawSurfels[index];
        worldPos = rawS.position;
    }
    else
    {
        // Mode 1: Packed 8-byte Quantized Surfels
        PackedSurfel s = g_InputSurfels[index];
        uint qx = s.packedPosRadius & 0x3FF;
        uint qy = (s.packedPosRadius >> 10) & 0x3FF;
        uint qz = (s.packedPosRadius >> 20) & 0x3FF;
        worldPos = g_AABBMin + float3(qx / 1023.0, qy / 1023.0, qz / 1023.0) * g_AABBExtents;
    }

    // Projected distance along camera forward vector
    float d = dot(worldPos - g_CamPos, g_CamForward);

    // Initial indices & 32-bit depth keys (inverted for descending back-to-front depth sort)
    g_VisibleIndices[index] = index;

    // Normalizing depth key into full 32-bit unsigned range
    // Higher key = farther from camera = rendered first
    float normD = saturate((d + 100.0f) / 1000.0f);
    uint key = (uint)(normD * 4294967295.0f);
    g_DepthKeys[index] = 0xFFFFFFFFu - key;
}

