// Backdrop.hlsl
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The scene's backdrop (SplatRenderer, State::backdropMode), drawn after the clear and before everything else: a vertical
// gradient from the top colour to the bottom one, or an image scaled to cover the view (centred, cropped, aspect kept).
// A solid colour needs no pass: it is the clear. Colours and the image are display-space (sRGB); on an sRGB target the
// output is converted to linear so the hardware's encode gives the same display colour back.

cbuffer BackdropCB : register(b0)
{
    float4 g_Top;           // rgb, display space
    float4 g_Bottom;
    uint   g_Mode;          // 1 gradient, 2 image
    uint   g_LinearOut;     // 1 = the target is an sRGB view
    float  g_ScreenAspect;  // width / height
    float  g_ImageAspect;
};

Texture2D<float4> g_Image   : register(t0);
SamplerState      g_Linear  : register(s0);

struct BackdropVSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;   // 0,0 top left
};

// One triangle covering the screen
BackdropVSOut backdropVS(uint id : SV_VertexID)
{
    BackdropVSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return o;
}

float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float4 backdropPS(BackdropVSOut i) : SV_Target0
{
    float3 c;
    if (g_Mode == 2)
    {
        // Cover: the image fills the view, cropped along whichever axis it is longer
        float2 uv = i.uv;
        if (g_ScreenAspect > g_ImageAspect) uv.y = 0.5 + (uv.y - 0.5) * g_ImageAspect / g_ScreenAspect;
        else uv.x = 0.5 + (uv.x - 0.5) * g_ScreenAspect / g_ImageAspect;
        c = g_Image.Sample(g_Linear, uv).rgb;
    }
    else
    {
        c = lerp(g_Top.rgb, g_Bottom.rgb, saturate(i.uv.y));
    }
    return float4(g_LinearOut != 0 ? SrgbToLinear(c) : c, 1.0);
}
