// WaveletTypes.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Shared data types for the wavelet streaming pipeline: the .sflw file format (magic/
// version/header), GPU-packed surfel and meshlet-chunk structs, and the pack/unpack
// helpers that convert between them. Included by every preprocessor and viewer file
// that touches surfel or chunk data, on both the CPU and (conceptually) GPU side.

#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace Surfels
{
    using namespace DirectX;

    // Shared by every raw point-cloud loader (PLYLoader, SPLATLoader): source coordinates farther than
    // this from the origin trigger origin-shifting so downstream single-precision float math doesn't
    // lose precision on real-world geospatial datasets (e.g. absolute UTM/ECEF coordinates).
    static constexpr double kGeospatialOffsetThreshold = 10000.0;

    // Magic bytes for .sflw binary stream container ("SFLW" in ASCII)
    static constexpr uint32_t SFLW_MAGIC = 0x574C4653;
    static constexpr uint32_t SFLW_VERSION = 3; // v2 adds SFLWFileHeader::splatRadius (appended at the
                                                 // struct's end so v1 files still read correctly -- see
                                                 // the version check in StreamPackager::LoadPackage).
                                                 // v3 adds an optional occlusion voxel array, appended
                                                 // after all chunk LOD data; occlusionVoxelCount/Offset
                                                 // are 0 on v1/v2 files and on v3 files that simply chose
                                                 // not to generate one.

    // A single solid occluder cube: interior/enclosed geometry generated at preprocessing time so the
    // (alpha-blended, non-depth-writing) surfel splat pass can depth-test against it and avoid seeing
    // through gaps in a sparse point cloud to surfels on the far side. The volume is a closed voxel
    // proxy of the whole model: one cube per grid cell that is not reachable from outside the model
    // without crossing a surfel-occupied cell -- i.e. the surfel-occupied surface shell itself plus
    // everything it encloses. halfSize is always exactly half the grid cell size, so adjacent cubes share
    // faces and the volume is watertight. Because shell cubes contain the surface, the renderer shrinks
    // the volume at draw time by pulling only its EXPOSED faces (those bordering a non-cube cell) inward
    // by a fraction of a cell (SurfelsCB::occlusionShrinkCells) so the proxy sits just beneath the
    // surfels -- shaving the outer skin off the solid rather than shrinking each cube about its own
    // centre, which would open seams between neighbours. A cube whose opposing exposed faces would
    // cross (a one-cell-thick shell) is clamped to a thin slab through the cell centre rather than
    // removed, so hollow/open scans keep a closed shell. packedColor holds an RGB565 bake of the
    // average colour of the nearest surfels
    // reachable without crossing empty (exterior) space in its low 16 bits, and the 6-bit exposed-face
    // mask (bit 16 + face, faces ordered -Z,+Z,-X,+X,-Y,+Y) in bits 16..21 -- a mask of 0 (files baked
    // before the mask existed) is treated by the shader as "all faces exposed". See
    // PreprocessApp::BuildOcclusionVolume.
    #pragma pack(push, 1)
    struct OcclusionVoxelGPU
    {
        XMFLOAT3 center;
        float    halfSize;
        uint32_t packedColor; // Bits 0..15: RGB565 colour (see Quantizer/UnpackColorRGB565); bits 16..21: exposed-face mask
    };
    #pragma pack(pop)
    static_assert(sizeof(OcclusionVoxelGPU) == 20, "OcclusionVoxelGPU must be exactly 20 bytes");

    // Packed 8-byte GPU Surfel structure
    // Layout:
    //  - packedPosRadius (32-bit):
    //      - pos.x: 10 bits [0..1023] relative to chunk AABB
    //      - pos.y: 10 bits [0..1023] relative to chunk AABB
    //      - pos.z: 10 bits [0..1023] relative to chunk AABB
    //      - radiusExp: 2 bits [0..3] coarse radius scaling
    //  - packedNormal (16-bit):
    //      - oct16 octahedral normal (2 x snorm8)
    //  - packedColor (16-bit):
    //      - RGB565 (5 bits red, 6 bits green, 5 bits blue)
    #pragma pack(push, 1)
    struct PackedSurfelGPU
    {
        uint32_t packedPosRadius; // 10:10:10:2
        uint16_t packedNormal;    // 8:8 Octahedral
        uint16_t packedColor;     // 5:6:5 RGB
    };
    #pragma pack(pop)

    static_assert(sizeof(PackedSurfelGPU) == 8, "PackedSurfelGPU must be exactly 8 bytes");

    // Meshlet / Micro-Chunk descriptor for 64/128-surfel spatial clusters
    struct MeshletChunkGPU
    {
        XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
        float    boundingRadius = 0.0f;
        XMFLOAT3 aabbMin = { 0.0f, 0.0f, 0.0f };
        uint32_t surfelOffset = 0;
        XMFLOAT3 aabbExtents = { 0.0f, 0.0f, 0.0f };
        uint32_t surfelCount = 0;
        float    blendWeight = 1.0f; // 1.0f = 100% solid opacity
        uint32_t lodLevel = 0;
        float    dilationMorph = 0.0f; // Morph dilation factor for silhouette reconstruction
        float    isSilhouette = 0.0f;  // 1.0f if silhouette chunk, 0.0f otherwise
        XMFLOAT3 coneAxis = { 0.0f, 1.0f, 0.0f }; // Average unit normal vector of cluster
        float    coneCutoff = -1.0f;              // cos(theta_max) of cluster normal cone (-1.0 = cone culling disabled)
    };
    static_assert(sizeof(MeshletChunkGPU) == 80, "MeshletChunkGPU must be exactly 80 bytes");

    // Uncompressed intermediate surfel representation for CPU / preprocessor
    struct SurfelVertex
    {
        XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 normal   = { 0.0f, 1.0f, 0.0f };
        XMFLOAT3 color    = { 1.0f, 1.0f, 1.0f };
        float    radius   = 0.05f;
    };
    static_assert(sizeof(SurfelVertex) == 40, "SurfelVertex size check");

    // Metadata for a single chunk's LOD layer
    struct ChunkLODHeader
    {
        uint32_t lodLevel;
        uint32_t surfelCount;
        uint32_t uncompressedByteSize;
        uint32_t compressedByteSize;
        uint64_t fileOffset;
        float    geometricError;
    };

    // Header stored per-chunk in manifest
    struct ChunkManifest
    {
        uint32_t chunkId;
        XMFLOAT3 aabbMin;
        XMFLOAT3 aabbMax;
        XMFLOAT3 center;
        float    boundingRadius;
        uint32_t numLODs;
        std::vector<ChunkLODHeader> lods;
    };

    // Global file header for .sflw package
    struct SFLWFileHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t numChunks;
        uint32_t maxLOD;
        uint64_t totalSurfelsLOD0;
        double   globalOriginX;
        double   globalOriginY;
        double   globalOriginZ;
        XMFLOAT3 globalBoundsMin;
        XMFLOAT3 globalBoundsMax;
        float    splatRadius; // v2+ only (SFLW_VERSION >= 2) -- garbage/unset on files packaged by v1.
                               // Always check header.version before trusting this field; see
                               // StreamPackager::LoadPackage, which falls back to 1.0f otherwise.
        uint32_t occlusionVoxelCount;  // v3+ only -- 0 (and occlusionVoxelOffset unset/ignored) on v1/v2
        uint64_t occlusionVoxelOffset; // files, or on a v3 file that chose not to generate a volume.
    };

    // =========================================================================
    // Encoding & Quantization Helper Functions
    // =========================================================================

    // Octahedral Normal Packing (Oct16: 2 x snorm8 -> uint16_t)
    inline uint16_t PackNormalOct16(XMFLOAT3 n)
    {
        float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
        if (l1 > 1e-6f)
        {
            n.x /= l1;
            n.y /= l1;
            n.z /= l1;
        }
        else
        {
            n = XMFLOAT3(0.0f, 1.0f, 0.0f);
        }

        float ox = n.x;
        float oy = n.y;
        if (n.z < 0.0f)
        {
            ox = (1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
            oy = (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
        }

        int8_t ix = (int8_t)std::max(-128.0f, std::min(127.0f, std::round(ox * 127.0f)));
        int8_t iy = (int8_t)std::max(-128.0f, std::min(127.0f, std::round(oy * 127.0f)));

        uint8_t ux = (uint8_t)ix;
        uint8_t uy = (uint8_t)iy;
        return (uint16_t)((uy << 8) | ux);
    }

    // Octahedral Normal Unpacking (uint16_t -> XMFLOAT3)
    inline XMFLOAT3 UnpackNormalOct16(uint16_t packed)
    {
        int8_t ix = (int8_t)(packed & 0xFF);
        int8_t iy = (int8_t)((packed >> 8) & 0xFF);

        float ox = ix / 127.0f;
        float oy = iy / 127.0f;
        float oz = 1.0f - std::abs(ox) - std::abs(oy);

        float x = ox;
        float y = oy;
        float z = oz;

        if (oz < 0.0f)
        {
            x = (1.0f - std::abs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
            y = (1.0f - std::abs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
        }

        float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1e-6f)
        {
            x /= len;
            y /= len;
            z /= len;
        }
        return XMFLOAT3(x, y, z);
    }

    // RGB Color Packing to RGB565 (uint16_t)
    inline uint16_t PackColorRGB565(XMFLOAT3 c)
    {
        uint32_t r = (uint32_t)std::max(0.0f, std::min(31.0f, std::round(c.x * 31.0f)));
        uint32_t g = (uint32_t)std::max(0.0f, std::min(63.0f, std::round(c.y * 63.0f)));
        uint32_t b = (uint32_t)std::max(0.0f, std::min(31.0f, std::round(c.z * 31.0f)));
        return (uint16_t)((r << 11) | (g << 5) | b);
    }

    // RGB565 Unpacking
    inline XMFLOAT3 UnpackColorRGB565(uint16_t packed)
    {
        float r = ((packed >> 11) & 0x1F) / 31.0f;
        float g = ((packed >> 5) & 0x3F) / 63.0f;
        float b = (packed & 0x1F) / 31.0f;
        return XMFLOAT3(r, g, b);
    }

    // Voxel-Relative Position Packing (10:10:10:2)
    inline uint32_t PackPosRadius(XMFLOAT3 pos, XMFLOAT3 aabbMin, XMFLOAT3 aabbExtents, uint32_t radiusExp = 0)
    {
        float nx = (aabbExtents.x > 1e-6f) ? (pos.x - aabbMin.x) / aabbExtents.x : 0.0f;
        float ny = (aabbExtents.y > 1e-6f) ? (pos.y - aabbMin.y) / aabbExtents.y : 0.0f;
        float nz = (aabbExtents.z > 1e-6f) ? (pos.z - aabbMin.z) / aabbExtents.z : 0.0f;

        uint32_t qx = (uint32_t)std::max(0.0f, std::min(1023.0f, std::round(nx * 1023.0f)));
        uint32_t qy = (uint32_t)std::max(0.0f, std::min(1023.0f, std::round(ny * 1023.0f)));
        uint32_t qz = (uint32_t)std::max(0.0f, std::min(1023.0f, std::round(nz * 1023.0f)));
        uint32_t re = radiusExp & 0x3;

        return (re << 30) | (qz << 20) | (qy << 10) | qx;
    }

    // Voxel-Relative Position Unpacking
    inline XMFLOAT3 UnpackPos(uint32_t packed, XMFLOAT3 aabbMin, XMFLOAT3 aabbExtents)
    {
        uint32_t qx = packed & 0x3FF;
        uint32_t qy = (packed >> 10) & 0x3FF;
        uint32_t qz = (packed >> 20) & 0x3FF;

        float x = aabbMin.x + (qx / 1023.0f) * aabbExtents.x;
        float y = aabbMin.y + (qy / 1023.0f) * aabbExtents.y;
        float z = aabbMin.z + (qz / 1023.0f) * aabbExtents.z;
        return XMFLOAT3(x, y, z);
    }

    // =========================================================================
    // Geometry Optimization & Culling Pipeline Telemetry
    // =========================================================================
    struct GeometryCullStats
    {
        uint32_t totalDatasetSurfels = 0;   // Total points in source model / LOD0
        uint32_t totalDatasetChunks = 0;    // Total chunks in source model
        
        // Stage 1: CPU / LOD Decimation
        uint32_t lodPrunedSurfels = 0;      // Surfels eliminated by multi-res LOD selection
        uint32_t lodActiveSurfels = 0;      // Surfels submitted for rendering this frame
        uint32_t lodActiveChunks = 0;       // Chunks submitted this frame
        
        // Stage 2: Amplification / Task Shader (mainAS) Culling
        uint32_t asFrustumCulledChunks = 0; // Chunks culled by view frustum AABB test
        uint32_t asFrustumCulledSurfels = 0;// Surfels in frustum-culled chunks
        uint32_t asConeCulledChunks = 0;    // Chunks culled by normal cone backface test
        uint32_t asConeCulledSurfels = 0;   // Surfels in cone-culled chunks
        uint32_t asPassedChunks = 0;        // Chunks passed AS -> dispatched to MS
        uint32_t asPassedSurfels = 0;       // Surfels dispatched to MS
        
        // Stage 3: Mesh Shader (mainMS) Culling
        uint32_t msDetachedCulledSurfels = 0; // Surfels culled in MS (e.g. detached cull cam)
        uint32_t msDrawnSurfels = 0;          // Surfels successfully drawn / rasterized
        
        // Amplification & Rasterizer Output
        uint32_t generatedVertices = 0;     // msDrawnSurfels * 4
        uint32_t generatedTriangles = 0;    // msDrawnSurfels * 2
        
        // Derived Efficiency Ratios
        float totalCullingRatio = 0.0f;     // Overall reduction % (Total - Drawn) / Total * 100
        float asCullingRatio = 0.0f;        // AS Stage Culling %
        float lodDecimationRatio = 0.0f;    // LOD Decimation %
        float vramBandwidthSavedMB = 0.0f;  // Estimated bandwidth saved vs flat rendering
    };
}


