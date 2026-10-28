// Grid.hlsl
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The editor grid, in the style of Unreal Engine's viewport: the horizontal plane at g_GridHeight, the scene's floor (a
// visual reference, so the model stands on it wherever its origin is), drawn per pixel on one
// large quad centred under the camera. Lines at every power of ten from a decade below the camera's height, each decade
// fading in as its cells grow past a few pixels (so the grid never shimmers and always shows a sensible scale); the X
// axis (z = 0) in red and the Z axis (x = 0) in green, Unreal's colours for its two ground axes; everything fading out
// with distance. Depth-tested against what the occluders wrote, not depth-writing, drawn before the splats so they
// blend over it. A classic vertex/pixel pipeline with no input layout, like ShadowMesh.hlsl, so it runs on every path.

// The members of SplatCB (Splat.hlsl) read here, at their offsets (static_asserts in SplatRenderer.h hold them there)
cbuffer SplatCB : register(b0)
{
    float4x4 g_ViewProj     : packoffset(c0);
    float3   g_CamRight     : packoffset(c4);
    float    g_Radius       : packoffset(c4.w);
    float3   g_CamUp        : packoffset(c5);
    float    g_Time         : packoffset(c5.w);
    float3   g_ViewerEyePos : packoffset(c6);
    float    g_GridHeight   : packoffset(c18.x);
};

struct GridVSOut
{
    float4 pos   : SV_Position;
    float3 world : TEXCOORD0;
};

// How far the grid reaches: a multiple of the camera's distance from the origin (and never under 10 m)
float GridReach() { return max(10.0, 6.0 * length(g_ViewerEyePos)); }

// Two triangles covering the reach around the point under the camera
GridVSOut gridVS(uint vertexId : SV_VertexID)
{
    static const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    const float reach = GridReach();
    const float2 xz = g_ViewerEyePos.xz + corners[vertexId] * reach;
    GridVSOut o;
    o.world = float3(xz.x, g_GridHeight, xz.y);
    o.pos = mul(g_ViewProj, float4(o.world, 1.0));
    return o;
}

// Coverage of the lines every `spacing` metres along both axes: one pixel wide, anti-aliased
float LineCoverage(float2 p, float spacing)
{
    const float2 g = p / spacing;
    const float2 w = max(fwidth(g), 1e-6);
    const float2 d = abs(frac(g - 0.5) - 0.5) / w;
    return 1.0 - saturate(min(d.x, d.y));
}

// How visible a decade is from the size of its cells on screen: hidden below 4 pixels, full from 16
float DecadeVisibility(float2 p, float spacing)
{
    const float2 w = fwidth(p / spacing);
    const float pixelsPerCell = 1.0 / max(max(w.x, w.y), 1e-6);
    return saturate((pixelsPerCell - 4.0) / 12.0);
}

float4 gridPS(GridVSOut i) : SV_Target0
{
    // Seen from below the floor the grid would lie in front of the model, yet the splats are drawn over it, so it seemed to
    // float above the model: from below there is no grid
    if (g_ViewerEyePos.y < g_GridHeight) discard;
    const float2 p = i.world.xz;
    // The finest decade: a tenth of the camera's height above the plane (at least a millimetre)
    const float base = pow(10.0, floor(log10(max(abs(g_ViewerEyePos.y - g_GridHeight), 0.01)))) * 0.1;
    float alpha = 0.0;
    float spacing = base;
    [unroll] for (int k = 0; k < 4; k++)
    {
        // Coarser decades are brighter: every tenth line stands out, as Unreal's major lines do
        const float strength = 0.18 + 0.14 * k;
        alpha = max(alpha, LineCoverage(p, spacing) * DecadeVisibility(p, spacing) * strength);
        spacing *= 10.0;
    }
    float3 colour = float3(0.62, 0.62, 0.66);

    // The axes, a pixel and a half wide
    const float2 w = max(fwidth(p), 1e-6);
    const float onX = 1.0 - saturate(abs(p.y) / w.y - 0.5); // z = 0: the X axis
    const float onZ = 1.0 - saturate(abs(p.x) / w.x - 0.5); // x = 0: the Z axis
    if (onX > 0.0) { colour = lerp(colour, float3(0.85, 0.22, 0.20), onX); alpha = max(alpha, 0.85 * onX); }
    if (onZ > 0.0) { colour = lerp(colour, float3(0.30, 0.78, 0.25), onZ); alpha = max(alpha, 0.85 * onZ); }

    // Fades out towards the edge of the reach
    const float d = length(p - g_ViewerEyePos.xz);
    alpha *= 1.0 - smoothstep(0.35, 1.0, d / GridReach());
    if (alpha <= 0.002) discard;
    return float4(colour, alpha);
}
