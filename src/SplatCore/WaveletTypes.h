// WaveletTypes.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Shared data types for the wavelet streaming pipeline: the .gstx file format (magic/
// version/header), GPU-packed splat and meshlet-chunk structs, and the pack/unpack
// helpers that convert between them. Included by every preprocessor and viewer file
// that touches splat or chunk data, on both the CPU and (conceptually) GPU side.

#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <fstream>
#include <istream>
#include <cstdio>
#include <cstring>
#include <DirectXMath.h>
#include "../../libs/gstx/ColourTransform.h" // PayloadTransformBits (v11 header field)

namespace Splat
{
    using namespace DirectX;

    // Shared by every raw point-cloud loader (PLYLoader, SPLATLoader): source coordinates farther than
    // this from the origin trigger origin-shifting so downstream single-precision float math doesn't
    // lose precision on real-world geospatial datasets (e.g. absolute UTM/ECEF coordinates).
    static constexpr double kGeospatialOffsetThreshold = 10000.0;

    // Magic bytes of a package: "GSTX" in ASCII (.gstx)
    static constexpr uint32_t GSTX_MAGIC = 0x58545347;
    // The format version written, and the only one read: packages of earlier versions are baked again from their
    // source. The history of the layout is in docs/GSTX_FORMAT_SPECIFICATION.md.
    static constexpr uint32_t SFLW_VERSION = 14;

    // Upper bound on the occlusion volume mip chain: mip 0 plus up to three coarser levels. Blocks fall
    // by roughly 4x per level (the skin is a surface), so the whole chain costs about a third more than
    // mip 0 alone, and a fourth coarsening would be too blobby to hug any surface usefully.
    static constexpr uint32_t kMaxOcclusionMips = 4;

    #pragma pack(push, 1)
    // One baked occluder block of the interior occlusion volume (libs/gstx/OcclusionVolume.h).
    // The volume is a closed voxel solid of the model sitting just inside the splat shell, stored as an
    // adaptive octree: the finest cubes hug the surface while the interior is covered by a few big ones,
    // and blocks with no exposed face are dropped, since a closed occluder only needs its skin. halfSize
    // is half of (cellSize * 2^level), and faces of neighbouring blocks of different sizes still tile
    // exactly because every block is grid-aligned to its own size.
    //
    // packedColor layout:
    //   bits  0..15  RGB565 bake of the mean splat colour over a wide box of surface around the block
    //                (half-width ~1.5x its depth below the surface), i.e. the local albedo of the cap of
    //                surface facing it rather than one nearest patch (see Quantizer/UnpackColorRGB565).
    //   bits 16..21  exposed-face mask, one bit per face in -Z,+Z,-X,+X,-Y,+Y order: set when any fine
    //                cell across that face is not solid, i.e. some part of the face is visible.
    //   bit  22      mask-valid flag. Set by every current bake. Clear on legacy files baked before the
    //                mask existed, which the shader treats as "all six faces exposed".
    //
    // There is deliberately no live shrink/shave at draw time: the erosion is baked, because the volume
    // travels inside the .gstx and the viewer should show exactly what was packaged.
    struct OcclusionVoxelGPU
    {
        XMFLOAT3 center;
        float    halfSize;
        uint32_t packedColor; // Bits 0..15: RGB565 colour (see Quantizer/UnpackColorRGB565); bits 16..21: exposed-face mask
    };
    #pragma pack(pop)
    static_assert(sizeof(OcclusionVoxelGPU) == 20, "OcclusionVoxelGPU must be exactly 20 bytes");

    // Layout of the occlusion volume mip chain inside one OcclusionVoxelGPU array (in a package, and the
    // in-memory result of OcclusionVolume::Bake). Mip 0 is the finest volume, the skin just inside the
    // splat surface; each further mip is a coarser level that lies inside the one below it and can never
    // protrude, with its own exposed-face masks. The renderer draws exactly ONE mip per frame, chosen so
    // a cell still covers a few pixels at the model's nearest point (see SplatRenderer): at distance the
    // fine skin is sub-pixel work and too tight for the big coarse-LOD splats it is paired with, so the
    // cube size grows together with the splat LOD in view.
    //
    // The blocks of all mips are stored back to back, mip 0 first (the coarser mips lie hidden inside mip 0's
    // skin, so drawing the whole array still renders correctly).
    struct OcclusionMipTable
    {
        uint32_t mipCount = 0;                          // 0 = no volume; 1 = level-0 only
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

