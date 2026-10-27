// SplatComposite.hlsl
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Opaque Front Surface (SplatRenderer, State::opaqueSurface): the splats are blended into a layer of their own, cleared
// to transparent black, and this pass lays it over the scene with the same premultiplied blend. Where something has
// written depth behind a pixel -- the occlusion volume inside the model -- a layer that is already mostly covered is
// taken as the surface itself: its colour is divided by its coverage and it goes on opaque, so the occluder's flat
// colour no longer shows through a thin front shell (the top of a head seen edge-on). The ramp from g_Lo to g_Hi
// coverage keeps faint splats in front of the occluder see-through. Elsewhere (silhouettes over the backdrop) the layer
// is laid on exactly as the splats would have blended.

cbuffer CompositeCB : register(b0)
{
    float g_Lo;       // Coverage at which the layer starts to be made opaque
    float g_Hi;       // Coverage from which it is fully opaque
    uint  g_LinearIn; // 1 = the layer was blended through an sRGB view: its stored colour is encoded
    uint  g_Debug;    // Mask: 1 = over the occlusion volume, how much of it shows through the splats (1 - coverage, black none ..
                      // red all, linear); 2 = elsewhere, the splats' coverage in green
};

Texture2D<float4> g_Layer : register(t0);
Texture2D<float>  g_Depth : register(t1);

struct CompositeVSOut
{
    float4 pos : SV_Position;
};

// One triangle covering the screen
CompositeVSOut compositeVS(uint id : SV_VertexID)
{
    CompositeVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return o;
}

float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float4 compositePS(CompositeVSOut i) : SV_Target0
{
    const int3 p = int3(i.pos.xy, 0);
    float4 s = g_Layer.Load(p);
    const bool overOccluder = g_Depth.Load(p) < 1.0;
    if ((g_Debug & 1u) != 0 && overOccluder)
    {
        // Show-through over the occluder; with Opaque Front Surface, the ramp's ends as thin blue bands
        const float t = 1.0 - s.a;
        float3 c = float3(t, 0.0, 0.0);
        if (abs(s.a - g_Lo) < 0.01 || abs(s.a - g_Hi) < 0.01) c = float3(0.0, 0.6, 1.0);
        return float4(saturate(c), 1.0);
    }
    if ((g_Debug & 2u) != 0 && !overOccluder && s.a > 0.0) return float4(0.0, s.a, 0.0, 1.0); // Elsewhere the coverage, in green
    if (s.a <= 0.0) discard;
    if (g_LinearIn != 0) s.rgb = SrgbToLinear(s.rgb);
    const float k = g_Depth.Load(p) < 1.0 ? smoothstep(g_Lo, g_Hi, s.a) : 0.0;
    return float4(lerp(s.rgb, s.rgb / s.a, k), lerp(s.a, 1.0, k));
}
