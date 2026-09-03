// SilhouetteEdgeExtractCS.hlsl
// GPU Item Buffer & Depth Discontinuity Edge Inversion Compute Shader
// DirectX 12 Compute Shader 6.0

cbuffer EdgeExtractCB : register(b0)
{
    uint2 g_ScreenSize;
    float g_DepthThreshold; // Depth step threshold for interior occlusion detection (e.g. 0.05f)
    uint  g_TotalChunks;
    uint  g_BitmaskDwordCount;
    uint  g_ExteriorOnly;   // 1 = only outer perimeter against background, 0 = include interior occlusion
    float2 g_Pad;
};

Texture2D<uint>            g_ChunkIdTex        : register(t0);
Texture2D<float>           g_DepthTex          : register(t1);
RWStructuredBuffer<uint>   g_SilhouetteBitmask : register(u0);

[numthreads(64, 1, 1)]
void clearBitmaskCS(uint id : SV_DispatchThreadID)
{
    if (id < g_BitmaskDwordCount)
    {
        g_SilhouetteBitmask[id] = 0;
    }
}

[numthreads(16, 16, 1)]
void mainCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_ScreenSize.x || dispatchThreadId.y >= g_ScreenSize.y)
        return;

    int2 pixelPos = int2(dispatchThreadId.xy);
    uint centerId = g_ChunkIdTex[pixelPos];

    // 0 = Background / empty space
    if (centerId == 0)
        return;

    float centerDepth = g_DepthTex[pixelPos];
    bool isEdge = false;

    // 4-neighborhood cardinal offsets (North, South, East, West)
    const int2 kOffsets[4] = {
        int2(0, -1),
        int2(0,  1),
        int2(-1, 0),
        int2( 1, 0)
    };

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        int2 neighborPos = pixelPos + kOffsets[i];
        if (neighborPos.x < 0 || neighborPos.x >= (int)g_ScreenSize.x ||
            neighborPos.y < 0 || neighborPos.y >= (int)g_ScreenSize.y)
        {
            // Border of the screen is an edge
            isEdge = true;
            break;
        }

        uint neighborId = g_ChunkIdTex[neighborPos];
        float neighborDepth = g_DepthTex[neighborPos];

        // 1. Outer Silhouette: neighbor is empty background
        if (neighborId == 0)
        {
            isEdge = true;
            break;
        }

        // 2. Interior Occluding Silhouette: neighbor is another chunk with depth step discontinuity (if not exterior only)
        if (g_ExteriorOnly == 0 && neighborId != centerId && abs(centerDepth - neighborDepth) > g_DepthThreshold)
        {
            isEdge = true;
            break;
        }
    }

    if (isEdge)
    {
        uint chunkIdx = centerId - 1; // 1-based chunk ID to 0-based index
        if (chunkIdx < g_TotalChunks)
        {
            uint dwordIdx = chunkIdx / 32;
            uint bitIdx = chunkIdx % 32;
            if (dwordIdx < g_BitmaskDwordCount)
            {
                InterlockedOr(g_SilhouetteBitmask[dwordIdx], 1u << bitIdx);
            }
        }
    }
}