    // Packed 8-byte GPU Splat structure
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
    struct PackedPointGPU
    {
        uint32_t packedPosRadius; // 10:10:10:2
        uint16_t packedNormal;    // 8:8 Octahedral
        uint16_t packedColor;     // 5:6:5 RGB
    };
    #pragma pack(pop)

    static_assert(sizeof(PackedPointGPU) == 8, "PackedPointGPU must be exactly 8 bytes");

    // Meshlet / Micro-Chunk descriptor for 64/128-splat spatial clusters
    struct MeshletChunkGPU
    {
        XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
        float    boundingRadius = 0.0f;
        XMFLOAT3 aabbMin = { 0.0f, 0.0f, 0.0f };
        uint32_t splatOffset = 0;
        XMFLOAT3 aabbExtents = { 0.0f, 0.0f, 0.0f };
        uint32_t splatCount = 0;
        float    blendWeight = 1.0f; // 1.0f = 100% solid opacity
        uint32_t lodLevel = 0;
        float    dilationMorph = 0.0f; // Morph dilation factor for silhouette reconstruction; splat mode: the level 0 Gaussian fade-in weight instead (0 = discs, 1 = Gaussians; see SplatPakApp::UpdateStreamingSimulation)
        float    isSilhouette = 0.0f;  // 1.0f if silhouette chunk, 0.0f otherwise
        XMFLOAT3 coneAxis = { 0.0f, 1.0f, 0.0f }; // Average unit normal vector of cluster
        float    coneCutoff = -1.0f;              // cos(theta_max) of cluster normal cone (-1.0 = cone culling disabled)
    };
    static_assert(sizeof(MeshletChunkGPU) == 80, "MeshletChunkGPU must be exactly 80 bytes");

    // Uncompressed intermediate splat representation for CPU / preprocessor
    struct SplatPoint
    {
        XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 normal   = { 0.0f, 1.0f, 0.0f };
        XMFLOAT3 color    = { 1.0f, 1.0f, 1.0f };
        float    radius   = 0.05f;
        // Splat mode: index of this point's entry in the source cloud's SplatAttributes array (the
        // Gaussian's rotation, scales, opacity and spherical harmonics). Survives the decimation into
        // coarser levels, so every level can be encoded from the original attributes. kNoSplatSource
        // marks a point with no such entry (a plain point cloud, a synthetic cloud, a decoded splat).
        uint32_t sourceIndex = 0xFFFFFFFFu;
    };
    static_assert(sizeof(SplatPoint) == 44, "SplatPoint size check (mirrored by RawSplat in the shaders)");
    static constexpr uint32_t kNoSplatSource = 0xFFFFFFFFu;

    // 3D Gaussian Splatting attributes of one source point, kept alongside SplatPoint during a bake
    // in splat mode (SplatPoint::sourceIndex points here). Scales and rotation are in the model's
    // frame after the import's axis change; the spherical-harmonics coefficients have had the same
    // change applied (see SplatCodec::ApplyAxisFlipToSH) so a shader evaluates them with a direction
    // in that frame. sh holds the bands above the DC term, coefficient-major: sh[3 * k + c] is band
    // coefficient k (0..14, bands 1..3 in the 3DGS order) of colour channel c. The DC colour itself is
    // SplatPoint::color (0.5 + C0 * f_dc, clamped to [0, 1]).
    static constexpr uint32_t kSplatSHMaxCoefficients = 45; // 15 coefficients x 3 channels (degree 3)
    struct SplatAttributes
    {
        XMFLOAT3 scale      = { 0.01f, 0.01f, 0.01f };    // Per-axis standard deviations (world units), exp() of the file's log scales
        XMFLOAT4 rotation   = { 0.0f, 0.0f, 0.0f, 1.0f }; // Unit quaternion (x, y, z, w)
        float    opacity    = 1.0f;                       // Sigmoid of the file's logit, 0..1
        uint32_t bakedLevel = 0;                          // Level whose footprint these scales already describe (0 for an import; see SplatCodec::AttributesForLevel)
        float    sh[kSplatSHMaxCoefficients] = {};
    };

    // Which per-point record a package's LOD payloads hold (SFLWFileHeader::recordFormat, v8+).
    enum SplatFormat : uint32_t
    {
        RECORD_FORMAT_POINT8  = 0, // PackedPointGPU: 8-byte oriented disc (no longer read)
        RECORD_FORMAT_SPLAT20  = 1, // PackedSplatGPU: 20-byte 3D Gaussian, plus an optional spherical-harmonics stream
    };

