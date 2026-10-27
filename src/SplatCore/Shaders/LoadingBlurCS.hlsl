// LoadingBlurCS.hlsl
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The loading veil (SplatRenderer, State::loadingBlurPx): one direction of a separable Gaussian blur of the finished
// scene, run twice (across, then down) while a scene's first levels and its residency ring are still arriving, so the
// coarse levels read as soft rather than as specks and blobs. Edges clamp. The radius is three sigmas.

cbuffer BlurCB : register(b0)
{
    uint2 g_Size;       // The image, pixels
    int   g_Radius;     // Taps each side
    uint  g_Horizontal; // 1 = across, 0 = down
    float g_Sigma;      // Pixels
};

Texture2D<float4>   g_In  : register(t0);
RWTexture2D<float4> g_Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Size.x || id.y >= g_Size.y) return;
    const int2 step = g_Horizontal != 0 ? int2(1, 0) : int2(0, 1);
    const float k = -0.5 / max(1e-3, g_Sigma * g_Sigma);
    float4 sum = 0.0;
    float weights = 0.0;
    for (int t = -g_Radius; t <= g_Radius; t++)
    {
        const int2 p = clamp(int2(id.xy) + step * t, int2(0, 0), int2(g_Size) - 1);
        const float w = exp(k * (float)(t * t));
        sum += g_In[p] * w;
        weights += w;
    }
    g_Out[id.xy] = sum / weights;
}
