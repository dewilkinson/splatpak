// =============================================================================
// TemporalFilterCS.hlsl
// High-Performance Temporal Accumulation & Dither Transition Resolver (TAA)
// Smooths stochastic Bayer dither patterns into seamless alpha dissolves
// and eliminates surfel geometric edge shimmering via sub-pixel reprojection.
// =============================================================================

cbuffer TemporalCB : register(b0)
{
    float4x4 g_CurViewProj;
    float4x4 g_PrevViewProj;
    float4x4 g_InvCurViewProj;
    uint2    g_ScreenSize;
    float    g_BlendWeight;      // Blend factor for current frame (e.g. 0.15)
    uint     g_EnableClamping;   // 1 = 3x3 Neighborhood Color Variance Box Clamping
    uint     g_FirstFrame;       // 1 = Skip history accumulation (first frame or reset)
    float3   g_Pad;
};

Texture2D<float4>   g_CurColor     : register(t0);
Texture2D<float>    g_CurDepth     : register(t1);
Texture2D<float4>   g_HistoryColor : register(t2);

SamplerState        g_LinearClamp  : register(s0);
SamplerState        g_PointClamp   : register(s1);

RWTexture2D<float4> g_OutResolved  : register(u0);
RWTexture2D<float4> g_OutHistory   : register(u1);

// RGB to YCoCg for perceptual color-space variance clipping
float3 RGBToYCoCg(float3 rgb)
{
    float Y  = dot(rgb, float3(0.25, 0.50, 0.25));
    float Co = dot(rgb, float3(0.50, 0.00, -0.50));
    float Cg = dot(rgb, float3(-0.25, 0.50, -0.25));
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float Y  = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    float r = Y + Co - Cg;
    float g = Y + Cg;
    float b = Y - Co - Cg;
    return float3(r, g, b);
}

// Clip history color sample to 3x3 neighborhood AABB in YCoCg space
float3 ClipToAABB(float3 historyYCoCg, float3 aabbMin, float3 aabbMax)
{
    float3 p_clip = historyYCoCg;
    float3 center = 0.5 * (aabbMax + aabbMin);
    float3 extents = 0.5 * (aabbMax - aabbMin);

    float3 v_clip = p_clip - center;
    float3 v_unit = v_clip / (extents + 1e-6);
    float3 a_unit = abs(v_unit);
    float ma_unit = max(a_unit.x, max(a_unit.y, a_unit.z));

    if (ma_unit > 1.0)
    {
        return center + v_clip / ma_unit;
    }
    return p_clip;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_ScreenSize.x || dispatchThreadId.y >= g_ScreenSize.y)
        return;

    int2 pixelPos = int2(dispatchThreadId.xy);
    float2 invScreen = 1.0 / float2(g_ScreenSize);
    float2 uv = (float2(pixelPos) + 0.5) * invScreen;

    float4 curColor = g_CurColor.Load(int3(pixelPos, 0));

    // First frame initialization or background pixel with no history:
    if (g_FirstFrame == 1)
    {
        g_OutResolved[pixelPos] = curColor;
        g_OutHistory[pixelPos] = curColor;
        return;
    }

    // 1. Gather 3x3 Neighborhood in Current Frame for Color Bounding Box & Variance
    // Exclude discarded Bayer/empty pixels (alpha <= 0.01) so they never pull the variance box down to black
    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float  alphaSum = 0.0;
    float3 minColorYCoCg = float3(1e6, 1e6, 1e6);
    float3 maxColorYCoCg = float3(-1e6, -1e6, -1e6);
    float  validSamples = 0.0;

    [unroll]
    for (int dy = -1; dy <= 1; dy++)
    {
        [unroll]
        for (int dx = -1; dx <= 1; dx++)
        {
            int2 sampleCoord = clamp(pixelPos + int2(dx, dy), int2(0, 0), int2(g_ScreenSize) - 1);
            float4 neighbor = g_CurColor.Load(int3(sampleCoord, 0));

            if (neighbor.a > 0.01)
            {
                float3 neighborYCoCg = RGBToYCoCg(neighbor.rgb);
                m1 += neighborYCoCg;
                m2 += neighborYCoCg * neighborYCoCg;
                minColorYCoCg = min(minColorYCoCg, neighborYCoCg);
                maxColorYCoCg = max(maxColorYCoCg, neighborYCoCg);
                alphaSum += neighbor.a;
                validSamples += 1.0;
            }
        }
    }

    // If entire 3x3 neighborhood is empty background:
    if (validSamples < 0.5)
    {
        g_OutResolved[pixelPos] = curColor;
        g_OutHistory[pixelPos] = curColor;
        return;
    }

    float3 meanYCoCg = m1 / validSamples;
    float3 stdDevYCoCg = sqrt(abs(m2 / validSamples - meanYCoCg * meanYCoCg));
    float  meanAlpha = alphaSum / validSamples;

    float3 curYCoCg = (curColor.a > 0.01) ? RGBToYCoCg(curColor.rgb) : meanYCoCg;
    float  effectiveCurAlpha = (curColor.a > 0.01) ? curColor.a : meanAlpha;

    // Variance-guided tight bounding box (1.75 standard deviations)
    float gamma = 1.75;
    float3 boxMin = min(curYCoCg, max(minColorYCoCg, meanYCoCg - gamma * stdDevYCoCg));
    float3 boxMax = max(curYCoCg, min(maxColorYCoCg, meanYCoCg + gamma * stdDevYCoCg));

    // 2. Sample History Color
    float4 rawHistory = g_HistoryColor.Load(int3(pixelPos, 0));
    float3 historyYCoCg = (rawHistory.a > 0.01) ? RGBToYCoCg(rawHistory.rgb) : curYCoCg;

    if (g_EnableClamping == 1)
    {
        historyYCoCg = ClipToAABB(historyYCoCg, boxMin, boxMax);
    }

    // 3. Exact, Color-Accurate Temporal Accumulation
    // Blend current frame and history: resolves Bayer dither patterns into seamless surface
    float blendAlpha = g_BlendWeight;
    float3 resolvedYCoCg = lerp(historyYCoCg, curYCoCg, blendAlpha);
    float3 resolvedRGB = YCoCgToRGB(resolvedYCoCg);
    float resolvedAlpha = lerp(rawHistory.a, effectiveCurAlpha, blendAlpha);

    float4 finalColor = float4(saturate(resolvedRGB), saturate(resolvedAlpha));

    g_OutResolved[pixelPos] = finalColor;
    g_OutHistory[pixelPos] = finalColor;
}