    // Splat mode's per-point GPU record: a full 3D Gaussian in 20 bytes. Five little-endian 32-bit words:
    //   word 0  bits  0..15 position x, 16..31 position y      -- 16-bit unorm inside the global bounds
    //   word 1  bits  0..15 position z, 16..23 scale x, 24..31 scale y
    //   word 2  bits  0..7  scale z, 8..15 opacity, 16..23 colour R, 24..31 colour G
    //   word 3  bits  0..7  colour B, 8..15 flags, 16..31 SH scale (IEEE 754 binary16)
    //   word 4  rotation: smallest-three unit quaternion, bits 0..1 index of the dropped (largest) component,
    //           bits 2..11 / 12..21 / 22..31 the other three in order, 10-bit unorm over [-1/sqrt2, +1/sqrt2]
    // Scales are 8-bit unorm over the package's [splatScaleLog2Min, splatScaleLog2Max] in log2 space.
    // Opacity is 8-bit unorm. Colour is the DC term as 8-bit display-space RGB. The SH scale is the
    // per-point dequantisation factor of the record's entry in the package's SH stream (0 = no SH).
    #pragma pack(push, 1)
    struct PackedSplatGPU
    {
        uint32_t word0;
        uint32_t word1;
        uint32_t word2;
        uint32_t word3;
        uint32_t word4;
    };
    #pragma pack(pop)
    static_assert(sizeof(PackedSplatGPU) == 20, "PackedSplatGPU must be exactly 20 bytes");

    // Splat mode: one entry per chunk LOD in the SH table at SFLWFileHeader::shManifestOffset (v8+),
    // in the same order as the chunk manifest's LOD headers. The payload is the level's SH stream:
    // splatCount records of SFLWFileHeader::shRecordBytes signed bytes each, passed through the
    // same shuffle and entropy stages as the LOD payload. A zero-length entry means no SH for that level.
    struct SHLODRecord
    {
        uint64_t fileOffset;
        uint32_t compressedByteSize;
        uint32_t uncompressedByteSize;
    };

    // The entropy stage a package's payloads went through (SFLWFileHeader::payloadCodec, v9+); the coders
    // themselves are in PayloadCodec.h
    enum PayloadCodecId : uint32_t
    {
        PAYLOAD_CODEC_LIBRARY = 0, // The codec library's own stage (ByteShuffle::CompressShuffled): every package before v9
        PAYLOAD_CODEC_BROTLI  = 1, // Brotli (RFC 7932): every package written since v9
    };

    inline uint32_t SplatRecordBytes(uint32_t recordFormat)
    {
        return recordFormat == RECORD_FORMAT_SPLAT20 ? (uint32_t)sizeof(PackedSplatGPU) : (uint32_t)sizeof(PackedPointGPU);
    }

    // Metadata for a single chunk's LOD layer
    struct ChunkLODHeader
    {
        uint32_t lodLevel;
        uint32_t splatCount;
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

