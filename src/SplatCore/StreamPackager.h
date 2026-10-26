// StreamPackager.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Reads and writes the self-contained .gstx binary stream container (header, per-chunk
// LOD payloads, optional occlusion volume, and an embedded chunk manifest table): runs
// each chunk through the wavelet/quantize/shuffle/compress pipeline on export, and
// restores the full multi-LOD chunk hierarchy (plus any baked occlusion volume) on import.

#pragma once
#include <fstream>
#include <functional>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include "SpatialOctree.h"
#include "Quantizer.h"
#include "../../libs/gstx/DetailHeatmap.h"
#include "../../libs/gstx/LiftingWavelet.h"
#include "../../libs/gstx/ByteShuffle.h"
#include "../../libs/gstx/SplatCodec.h"
#include "../../libs/gstx/ShadowMeshCodec.h"
#include "PayloadCodec.h"

namespace Splat
{
    // Splat mode input to PackageDataset: the source Gaussians every chunk's points refer to through
    // SplatPoint::sourceIndex, and the spherical-harmonics degree to keep (0, 1 or 3).
    struct SplatBakeInput
    {
        const std::vector<SplatAttributes>* attributes = nullptr;
        uint32_t shDegree = 1;
        // Bits per SH coefficient for bands 1, 2 and 3 (8 = as encoded, lossless). See RequantiseSH.
        uint8_t shBits[3] = { 8, 8, 8 };
    };

    // Requantises one SH stream in place (count entries of shRecordBytes signed bytes) to shBits per band (1, 2, 3).
    // Each coefficient's (R, G, B) is taken to YCoCg-R (ColourTransform.h), each of Y, Co and Cg is snapped to
    // 2^(bits-1) - 1 levels per sign of the signed byte range, and the triple is taken back to RGB. The result is
    // what a reader decodes; the writer's YCoCg transform then stores exactly the snapped values (the transform is
    // a bijection), so the entropy stage sees a small alphabet and the format and readers are unchanged. Snapping
    // Co or Cg can wrap a channel past the end of the signed range in 8-bit arithmetic; a triple whose decode moves
    // a channel by more than a quarter of the range keeps its exact value. Measured on Cthulhu against the 8-bit
    // stream (colour error over all view directions, per splat): 6 bits -23% package size at 46 dB, 5 bits -33%
    // at 41 dB, 4 bits -44% at 35 dB; snapping in RGB instead gave -7%, -25% and -41% (docs/private/sh-compression-study.md).
    inline void RequantiseSH(uint8_t* sh, size_t count, uint32_t shRecordBytes, const uint8_t bits[3])
    {
        int levels[3];
        bool any = false;
        for (int b = 0; b < 3; b++)
        {
            levels[b] = bits[b] >= 8 ? 127 : bits[b] <= 1 ? 0 : (1 << (bits[b] - 1)) - 1;
            any |= bits[b] < 8;
        }
        if (!any || shRecordBytes < 3) return;
        // One table per band: every byte value's snapped value
        uint8_t snapTable[3][256];
        for (int b = 0; b < 3; b++)
            for (int byte = 0; byte < 256; byte++)
            {
                const int L = levels[b], v = (int8_t)byte;
                int out = v;
                if (L == 0) out = 0;
                else if (L < 127)
                {
                    const float step = 127.0f / (float)L;
                    const int q = (int)std::lround(std::fabs((float)v) / step);
                    out = (v < 0 ? -1 : 1) * std::min(127, (int)std::lround(q * step));
                }
                snapTable[b][byte] = (uint8_t)(int8_t)out;
            }
        const uint32_t coeffs = shRecordBytes / 3;
        for (size_t i = 0; i < count; i++)
            for (uint32_t k = 0; k < coeffs; k++)
            {
                const int band = k < 3 ? 0 : k < 8 ? 1 : 2;
                if (levels[band] >= 127) continue;
                const uint8_t* snap = snapTable[band];
                uint8_t* rgb = sh + i * shRecordBytes + 3 * k;
                uint8_t y = rgb[0], co = rgb[1], cg = rgb[2];
                ColourTransform::Forward(y, co, cg);
                uint8_t r = snap[y], g = snap[co], bl = snap[cg];
                ColourTransform::Inverse(r, g, bl);
                if (std::abs((int)(int8_t)r - (int)(int8_t)rgb[0]) > 64 || std::abs((int)(int8_t)g - (int)(int8_t)rgb[1]) > 64 ||
                    std::abs((int)(int8_t)bl - (int)(int8_t)rgb[2]) > 64)
                    continue; // Would wrap: keep the exact triple
                rgb[0] = r; rgb[1] = g; rgb[2] = bl;
            }
    }

