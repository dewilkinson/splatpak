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
#include <fstream>
#include <istream>
#include <cstdio>
#include <cstring>
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
    static constexpr uint32_t SFLW_VERSION = 6; // v2 adds SFLWFileHeader::splatRadius (appended at the
                                                 // struct's end so v1 files still read correctly -- see
                                                 // the version check in StreamPackager::LoadPackage).
                                                 // v3 adds an optional occlusion voxel array, appended
                                                 // after all chunk LOD data; occlusionVoxelCount/Offset
                                                 // are 0 on v1/v2 files and on v3 files that simply chose
                                                 // not to bake one.
                                                 // v4 folds the chunk manifest (previously a companion
                                                 // .json file) into the .sflw itself as a binary table at
                                                 // SFLWFileHeader::manifestOffset, so a package is a
                                                 // single self-contained file. v1-v3 files still load via
                                                 // their companion .json (see ParseLegacyJsonManifest).
                                                 // v5 adds sourceFileBytes: the size of the original
                                                 // input file (.ply/.splat) the package was built from,
                                                 // so the compression ratio shown is always that file
                                                 // against this one. 0 on v1-v4 files or when unknown.
                                                 // v6 adds the occlusion volume mip table (occlusionMipCount,
                                                 // occlusionMipBlockCount[], occlusionMipCellSize[]): the
                                                 // voxel array now holds a nested chain of volumes, mip 0
                                                 // (the finest, exactly what v3-v5 stored) first, followed
                                                 // by progressively coarser conservative downsamples. A v5
                                                 // reader draws the whole array and still sees a correct
                                                 // volume, because every coarser mip lies strictly inside
                                                 // mip 0's skin (see OcclusionMipTable).

    // Upper bound on the occlusion volume mip chain: mip 0 plus up to three coarser levels. Blocks fall
    // by roughly 4x per level (the skin is a surface), so the whole chain costs about a third more than
    // mip 0 alone, and a fourth coarsening would be too blobby to hug any surface usefully.
    static constexpr uint32_t kMaxOcclusionMips = 4;

    #pragma pack(push, 1)
    // One baked occluder block of the interior occlusion volume (see PreprocessApp::BuildOcclusionVolume).
    // The volume is a closed voxel solid -- every fine grid cell that cannot be reached from outside the
    // model without crossing a surfel-occupied cell (the surfel-occupied shell plus everything it
    // encloses) -- eroded by a baked number of cell layers so it sits just inside the surfel shell, and
    // then stored as an adaptive octree: runs of 2x2x2 solid blocks are merged into one larger cube,
    // recursively, so the finest cubes hug the surface while the interior is covered by a few big ones.
    // Blocks with no exposed face at all are dropped outright, since a closed occluder only needs its
    // skin. halfSize is therefore half of (cellSize * 2^level), and faces of neighbouring blocks of
    // different sizes still tile exactly because every block is grid-aligned to its own size.
    //
    // packedColor layout:
    //   bits  0..15  RGB565 bake of the mean surfel colour over a wide box of surface around the block
    //                (half-width ~1.5x its depth below the surface), i.e. the local albedo of the cap of
    //                surface facing it rather than one nearest patch (see Quantizer/UnpackColorRGB565).
    //   bits 16..21  exposed-face mask, one bit per face in -Z,+Z,-X,+X,-Y,+Y order: set when any fine
    //                cell across that face is not solid, i.e. some part of the face is visible.
    //   bit  22      mask-valid flag. Set by every current bake. Clear on legacy files baked before the
    //                mask existed, which the shader treats as "all six faces exposed".
    //
    // There is deliberately no live shrink/shave at draw time: the erosion is baked, because the volume
    // travels inside the .sflw and the viewer should show exactly what was packaged.
    struct OcclusionVoxelGPU
    {
        XMFLOAT3 center;
        float    halfSize;
        uint32_t packedColor; // Bits 0..15: RGB565 colour (see Quantizer/UnpackColorRGB565); bits 16..21: exposed-face mask
    };
    #pragma pack(pop)
    static_assert(sizeof(OcclusionVoxelGPU) == 20, "OcclusionVoxelGPU must be exactly 20 bytes");

    // Layout of the occlusion volume mip chain inside one OcclusionVoxelGPU array (v6+ packages, and the
    // in-memory result of OcclusionVolume::Bake). Mip 0 is the finest volume -- the level-0 skin, sitting
    // one below it and can never protrude, and its exposed-face masks are recomputed against its own
    // solid. The renderer draws exactly ONE mip per frame, chosen so a cell still covers a few pixels at
    // the model's nearest point (see PreprocessRenderer): at distance the fine skin is sub-pixel work and,
    // worse, too tight for the big coarse-LOD splats it is paired with (a tangent disc sags below a
    // curved surface by ~r^2 / 2R, and gets clipped once that exceeds the skin's erosion margin), so the
    // cube size and the erosion margin grow together with the surfel LOD in view.
    //
    // The blocks of all mips are stored back to back, mip 0 first, so a pre-v6 reader that draws the
    // whole array still renders correctly (the coarser mips are hidden inside mip 0's skin).
    struct OcclusionMipTable
    {
        uint32_t mipCount = 0;                          // 0 = no volume; 1 = level-0 only (what v3-v5 packages hold)
        uint32_t blockCount[kMaxOcclusionMips] = {};    // Blocks per mip, in array order (mip 0 first)
        float    cellSize[kMaxOcclusionMips] = {};      // Finest cube edge of each mip; cellSize[k] = cellSize[0] * 2^k

        uint32_t FirstBlock(uint32_t mip) const
        {
            uint32_t first = 0;
            for (uint32_t k = 0; k < mip && k < kMaxOcclusionMips; k++) first += blockCount[k];
            return first;
        }
        uint32_t TotalBlocks() const { return FirstBlock(kMaxOcclusionMips); }

        // A table describing a plain single-level array (legacy packages, or any caller that never
        // asked for mips): one mip holding every block, its cell size read off the smallest block.
        static OcclusionMipTable SingleLevel(const OcclusionVoxelGPU* blocks, uint32_t count)
        {
            OcclusionMipTable t;
            if (count == 0 || blocks == nullptr) return t;
            float minHalf = blocks[0].halfSize;
            for (uint32_t i = 1; i < count; i++) minHalf = (blocks[i].halfSize < minHalf) ? blocks[i].halfSize : minHalf;
            t.mipCount = 1;
            t.blockCount[0] = count;
            t.cellSize[0] = minHalf * 2.0f;
            return t;
        }
    };

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
        uint64_t manifestOffset;       // v4+ only -- absolute byte offset of the embedded chunk manifest
                                       // table (see ChunkManifestRecord). 0 / unset on v1-v3 files, which
                                       // keep their manifest in a companion .json instead.
        uint64_t sourceFileBytes;      // v5+ only -- byte size of the original input file this package was
                                       // built from (0 = unknown). Compression ratio = this / package size.
        uint32_t occlusionMipCount;    // v6+ only -- occlusion volume mips stored back to back in the voxel
                                       // array (mip 0 first); see OcclusionMipTable. On v3-v5 files the whole
                                       // array is one mip (StreamPackager::LoadPackage synthesizes the table).
        uint32_t occlusionMipBlockCount[kMaxOcclusionMips]; // v6+ only -- blocks per mip; sums to occlusionVoxelCount
        float    occlusionMipCellSize[kMaxOcclusionMips];   // v6+ only -- finest cube edge of each mip
    };

    // On-disk form of one ChunkManifest entry in a v4+ .sflw's embedded manifest table. The table
    // lives at SFLWFileHeader::manifestOffset and holds SFLWFileHeader::numChunks entries, each a
    // ChunkManifestRecord immediately followed by numLODs ChunkLODHeader records.
    struct ChunkManifestRecord
    {
        uint32_t chunkId;
        XMFLOAT3 aabbMin;
        XMFLOAT3 aabbMax;
        XMFLOAT3 center;
        float    boundingRadius;
        uint32_t numLODs;
    };

    // Reads the v4+ embedded manifest table out of an already-open .sflw stream. Leaves the stream
    // position wherever the table ended; callers seek by absolute offset for everything else anyway.
    inline bool ReadEmbeddedManifest(std::istream& in, const SFLWFileHeader& header, std::vector<ChunkManifest>& outChunks)
    {
        outChunks.clear();
        if (header.version < 4 || header.manifestOffset == 0) return false;

        in.seekg((std::streamoff)header.manifestOffset, std::ios::beg);
        if (!in.good()) return false;

        outChunks.reserve(header.numChunks);
        for (uint32_t c = 0; c < header.numChunks; c++)
        {
            ChunkManifestRecord rec = {};
            in.read(reinterpret_cast<char*>(&rec), sizeof(rec));
            if (!in.good()) { outChunks.clear(); return false; }

            ChunkManifest cm = {};
            cm.chunkId        = rec.chunkId;
            cm.aabbMin        = rec.aabbMin;
            cm.aabbMax        = rec.aabbMax;
            cm.center         = rec.center;
            cm.boundingRadius = rec.boundingRadius;
            cm.numLODs        = rec.numLODs;
            cm.lods.resize(rec.numLODs);
            if (rec.numLODs > 0)
            {
                in.read(reinterpret_cast<char*>(cm.lods.data()), sizeof(ChunkLODHeader) * rec.numLODs);
                if (!in.good()) { outChunks.clear(); return false; }
            }
            outChunks.push_back(std::move(cm));
        }
        return true;
    }

    // Parses the companion .json manifest that v1-v3 packages shipped alongside their .sflw. Only
    // needed to keep those older files loadable; v4+ packages embed the manifest (ReadEmbeddedManifest).
    // Deliberately a minimal scanner for the exact schema StreamPackager used to write, not a general
    // JSON parser.
    inline bool ParseLegacyJsonManifest(const std::string& jsonPath, std::vector<ChunkManifest>& outChunks)
    {
        outChunks.clear();

        std::ifstream in(jsonPath);
        if (!in.is_open()) return false;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        size_t pos = content.find("\"chunks\":");
        if (pos == std::string::npos) return false;

        auto readVec3 = [&](const char* key, size_t from, size_t limit, XMFLOAT3& out)
        {
            size_t at = content.find(key, from);
            if (at == std::string::npos || at >= limit) return;
            const char* p = strchr(content.c_str() + at, '[');
            if (p) sscanf_s(p, "[%f, %f, %f]", &out.x, &out.y, &out.z);
        };

        while ((pos = content.find("\"id\":", pos)) != std::string::npos)
        {
            // Each chunk object is short; bound the field searches so a missing key can't pick up the
            // next chunk's value.
            size_t objLimit = pos + 400;

            ChunkManifest cm = {};
            sscanf_s(content.c_str() + pos, "\"id\": %u", &cm.chunkId);
            readVec3("\"bounds_min\":", pos, objLimit, cm.aabbMin);
            readVec3("\"bounds_max\":", pos, objLimit, cm.aabbMax);
            readVec3("\"center\":",     pos, objLimit, cm.center);

            size_t radPos = content.find("\"radius\":", pos);
            if (radPos != std::string::npos && radPos < objLimit)
                sscanf_s(content.c_str() + radPos, "\"radius\": %f", &cm.boundingRadius);

            size_t lodsPos = content.find("\"lods\":", pos);
            size_t lodsEnd = (lodsPos != std::string::npos) ? content.find("]", lodsPos) : std::string::npos;
            if (lodsPos != std::string::npos && lodsPos < objLimit && lodsEnd != std::string::npos)
            {
                size_t curLod = lodsPos;
                while (true)
                {
                    size_t lvlPos = content.find("\"level\":", curLod);
                    if (lvlPos == std::string::npos || lvlPos >= lodsEnd) break;

                    ChunkLODHeader lh = {};
                    auto readU32 = [&](const char* key, uint32_t& out)
                    {
                        size_t at = content.find(key, lvlPos);
                        if (at != std::string::npos && at < lodsEnd)
                        {
                            std::string fmt = std::string(key) + " %u";
                            sscanf_s(content.c_str() + at, fmt.c_str(), &out);
                        }
                    };
                    readU32("\"level\":",            lh.lodLevel);
                    readU32("\"count\":",            lh.surfelCount);
                    readU32("\"raw_bytes\":",        lh.uncompressedByteSize);
                    readU32("\"compressed_bytes\":", lh.compressedByteSize);

                    size_t offPos = content.find("\"offset\":", lvlPos);
                    if (offPos != std::string::npos && offPos < lodsEnd)
                        sscanf_s(content.c_str() + offPos, "\"offset\": %llu", &lh.fileOffset);

                    size_t errPos = content.find("\"error\":", lvlPos);
                    if (errPos != std::string::npos && errPos < lodsEnd)
                        sscanf_s(content.c_str() + errPos, "\"error\": %f", &lh.geometricError);

                    cm.lods.push_back(lh);

                    size_t nextObj = content.find("}", lvlPos);
                    if (nextObj == std::string::npos || nextObj >= lodsEnd) break;
                    curLod = nextObj + 1;
                }
                pos = lodsEnd + 1;
            }
            else
            {
                pos += 10;
            }

            cm.numLODs = (uint32_t)cm.lods.size();
            outChunks.push_back(std::move(cm));
        }

        return !outChunks.empty();
    }

    // Given any of "<base>", "<base>.sflw", or a legacy "<base>.json", returns the .sflw path and the
    // legacy companion .json path (only consulted for v1-v3 files).
    inline void ResolvePackagePaths(const std::string& inputPath, std::string& outSflwPath, std::string& outJsonPath)
    {
        std::string lowerPath = inputPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        std::string base = inputPath;
        if (lowerPath.size() >= 5 && (lowerPath.compare(lowerPath.size() - 5, 5, ".sflw") == 0 ||
                                      lowerPath.compare(lowerPath.size() - 5, 5, ".json") == 0))
        {
            base = inputPath.substr(0, inputPath.size() - 5);
        }
        outSflwPath = base + ".sflw";
        outJsonPath = base + ".json";
    }

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