    // Global file header for .gstx package
    struct SFLWFileHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t numChunks;
        uint32_t maxLOD;
        uint64_t totalSplatsLOD0;
        double   globalOriginX;
        double   globalOriginY;
        double   globalOriginZ;
        XMFLOAT3 globalBoundsMin;
        XMFLOAT3 globalBoundsMax;
        float    splatRadius;          // Disc radius scale the model was authored with
        uint32_t occlusionVoxelCount;  // Voxels of the occlusion volume; 0 = none (a package with an occluder mesh has none)
        uint64_t occlusionVoxelOffset; // Absolute offset of the voxel array
        uint64_t manifestOffset;       // Absolute offset of the embedded chunk manifest table (see ChunkManifestRecord)
        uint64_t sourceFileBytes;      // Byte size of the input file this package was built from (0 = unknown).
                                       // Compression ratio = this / package size.
        uint32_t occlusionMipCount;    // Occlusion volume mips stored back to back in the voxel array (mip 0 first); see OcclusionMipTable
        uint32_t occlusionMipBlockCount[kMaxOcclusionMips]; // Blocks per mip; sums to occlusionVoxelCount
        float    occlusionMipCellSize[kMaxOcclusionMips];   // Finest cube edge of each mip
        uint32_t detailGridDims[3];    // Detail heatmap grid resolution (x, y, z); all 0 = no grid stored
        float    detailGridCellSize;   // Cell edge in metres; the grid's origin is globalBoundsMin
        uint64_t detailGridOffset;     // Absolute offset of the dims[0]*dims[1]*dims[2] uint8 score array (see DetailGrid)
        uint32_t recordFormat;         // RecordFormat of every LOD payload: always RECORD_FORMAT_SPLAT20 (1)
        uint32_t shDegree;             // Spherical-harmonics degree stored in the SH stream (0 = DC only, 1, 2 or 3)
        uint32_t splatRecordBytes;     // Bytes per record in a LOD payload (sizeof(PackedSplatGPU))
        uint32_t shRecordBytes;        // Bytes per point in the SH stream (9 for degree 1, 45 for degree 3, 0 for none)
        float    splatScaleLog2Min;    // log2 scale mapped to byte 0 of the record's scale fields
        float    splatScaleLog2Max;    // log2 scale mapped to byte 255
        uint64_t shManifestOffset;     // Absolute offset of the SHLODRecord table (one per chunk LOD, manifest order); 0 = no SH stream
        uint32_t payloadCodec;         // The entropy stage of every payload (PayloadCodecId)
        uint32_t payloadTransforms;    // PayloadTransformBits (ColourTransform.h)
        uint64_t shadowMeshOffset;     // Absolute offset of the shadow mesh section (ShadowMeshCodec.h); 0 = no mesh
        uint64_t shadowMeshBytes;      // The section's size in bytes
        uint64_t occluderMeshOffset;   // Absolute offset of the occluder mesh section (same encoding); 0 = none
        uint64_t occluderMeshBytes;    // Its size in bytes
    };
    static_assert(offsetof(SFLWFileHeader, payloadTransforms) == 204, "payloadTransforms sits in what was the v9 header's trailing padding");
    static_assert(offsetof(SFLWFileHeader, shadowMeshOffset) == 208, "v12 appends the shadow mesh fields after the v11 header");
    static_assert(offsetof(SFLWFileHeader, occluderMeshOffset) == 224, "v13 appends the occluder mesh fields after the v12 header");
    static_assert(sizeof(SFLWFileHeader) == 240, "the header is 240 bytes");

    // On-disk form of one ChunkManifest entry in a .gstx's embedded manifest table. The table
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