    // What the v11 colour transform did to the data a package write encoded: mean order-0 entropy of the byte
    // planes (bits per byte) before and after, byte-weighted over every level. Shown by the save dialog and traced.
    struct TransformStats
    {
        double recordEntropyBefore = 0.0, recordEntropyAfter = 0.0, recordBytes = 0.0;
        double shEntropyBefore = 0.0, shEntropyAfter = 0.0, shBytes = 0.0;
        void Finish()
        {
            if (recordBytes > 0.0) { recordEntropyBefore /= recordBytes; recordEntropyAfter /= recordBytes; }
            if (shBytes > 0.0) { shEntropyBefore /= shBytes; shEntropyAfter /= shBytes; }
        }
        // Both streams together, as a percentage change (negative = lower entropy)
        double TotalChangePercent() const
        {
            const double before = recordEntropyBefore * recordBytes + shEntropyBefore * shBytes;
            const double after = recordEntropyAfter * recordBytes + shEntropyAfter * shBytes;
            return before > 0.0 ? 100.0 * (after / before - 1.0) : 0.0;
        }
    };

    class StreamPackager
    {
    public:
        static bool PackageDataset(
            const std::string& outputBasepath,
            std::vector<ChunkData>& chunks,
            uint32_t maxLODs = 4,
            float deadbandThreshold = 0.003f,
            float splatRadius = 1.0f,
            const std::vector<OcclusionVoxelGPU>& occlusionVoxels = {},
            uint64_t sourceFileBytes = 0,
            const OcclusionMipTable* pOcclusionMips = nullptr, // Mip layout of occlusionVoxels; nullptr = one mip covering the array
            const DetailGrid* detailGrid = nullptr,             // Detail heatmap (v7+); nullptr = none stored
            const SplatBakeInput* splat = nullptr,              // Splat mode (v8+, splat format 1); nullptr = 8-byte splat records
            uint32_t payloadCodec = PayloadCodec::kDefault,     // Entropy stage of every payload (v9+); see PayloadCodec.h
            const std::function<bool(float)>& progress = nullptr, // Fraction done; returning false stops the write and deletes the file
            uint32_t payloadTransforms = PAYLOAD_TRANSFORMS_KNOWN, // Colour stored as YCoCg-R (v11+, ColourTransform.h); 0 = as before, for A/B measurement
            TransformStats* transformStats = nullptr,           // Filled once every level is encoded, before compression starts
            const ShadowMesh* shadowMesh = nullptr,             // Shadow mesh section (v12+); nullptr or empty = none
            const ShadowMesh* occluderMesh = nullptr)           // Occluder mesh section (v13+); with one, pass no occlusionVoxels
        {
            std::string sflwPath = outputBasepath + ".gstx";

            std::ofstream sflwOut(sflwPath, std::ios::binary);
            if (!sflwOut.is_open())
            {
                std::cerr << "Failed to open output file: " << sflwPath << std::endl;
                return false;
            }
            auto stopped = [&](float fraction)
            {
                if (!progress || progress(fraction)) return false;
                sflwOut.close();
                std::remove(sflwPath.c_str());
                return true;
            };

            // Global Bounding Box
            XMFLOAT3 gMin(1e9f, 1e9f, 1e9f);
            XMFLOAT3 gMax(-1e9f, -1e9f, -1e9f);
            uint64_t totalSplatsLOD0 = 0;

            for (const auto& c : chunks)
            {
                gMin.x = std::min(gMin.x, c.aabbMin.x);
                gMin.y = std::min(gMin.y, c.aabbMin.y);
                gMin.z = std::min(gMin.z, c.aabbMin.z);

                gMax.x = std::max(gMax.x, c.aabbMax.x);
                gMax.y = std::max(gMax.y, c.aabbMax.y);
                gMax.z = std::max(gMax.z, c.aabbMax.z);

                totalSplatsLOD0 += c.splats.size();
            }

            // Write File Header placeholder
            SFLWFileHeader header = {};
            header.magic = GSTX_MAGIC; // "GSTX" from version 10 (WaveletTypes.h)
            header.version = SFLW_VERSION;
            header.numChunks = (uint32_t)chunks.size();
            header.maxLOD = maxLODs;
            header.totalSplatsLOD0 = totalSplatsLOD0;
            header.globalOriginX = 0.0;
            header.globalOriginY = 0.0;
            header.globalOriginZ = 0.0;
            header.globalBoundsMin = gMin;
            header.globalBoundsMax = gMax;
            header.splatRadius = splatRadius;
            header.sourceFileBytes = sourceFileBytes;
            header.payloadCodec = payloadCodec;

            // Record format (v8+). Splat mode encodes every level through SplatCodec against one
            // global scale range, so the header carries what a decoder needs to invert it.
            const bool splatMode = (splat != nullptr && splat->attributes != nullptr);
            SplatEncodeParams splatParams;
            header.recordFormat = splatMode ? RECORD_FORMAT_SPLAT20 : RECORD_FORMAT_POINT8;
            header.splatRecordBytes = SplatRecordBytes(header.recordFormat);
            header.shDegree = 0;
            header.shRecordBytes = 0;
            header.splatScaleLog2Min = 0.0f;
            header.splatScaleLog2Max = 0.0f;
            header.shManifestOffset = 0;
            if (splatMode)
            {
                splatParams.shDegree = std::min(splat->shDegree, 3u); // 0 (none) to 3; the record size follows the degree (SplatCodec::SHRecordBytes)
                splatParams.aabbMin = gMin;
                splatParams.aabbMax = gMax;
                SplatCodec::ComputeScaleRange(*splat->attributes, maxLODs, splatParams.scaleLog2Min, splatParams.scaleLog2Max);
                header.shDegree = splatParams.shDegree;
                header.shRecordBytes = SplatCodec::SHRecordBytes(splatParams.shDegree);
                header.splatScaleLog2Min = splatParams.scaleLog2Min;
                header.splatScaleLog2Max = splatParams.scaleLog2Max;
            }
            const uint32_t recordBytes = header.splatRecordBytes;
            // The colour transform applies to the 20-byte splat record and its SH stream; point records are legacy
            header.payloadTransforms = splatMode ? (payloadTransforms & PAYLOAD_TRANSFORMS_KNOWN) : 0u;
            TransformStats stats;

            sflwOut.write(reinterpret_cast<const char*>(&header), sizeof(SFLWFileHeader));

            std::vector<ChunkManifest> chunkManifests;
            chunkManifests.reserve(chunks.size());
            std::vector<SHLODRecord> shRecords; // Splat mode: one per chunk LOD, manifest order

            size_t totalRawBytes = 0;
            size_t totalCompressedBytes = 0;

            // Three passes. Every level is decomposed, encoded and byte-shuffled first (the codec library,
            // one chunk after another); then every payload is compressed at once across the cores (Brotli
            // at the package quality runs at about 1 MB/s per core and is the bulk of a bake); then the
            // payloads are written in order and the manifest takes their offsets.
            struct LevelPayloads
            {
                uint32_t lodLevel = 0;
                uint32_t recordCount = 0;
                uint32_t uncompressedBytes = 0;   // Records, before compression
                uint32_t shUncompressedBytes = 0; // SH stream, before compression (0 = none)
                float    geometricError = 0.0f;
                std::vector<uint8_t> records;     // Shuffled, then compressed in place
                std::vector<uint8_t> sh;
            };
            std::vector<std::vector<LevelPayloads>> chunkLevels(chunks.size());

            for (size_t i = 0; i < chunks.size(); i++)
            {
                if (stopped(0.03f * (float)i / (float)std::max<size_t>(1, chunks.size()))) return false;
                auto& chunk = chunks[i];

                // 1. Decompose via Lifting Wavelet
                auto waveletResult = LiftingWavelet::DecomposeChunk(chunk.splats, maxLODs, deadbandThreshold);

                ChunkManifest cm = {};
                cm.chunkId = chunk.chunkId;
                cm.aabbMin = chunk.aabbMin;
                cm.aabbMax = chunk.aabbMax;
                cm.center = chunk.center;
                cm.boundingRadius = chunk.boundingRadius;
                cm.numLODs = (uint32_t)waveletResult.lodLevels.size();
                chunkManifests.push_back(std::move(cm)); // Its LOD headers are filled in when the payloads are written

                for (const auto& lod : waveletResult.lodLevels)
                {
                    // 2. Quantize against the dataset's global bounding box, so every streamed chunk
                    // unpacks with the exact same frame in the shaders and the GPU sort.
                    std::vector<PackedPointGPU> packedPoints;
                    std::vector<PackedSplatGPU> packedSplats;
                    std::vector<uint8_t> shStream;
                    const uint8_t* recordBytesPtr = nullptr;
                    size_t recordCount = lod.splats.size();
                    if (splatMode)
                    {
                        SplatCodec::Encode(lod.splats, *splat->attributes, lod.level, splatParams, packedSplats, shStream);
                        RequantiseSH(shStream.data(), recordCount, header.shRecordBytes, splat->shBits);
                        recordBytesPtr = reinterpret_cast<const uint8_t*>(packedSplats.data());
                    }
                    else
                    {
                        packedPoints = Quantizer::QuantizePoints(lod.splats, gMin, gMax);
                        recordBytesPtr = reinterpret_cast<const uint8_t*>(packedPoints.data());
                    }

                    // 3. Colour as YCoCg-R (v11), measured before and after, then the byte shuffle (the SH stream too,
                    // at its own stride)
                    if (splatMode && recordCount > 0)
                    {
                        uint8_t* rec = reinterpret_cast<uint8_t*>(packedSplats.data());
                        const double w = (double)recordCount * recordBytes;
                        stats.recordEntropyBefore += w * ColourTransform::MeanPlaneEntropy(rec, recordCount, recordBytes);
                        if (header.payloadTransforms & PAYLOAD_TRANSFORM_RECORD_YCOCG) ColourTransform::ApplyRecords(rec, recordCount, recordBytes);
                        stats.recordEntropyAfter += w * ColourTransform::MeanPlaneEntropy(rec, recordCount, recordBytes);
                        stats.recordBytes += w;
                        if (!shStream.empty())
                        {
                            const double ws = (double)shStream.size();
                            stats.shEntropyBefore += ws * ColourTransform::MeanPlaneEntropy(shStream.data(), recordCount, header.shRecordBytes);
                            if (header.payloadTransforms & PAYLOAD_TRANSFORM_SH_YCOCG) ColourTransform::ApplySH(shStream.data(), shStream.size());
                            stats.shEntropyAfter += ws * ColourTransform::MeanPlaneEntropy(shStream.data(), recordCount, header.shRecordBytes);
                            stats.shBytes += ws;
                        }
                    }
                    LevelPayloads lp;
                    lp.lodLevel = lod.level;
                    lp.recordCount = (uint32_t)recordCount;
                    lp.uncompressedBytes = (uint32_t)(recordCount * recordBytes);
                    lp.geometricError = lod.geometricError;
                    lp.records = ByteShuffle::Shuffle(recordBytesPtr, recordCount, recordBytes);
                    if (splatMode && header.shRecordBytes > 0 && !shStream.empty())
                    {
                        lp.sh = ByteShuffle::Shuffle(shStream.data(), recordCount, header.shRecordBytes);
                        lp.shUncompressedBytes = (uint32_t)shStream.size();
                    }
                    chunkLevels[i].push_back(std::move(lp));
                }
            }

            stats.Finish();
            if (transformStats) *transformStats = stats;

            // 4. Compress every payload (PayloadCodec: Brotli across the cores)
            {
                std::vector<std::vector<uint8_t>*> payloads;
                for (auto& levels : chunkLevels)
                    for (auto& lp : levels)
                    {
                        payloads.push_back(&lp.records);
                        if (lp.shUncompressedBytes > 0) payloads.push_back(&lp.sh);
                    }
                // Nearly all of a save is spent here (Brotli at package quality, ~95% of the time on Cthulhu): 3% to 97% of the progress
                std::function<bool(float)> compressProgress;
                if (progress) compressProgress = [&](float f) { return progress(0.03f + 0.94f * f); };
                if (!PayloadCodec::CompressInPlace(payloads, header.payloadCodec, PayloadCodec::kPackageQuality, compressProgress))
                {
                    stopped(0.03f); // Cancelled: close and delete the file
                    return false;
                }
                for (const auto& levels : chunkLevels)
                    for (const auto& lp : levels)
                        if ((lp.uncompressedBytes > 0 && lp.records.empty()) || (lp.shUncompressedBytes > 0 && lp.sh.empty()))
                        {
                            std::cerr << "The payload codec (" << PayloadCodec::Name(header.payloadCodec) << ") failed to compress a level; out of memory?" << std::endl;
                            return false;
                        }
            }

            // 5. Write the payloads in order: each level's records, then (splat mode) its SH stream
            for (size_t i = 0; i < chunks.size(); i++)
            {
                if (stopped(0.97f + 0.03f * (float)i / (float)std::max<size_t>(1, chunks.size()))) return false;
                auto& cm = chunkManifests[i];
                for (auto& lp : chunkLevels[i])
                {
                    ChunkLODHeader lh = {};
                    lh.lodLevel = lp.lodLevel;
                    lh.splatCount = lp.recordCount;
                    lh.uncompressedByteSize = lp.uncompressedBytes;
                    lh.compressedByteSize = (uint32_t)lp.records.size();
                    lh.fileOffset = (uint64_t)sflwOut.tellp();
                    lh.geometricError = lp.geometricError;
                    sflwOut.write(reinterpret_cast<const char*>(lp.records.data()), lp.records.size());

                    if (splatMode)
                    {
                        SHLODRecord sr = {};
                        if (lp.shUncompressedBytes > 0)
                        {
                            sr.fileOffset = (uint64_t)sflwOut.tellp();
                            sr.uncompressedByteSize = lp.shUncompressedBytes;
                            sr.compressedByteSize = (uint32_t)lp.sh.size();
                            sflwOut.write(reinterpret_cast<const char*>(lp.sh.data()), lp.sh.size());
                            totalRawBytes += lp.shUncompressedBytes;
                            totalCompressedBytes += lp.sh.size();
                        }
                        shRecords.push_back(sr);
                    }

                    cm.lods.push_back(lh);

                    totalRawBytes += lp.uncompressedBytes;
                    totalCompressedBytes += lp.records.size();

                    std::vector<uint8_t>().swap(lp.records); // Written: free as we go
                    std::vector<uint8_t>().swap(lp.sh);
                }
            }

            // Occlusion voxels (v3+, optional) go after all chunk LOD data. Written after the payloads
            // since their count/offset weren't known when the header placeholder above was first written.
            header.occlusionVoxelCount = (uint32_t)occlusionVoxels.size();
            if (!occlusionVoxels.empty())
            {
                header.occlusionVoxelOffset = (uint64_t)sflwOut.tellp();
                sflwOut.write(reinterpret_cast<const char*>(occlusionVoxels.data()),
                    occlusionVoxels.size() * sizeof(OcclusionVoxelGPU));
            }
            else
            {
                header.occlusionVoxelOffset = 0;
            }
            // Mip table (v6+): how the voxel array above splits into its nested mips (mip 0 first). A caller
            // that never asked for mips, or handed a table that does not add up to the array, gets a
            // one-mip table covering the whole array -- exactly what every pre-v6 package implicitly was.
            {
                OcclusionMipTable mips;
                if (pOcclusionMips != nullptr && pOcclusionMips->mipCount > 0 && pOcclusionMips->mipCount <= kMaxOcclusionMips
                    && pOcclusionMips->TotalBlocks() == (uint32_t)occlusionVoxels.size())
                    mips = *pOcclusionMips;
                else
                    mips = OcclusionMipTable::SingleLevel(occlusionVoxels.data(), (uint32_t)occlusionVoxels.size());
                header.occlusionMipCount = mips.mipCount;
                for (uint32_t k = 0; k < kMaxOcclusionMips; k++)
                {
                    header.occlusionMipBlockCount[k] = mips.blockCount[k];
                    header.occlusionMipCellSize[k]   = mips.cellSize[k];
                }
            }

            // Detail heatmap grid (v7+, optional): one byte per cell, written after the occlusion voxels.
            // The streamer reads it back as an input to its delivery order (see StreamOrder.h).
            header.detailGridDims[0] = header.detailGridDims[1] = header.detailGridDims[2] = 0;
            header.detailGridCellSize = 0.0f;
            header.detailGridOffset = 0;
            if (detailGrid != nullptr && detailGrid->Valid())
            {
                header.detailGridDims[0] = detailGrid->nx;
                header.detailGridDims[1] = detailGrid->ny;
                header.detailGridDims[2] = detailGrid->nz;
                header.detailGridCellSize = detailGrid->cellSize;
                header.detailGridOffset = (uint64_t)sflwOut.tellp();
                sflwOut.write(reinterpret_cast<const char*>(detailGrid->score.data()), detailGrid->score.size());
            }

            // Splat mode SH table (v8+): one SHLODRecord per chunk LOD, in manifest order.
            if (splatMode && header.shRecordBytes > 0 && !shRecords.empty())
            {
                header.shManifestOffset = (uint64_t)sflwOut.tellp();
                sflwOut.write(reinterpret_cast<const char*>(shRecords.data()), sizeof(SHLODRecord) * shRecords.size());
            }

            // Shadow mesh section (v12+, optional; ShadowMeshCodec.h)
            header.shadowMeshOffset = 0;
            header.shadowMeshBytes = 0;
            if (shadowMesh != nullptr && !shadowMesh->Empty())
            {
                std::vector<uint8_t> section;
                if (ShadowMeshCodec::Encode(*shadowMesh, section))
                {
                    header.shadowMeshOffset = (uint64_t)sflwOut.tellp();
                    header.shadowMeshBytes = section.size();
                    sflwOut.write(reinterpret_cast<const char*>(section.data()), section.size());
                }
            }

            // Occluder mesh section (v13+, optional; same encoding)
            header.occluderMeshOffset = 0;
            header.occluderMeshBytes = 0;
            if (occluderMesh != nullptr && !occluderMesh->Empty())
            {
                std::vector<uint8_t> section;
                if (ShadowMeshCodec::Encode(*occluderMesh, section))
                {
                    header.occluderMeshOffset = (uint64_t)sflwOut.tellp();
                    header.occluderMeshBytes = section.size();
                    sflwOut.write(reinterpret_cast<const char*>(section.data()), section.size());
                }
            }

            // Embedded manifest table (v4+) goes last: one ChunkManifestRecord per chunk, each followed
            // by its ChunkLODHeader array. Every LOD payload offset is already absolute, so the manifest
            // can sit anywhere; the end of the file keeps the payload region contiguous.
            header.manifestOffset = (uint64_t)sflwOut.tellp();
            for (const auto& c : chunkManifests)
            {
                ChunkManifestRecord rec = {};
                rec.chunkId        = c.chunkId;
                rec.aabbMin        = c.aabbMin;
                rec.aabbMax        = c.aabbMax;
                rec.center         = c.center;
                rec.boundingRadius = c.boundingRadius;
                rec.numLODs        = (uint32_t)c.lods.size();
                sflwOut.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
                if (!c.lods.empty())
                {
                    sflwOut.write(reinterpret_cast<const char*>(c.lods.data()), sizeof(ChunkLODHeader) * c.lods.size());
                }
            }

            // Patch the header in place now that every offset is known.
            sflwOut.seekp(0, std::ios::beg);
            sflwOut.write(reinterpret_cast<const char*>(&header), sizeof(SFLWFileHeader));
            sflwOut.seekp(0, std::ios::end);

            sflwOut.close();
            if (!sflwOut.good())
            {
                std::cerr << "Failed while writing " << sflwPath << std::endl;
                return false;
            }

            std::cout << "Successfully packaged " << totalSplatsLOD0 << " splats across " << chunks.size() << " chunks.\n";
            std::cout << "Uncompressed packed size: " << (totalRawBytes / 1024.0 / 1024.0) << " MB\n";
            std::cout << "Compressed package size:  " << (totalCompressedBytes / 1024.0 / 1024.0) << " MB\n";
            std::cout << "Compression ratio:        " << (totalRawBytes > 0 ? (float)totalRawBytes / totalCompressedBytes : 1.0f) << "x\n";

            return true;
        }