    // Reads the embedded manifest table out of an already-open .gstx stream. Leaves the stream
    // position wherever the table ended; callers seek by absolute offset for everything else anyway.
    inline bool ReadEmbeddedManifest(std::istream& in, const SFLWFileHeader& header, std::vector<ChunkManifest>& outChunks)
    {
        outChunks.clear();
        if (header.manifestOffset == 0) return false;

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

    // ---- .gstx format validation --------------------------------------------------------------
    // Shared by every loader (StreamPackager::LoadPackage for SplatPak and the tests, StreamingManager
    // for the standalone viewer) so a bad package is diagnosed the same way everywhere. Each check
    // fills outReason with a sentence a user can act on; the caller decides how to show it.

    // Validates a header read with one sizeof(SFLWFileHeader)-byte read (headerBytesRead is that read's gcount()).
    inline bool ValidateSFLWHeader(const SFLWFileHeader& h, size_t headerBytesRead, uint64_t fileSize, std::string& outReason)
    {
        char buf[256];
        if (headerBytesRead < 8)
        {
            outReason = "The file is too small to be a .gstx package (it has no header).";
            return false;
        }
        if (h.magic != GSTX_MAGIC)
        {
            snprintf(buf, sizeof(buf), "Not a .gstx package: the file signature is 0x%08X, expected 0x%08X (\"GSTX\").", h.magic, GSTX_MAGIC);
            outReason = buf;
            return false;
        }
        if (h.version != SFLW_VERSION)
        {
            if (h.version > SFLW_VERSION)
                snprintf(buf, sizeof(buf), "The package uses .gstx format version %u, but this build reads version %u. It was written by a newer SplatPak.", h.version, SFLW_VERSION);
            else
                snprintf(buf, sizeof(buf), "The package uses .gstx format version %u, which this build no longer reads (it reads version %u). Bake it again from its source scan.", h.version, SFLW_VERSION);
            outReason = buf;
            return false;
        }
        if (headerBytesRead < sizeof(SFLWFileHeader))
        {
            snprintf(buf, sizeof(buf), "The file is truncated: the header needs %zu bytes but only %zu were present.", sizeof(SFLWFileHeader), headerBytesRead);
            outReason = buf;
            return false;
        }
        if (h.shadowMeshOffset != 0 && (h.shadowMeshOffset < sizeof(SFLWFileHeader) || h.shadowMeshBytes == 0 || h.shadowMeshOffset + h.shadowMeshBytes > fileSize))
        {
            snprintf(buf, sizeof(buf), "The shadow mesh section (%llu bytes at %llu) lies outside the file; the package is truncated or corrupt.",
                     (unsigned long long)h.shadowMeshBytes, (unsigned long long)h.shadowMeshOffset);
            outReason = buf;
            return false;
        }
        if (h.occluderMeshOffset != 0 && (h.occluderMeshOffset < sizeof(SFLWFileHeader) || h.occluderMeshBytes == 0 || h.occluderMeshOffset + h.occluderMeshBytes > fileSize))
        {
            snprintf(buf, sizeof(buf), "The occluder mesh section (%llu bytes at %llu) lies outside the file; the package is truncated or corrupt.",
                     (unsigned long long)h.occluderMeshBytes, (unsigned long long)h.occluderMeshOffset);
            outReason = buf;
            return false;
        }
        if ((h.payloadTransforms & ~(uint32_t)PAYLOAD_TRANSFORMS_KNOWN) != 0)
        {
            snprintf(buf, sizeof(buf), "The package declares payload transforms 0x%x, which this build cannot undo. It was written by a newer SplatPak.", h.payloadTransforms);
            outReason = buf;
            return false;
        }
        if (h.numChunks == 0)
        {
            outReason = "The package header lists zero chunks, so there is nothing to render.";
            return false;
        }
        if (h.numChunks > 16u * 1024u * 1024u)
        {
            snprintf(buf, sizeof(buf), "The package header lists %u chunks, which is not a plausible count -- the header is corrupt.", h.numChunks);
            outReason = buf;
            return false;
        }
        if (h.manifestOffset == 0 || h.manifestOffset >= fileSize)
        {
            snprintf(buf, sizeof(buf), "The chunk manifest offset (%llu) lies outside the file (%llu bytes). The package is corrupt or truncated.", (unsigned long long)h.manifestOffset, (unsigned long long)fileSize);
            outReason = buf;
            return false;
        }
        if (h.occlusionVoxelCount > 0)
        {
            const uint64_t end = h.occlusionVoxelOffset + (uint64_t)h.occlusionVoxelCount * sizeof(OcclusionVoxelGPU);
            if (h.occlusionVoxelOffset == 0 || end > fileSize)
            {
                outReason = "The occlusion volume data extends past the end of the file. The package is corrupt or truncated.";
                return false;
            }
        }
        if (h.payloadCodec != PAYLOAD_CODEC_LIBRARY && h.payloadCodec != PAYLOAD_CODEC_BROTLI)
        {
            snprintf(buf, sizeof(buf), "The package declares payload codec %u, which this build does not know.", h.payloadCodec);
            outReason = buf;
            return false;
        }
        if (h.recordFormat != RECORD_FORMAT_SPLAT20)
        {
            snprintf(buf, sizeof(buf), "The package declares record format %u; this build reads 3D Gaussian packages (format 1) only.", h.recordFormat);
            outReason = buf;
            return false;
        }
        if (h.splatRecordBytes != SplatRecordBytes(h.recordFormat))
        {
            snprintf(buf, sizeof(buf), "The package declares %u-byte records, but its record format uses %u-byte records.", h.splatRecordBytes, SplatRecordBytes(h.recordFormat));
            outReason = buf;
            return false;
        }
        if (h.shDegree > 3 || (h.shDegree != 0 && h.shRecordBytes != 3u * ((h.shDegree + 1) * (h.shDegree + 1) - 1)))
        {
            snprintf(buf, sizeof(buf), "The package declares spherical-harmonics degree %u with %u-byte records, which do not agree.", h.shDegree, h.shRecordBytes);
            outReason = buf;
            return false;
        }
        if (h.shDegree != 0 && (h.shManifestOffset == 0 || h.shManifestOffset >= fileSize))
        {
            outReason = "The spherical-harmonics table offset lies outside the file. The package is corrupt or truncated.";
            return false;
        }
        if (!(h.splatScaleLog2Max > h.splatScaleLog2Min))
        {
            outReason = "The package's splat scale range is empty or inverted. The package is corrupt.";
            return false;
        }
        return true;
    }

    // Validates every chunk's LOD table against the file size: offsets inside the file and byte sizes
    // consistent with the splat count. Catches truncated downloads and manifests from a different
    // splat layout before any payload is decoded.
    inline bool ValidateSFLWManifest(const std::vector<ChunkManifest>& chunks, uint64_t fileSize, std::string& outReason, uint32_t recordBytes = (uint32_t)sizeof(PackedPointGPU))
    {
        char buf[256];
        if (chunks.empty())
        {
            outReason = "The chunk manifest is empty.";
            return false;
        }
        for (size_t c = 0; c < chunks.size(); c++)
        {
            const ChunkManifest& cm = chunks[c];
            if (cm.lods.empty())
            {
                snprintf(buf, sizeof(buf), "Chunk %zu has no LOD levels.", c);
                outReason = buf;
                return false;
            }
            for (size_t l = 0; l < cm.lods.size(); l++)
            {
                const ChunkLODHeader& lod = cm.lods[l];
                if (lod.fileOffset + (uint64_t)lod.compressedByteSize > fileSize)
                {
                    snprintf(buf, sizeof(buf), "Chunk %zu LOD %zu (%u bytes at offset %llu) extends past the end of the %llu-byte file. The package is truncated.", c, l, lod.compressedByteSize, (unsigned long long)lod.fileOffset, (unsigned long long)fileSize);
                    outReason = buf;
                    return false;
                }
                if ((uint64_t)lod.uncompressedByteSize != (uint64_t)lod.splatCount * recordBytes)
                {
                    snprintf(buf, sizeof(buf), "Chunk %zu LOD %zu declares %u splats but %u payload bytes (expected %llu). The package was written with a different splat layout.", c, l, lod.splatCount, lod.uncompressedByteSize, (unsigned long long)lod.splatCount * recordBytes);
                    outReason = buf;
                    return false;
                }
            }
        }
        return true;
    }

    // The advice appended to every "invalid package" report. Both loaders share it so the wording
    // stays identical in the SplatPak dialog, the standalone viewer's dialog, and the console tools.
    inline const char* SFLWRebakeAdvice()
    {
        return "If the file was copied or downloaded, retry with a fresh copy. Otherwise re-bake it: open the original "
               ".ply or .splat in SplatPak's Package Bake tab and use Save Compressed Package (.gstx). Packages "
               "written by the proprietary codec and by the open stand-in codec are not interchangeable, so a "
               "package must be baked with the same codec build that will read it.";
    }

    // The package path for "<base>" or "<base>.gstx"
    inline std::string ResolvePackagePath(const std::string& inputPath)
    {
        std::string lowerPath = inputPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        if (lowerPath.size() >= 5 && lowerPath.compare(lowerPath.size() - 5, 5, ".gstx") == 0) return inputPath;
        return inputPath + ".gstx";
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
        uint32_t totalDatasetSplats = 0;   // Total points in source model / LOD0
        uint32_t totalDatasetChunks = 0;    // Total chunks in source model
        
        // Stage 1: CPU / LOD Decimation
        uint32_t lodPrunedSplats = 0;      // Splat eliminated by multi-res LOD selection
        uint32_t lodActiveSplats = 0;      // Splat submitted for rendering this frame
        uint32_t lodActiveChunks = 0;       // Chunks submitted this frame
        
        // Stage 2: Amplification / Task Shader (mainAS) Culling
        uint32_t asFrustumCulledChunks = 0; // Chunks culled by view frustum AABB test
        uint32_t asFrustumCulledSplats = 0;// Splat in frustum-culled chunks
        uint32_t asConeCulledChunks = 0;    // Chunks culled by normal cone backface test
        uint32_t asConeCulledSplats = 0;   // Splat in cone-culled chunks
        uint32_t asPassedChunks = 0;        // Chunks passed AS -> dispatched to MS
        uint32_t asPassedSplats = 0;       // Splat dispatched to MS
        
        // Stage 3: Mesh Shader (mainMS) Culling
        uint32_t msDetachedCulledSplats = 0; // Splat culled in MS (e.g. detached cull cam)
        uint32_t msDrawnSplats = 0;          // Splat successfully drawn / rasterized
        
        // Amplification & Rasterizer Output
        uint32_t generatedVertices = 0;     // msDrawnSplats * 4
        uint32_t generatedTriangles = 0;    // msDrawnSplats * 2
        
        // Derived Efficiency Ratios
        float totalCullingRatio = 0.0f;     // Overall reduction % (Total - Drawn) / Total * 100
        float asCullingRatio = 0.0f;        // AS Stage Culling %
        float lodDecimationRatio = 0.0f;    // LOD Decimation %
        float vramBandwidthSavedMB = 0.0f;  // Estimated bandwidth saved vs flat rendering
    };
}