        struct SFLWPackageData
        {
            SFLWFileHeader header = {};
            std::vector<ChunkManifest> chunkManifests;
            // The records and the SH stream per chunk LOD, plus the parameters that decode them (SplatCodec::Decode)
            std::vector<std::vector<std::vector<PackedSplatGPU>>> chunkLODSplats;
            std::vector<std::vector<std::vector<uint8_t>>>        chunkLODSH;
            SplatEncodeParams splatParams;
            bool IsSplatPackage() const { return header.recordFormat == RECORD_FORMAT_SPLAT20; } // Every package that loads
            std::vector<OcclusionVoxelGPU> occlusionVoxels; // Empty when no volume was baked, or the package has an occluder mesh
            OcclusionMipTable occlusionMips;                // Mip layout of occlusionVoxels
            DetailGrid detailGrid;                          // Invalid (nx == 0) when none was stored -- the app rebuilds one from LOD 0
            ShadowMesh shadowMesh;                          // Empty when the package has no mesh section
            ShadowMesh occluderMesh;                        // When present the package has no voxel volume
            uint64_t totalSplats = 0;
            size_t totalCompressedBytes = 0;
        };

        // Loads a package into memory. On failure returns false and, when outError is given, fills it
        // with a one-paragraph explanation of what is wrong with the file (bad signature, newer format
        // version, truncation, a payload the linked codec cannot decode, ...) that the caller can put in
        // front of the user. Every check goes through the shared validators in WaveletTypes.h so the
        // standalone viewer's StreamingManager reports the same faults the same way.
        static bool LoadPackage(const std::string& inputPath, SFLWPackageData& outPackage, std::string* outError = nullptr)
        {
            const std::string sflwPath = ResolvePackagePath(inputPath);

            auto fail = [&](const std::string& reason) -> bool
            {
                std::cerr << "LoadPackage: " << sflwPath << ": " << reason << std::endl;
                if (outError) *outError = reason;
                return false;
            };

            std::ifstream sflwIn(sflwPath, std::ios::binary);
            if (!sflwIn.is_open())
            {
                return fail("The file could not be opened: " + sflwPath);
            }

            sflwIn.seekg(0, std::ios::end);
            const uint64_t fileSize = (uint64_t)sflwIn.tellg();
            sflwIn.seekg(0, std::ios::beg);

            // One full-struct read; a short file reads fewer bytes, so clear the failbit and let the validator decide
            outPackage.header = {};
            sflwIn.read(reinterpret_cast<char*>(&outPackage.header), sizeof(SFLWFileHeader));
            const size_t headerBytesRead = (size_t)sflwIn.gcount();
            sflwIn.clear();

            std::string reason;
            if (!ValidateSFLWHeader(outPackage.header, headerBytesRead, fileSize, reason))
            {
                return fail(reason);
            }

            if (!ReadEmbeddedManifest(sflwIn, outPackage.header, outPackage.chunkManifests))
            {
                return fail("The embedded chunk manifest could not be read; the package is corrupt or truncated.");
            }
            const bool splatPackage = outPackage.IsSplatPackage();
            const uint32_t recordBytes = SplatRecordBytes(outPackage.header.recordFormat);
            if (!ValidateSFLWManifest(outPackage.chunkManifests, fileSize, reason, recordBytes))
            {
                return fail(reason);
            }

            // Splat mode: the SH table, one record per chunk LOD in manifest order.
            std::vector<SHLODRecord> shRecords;
            if (splatPackage)
            {
                outPackage.splatParams.shDegree = outPackage.header.shDegree;
                outPackage.splatParams.scaleLog2Min = outPackage.header.splatScaleLog2Min;
                outPackage.splatParams.scaleLog2Max = outPackage.header.splatScaleLog2Max;
                outPackage.splatParams.aabbMin = outPackage.header.globalBoundsMin;
                outPackage.splatParams.aabbMax = outPackage.header.globalBoundsMax;
                size_t lodCount = 0;
                for (const auto& cm : outPackage.chunkManifests) lodCount += cm.lods.size();
                if (outPackage.header.shRecordBytes > 0 && outPackage.header.shManifestOffset > 0)
                {
                    if (outPackage.header.shManifestOffset + lodCount * sizeof(SHLODRecord) > fileSize)
                        return fail("The spherical-harmonics table extends past the end of the file. The package is truncated.");
                    shRecords.resize(lodCount);
                    sflwIn.seekg((std::streamoff)outPackage.header.shManifestOffset, std::ios::beg);
                    sflwIn.read(reinterpret_cast<char*>(shRecords.data()), (std::streamsize)(sizeof(SHLODRecord) * lodCount));
                    if (!sflwIn.good()) return fail("The spherical-harmonics table could not be read; the package is corrupt or truncated.");
                    for (size_t r = 0; r < shRecords.size(); r++)
                    {
                        if (shRecords[r].compressedByteSize != 0 && shRecords[r].fileOffset + (uint64_t)shRecords[r].compressedByteSize > fileSize)
                            return fail("A spherical-harmonics payload extends past the end of the file. The package is truncated.");
                    }
                }
            }

            outPackage.chunkLODSplats.resize(splatPackage ? outPackage.chunkManifests.size() : 0);
            outPackage.chunkLODSH.resize(splatPackage ? outPackage.chunkManifests.size() : 0);
            outPackage.totalSplats = 0;
            outPackage.totalCompressedBytes = 0;
            size_t shRecordCursor = 0;

            for (size_t c = 0; c < outPackage.chunkManifests.size(); c++)
            {
                const auto& cm = outPackage.chunkManifests[c];
                if (cm.lods.empty()) continue;

                if (splatPackage)
                {
                    outPackage.chunkLODSplats[c].resize(cm.lods.size());
                    outPackage.chunkLODSH[c].resize(cm.lods.size());
                }

                for (size_t l = 0; l < cm.lods.size(); l++)
                {
                    const auto& lodHeader = cm.lods[l];
                    outPackage.totalCompressedBytes += lodHeader.compressedByteSize;
                    std::vector<uint8_t> compressedBytes(lodHeader.compressedByteSize);
                    sflwIn.seekg(lodHeader.fileOffset, std::ios::beg);
                    sflwIn.read(reinterpret_cast<char*>(compressedBytes.data()), lodHeader.compressedByteSize);
                    if (!sflwIn.good())
                    {
                        char buf[160];
                        snprintf(buf, sizeof(buf), "Chunk %zu LOD %zu could not be read from the file; the package is truncated.", c, l);
                        return fail(buf);
                    }

                    // A payload the codec rejects is corrupt data, or (the codec library's stage) a package baked
                    // with the other codec build (proprietary vs. open stand-in) -- fail loudly.
                    std::vector<uint8_t> shuffled(lodHeader.uncompressedByteSize);
                    if (!PayloadCodec::Decompress(compressedBytes.data(), compressedBytes.size(), shuffled.data(), lodHeader.uncompressedByteSize, outPackage.header.payloadCodec))
                    {
                        char buf[224];
                        snprintf(buf, sizeof(buf), "Chunk %zu LOD %zu holds %s data this build cannot decode (%u -> %u bytes). The package is corrupt, or was baked with a different codec build.",
                            c, l, PayloadCodec::Name(outPackage.header.payloadCodec), lodHeader.compressedByteSize, lodHeader.uncompressedByteSize);
                        return fail(buf);
                    }

                    if (splatPackage)
                    {
                        outPackage.chunkLODSplats[c][l].resize(lodHeader.splatCount);
                        ByteShuffle::Unshuffle(shuffled.data(), reinterpret_cast<uint8_t*>(outPackage.chunkLODSplats[c][l].data()), lodHeader.splatCount, recordBytes);
                        if (outPackage.header.payloadTransforms & PAYLOAD_TRANSFORM_RECORD_YCOCG)
                            ColourTransform::UndoRecords(reinterpret_cast<uint8_t*>(outPackage.chunkLODSplats[c][l].data()), lodHeader.splatCount, recordBytes);

                        // The level's spherical-harmonics stream, when the package carries one.
                        const size_t r = shRecordCursor++;
                        if (r < shRecords.size() && shRecords[r].compressedByteSize > 0 && shRecords[r].uncompressedByteSize == (uint32_t)lodHeader.splatCount * outPackage.header.shRecordBytes)
                        {
                            std::vector<uint8_t> shCompressed(shRecords[r].compressedByteSize);
                            sflwIn.seekg((std::streamoff)shRecords[r].fileOffset, std::ios::beg);
                            sflwIn.read(reinterpret_cast<char*>(shCompressed.data()), shRecords[r].compressedByteSize);
                            if (!sflwIn.good())
                            {
                                char buf[160];
                                snprintf(buf, sizeof(buf), "Chunk %zu LOD %zu spherical harmonics could not be read from the file; the package is truncated.", c, l);
                                return fail(buf);
                            }
                            std::vector<uint8_t> shShuffled(shRecords[r].uncompressedByteSize);
                            if (!PayloadCodec::Decompress(shCompressed.data(), shCompressed.size(), shShuffled.data(), shRecords[r].uncompressedByteSize, outPackage.header.payloadCodec))
                            {
                                char buf[224];
                                snprintf(buf, sizeof(buf), "Chunk %zu LOD %zu holds spherical-harmonics data (%s) this build cannot decode. The package is corrupt, or was baked with a different codec build.", c, l, PayloadCodec::Name(outPackage.header.payloadCodec));
                                return fail(buf);
                            }
                            outPackage.chunkLODSH[c][l].resize(shRecords[r].uncompressedByteSize);
                            ByteShuffle::Unshuffle(shShuffled.data(), outPackage.chunkLODSH[c][l].data(), lodHeader.splatCount, outPackage.header.shRecordBytes);
                            if (outPackage.header.payloadTransforms & PAYLOAD_TRANSFORM_SH_YCOCG)
                                ColourTransform::UndoSH(outPackage.chunkLODSH[c][l].data(), outPackage.chunkLODSH[c][l].size());
                            outPackage.totalCompressedBytes += shRecords[r].compressedByteSize;
                        }
                    }
                    if (l == 0)
                    {
                        outPackage.totalSplats += lodHeader.splatCount;
                    }
                }
            }

            outPackage.occlusionVoxels.clear();
            outPackage.occlusionMips = OcclusionMipTable{};
            if (outPackage.header.occlusionVoxelCount > 0)
            {
                outPackage.occlusionVoxels.resize(outPackage.header.occlusionVoxelCount);
                sflwIn.seekg(outPackage.header.occlusionVoxelOffset, std::ios::beg);
                sflwIn.read(reinterpret_cast<char*>(outPackage.occlusionVoxels.data()),
                    outPackage.header.occlusionVoxelCount * sizeof(OcclusionVoxelGPU));

                // A mip table that does not add up to the array is treated as one mip covering everything
                OcclusionMipTable mips;
                mips.mipCount = (outPackage.header.occlusionMipCount <= kMaxOcclusionMips) ? outPackage.header.occlusionMipCount : kMaxOcclusionMips;
                for (uint32_t k = 0; k < mips.mipCount; k++)
                {
                    mips.blockCount[k] = outPackage.header.occlusionMipBlockCount[k];
                    mips.cellSize[k]   = outPackage.header.occlusionMipCellSize[k];
                }
                if (mips.mipCount == 0 || mips.TotalBlocks() != outPackage.header.occlusionVoxelCount)
                    mips = OcclusionMipTable::SingleLevel(outPackage.occlusionVoxels.data(), outPackage.header.occlusionVoxelCount);
                outPackage.occlusionMips = mips;
            }

            // Detail heatmap grid. Sanity-check the dimensions before trusting the offset.
            outPackage.detailGrid = DetailGrid{};
            {
                const SFLWFileHeader& h = outPackage.header;
                const uint64_t cells = (uint64_t)h.detailGridDims[0] * h.detailGridDims[1] * h.detailGridDims[2];
                if (cells > 0 && cells <= (uint64_t)(96 * 96 * 96) && h.detailGridCellSize > 0.0f && h.detailGridOffset > 0)
                {
                    DetailGrid& dg = outPackage.detailGrid;
                    dg.nx = h.detailGridDims[0]; dg.ny = h.detailGridDims[1]; dg.nz = h.detailGridDims[2];
                    dg.cellSize = h.detailGridCellSize;
                    dg.gMin = h.globalBoundsMin;
                    dg.score.resize((size_t)cells);
                    sflwIn.seekg((std::streamoff)h.detailGridOffset, std::ios::beg);
                    sflwIn.read(reinterpret_cast<char*>(dg.score.data()), (std::streamsize)cells);
                    if (!sflwIn.good()) { dg = DetailGrid{}; sflwIn.clear(); }
                    else { for (uint8_t v : dg.score) if (v) dg.occupiedCells++; }
                }
            }

            // Shadow mesh section
            outPackage.shadowMesh = ShadowMesh{};
            if (outPackage.header.shadowMeshOffset != 0)
            {
                std::vector<uint8_t> section((size_t)outPackage.header.shadowMeshBytes);
                sflwIn.clear();
                sflwIn.seekg((std::streamoff)outPackage.header.shadowMeshOffset, std::ios::beg);
                sflwIn.read(reinterpret_cast<char*>(section.data()), (std::streamsize)section.size());
                std::string meshError;
                if (!sflwIn.good() || !ShadowMeshCodec::Decode(section.data(), section.size(), outPackage.shadowMesh, meshError))
                {
                    return fail("The shadow mesh section cannot be read: " + (sflwIn.good() ? meshError : std::string("the file is truncated.")));
                }
            }

            // Occluder mesh section
            outPackage.occluderMesh = ShadowMesh{};
            if (outPackage.header.occluderMeshOffset != 0)
            {
                std::vector<uint8_t> section((size_t)outPackage.header.occluderMeshBytes);
                sflwIn.clear();
                sflwIn.seekg((std::streamoff)outPackage.header.occluderMeshOffset, std::ios::beg);
                sflwIn.read(reinterpret_cast<char*>(section.data()), (std::streamsize)section.size());
                std::string meshError;
                if (!sflwIn.good() || !ShadowMeshCodec::Decode(section.data(), section.size(), outPackage.occluderMesh, meshError))
                {
                    return fail("The occluder mesh section cannot be read: " + (sflwIn.good() ? meshError : std::string("the file is truncated.")));
                }
            }

            sflwIn.close();
            return true;
        }
    };
}

