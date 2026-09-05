// StreamPackager.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Reads and writes the self-contained .sflw binary stream container (header, per-chunk
// LOD payloads, optional occlusion volume, and an embedded chunk manifest table): runs
// each chunk through the wavelet/quantize/shuffle/compress pipeline on export, and
// restores the full multi-LOD chunk hierarchy (plus any baked occlusion volume) on import.
// Packages written before v4 kept their manifest in a companion .json; those still load.

#pragma once
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include "SpatialOctree.h"
#include "Quantizer.h"
#include "../../libs/SurfelsCore/LiftingWavelet.h"
#include "../../libs/SurfelsCore/ByteShuffle.h"

namespace Surfels
{
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
            uint64_t sourceFileBytes = 0)
        {
            std::string sflwPath = outputBasepath + ".sflw";

            std::ofstream sflwOut(sflwPath, std::ios::binary);
            if (!sflwOut.is_open())
            {
                std::cerr << "Failed to open output file: " << sflwPath << std::endl;
                return false;
            }

            // Global Bounding Box
            XMFLOAT3 gMin(1e9f, 1e9f, 1e9f);
            XMFLOAT3 gMax(-1e9f, -1e9f, -1e9f);
            uint64_t totalSurfelsLOD0 = 0;

            for (const auto& c : chunks)
            {
                gMin.x = std::min(gMin.x, c.aabbMin.x);
                gMin.y = std::min(gMin.y, c.aabbMin.y);
                gMin.z = std::min(gMin.z, c.aabbMin.z);

                gMax.x = std::max(gMax.x, c.aabbMax.x);
                gMax.y = std::max(gMax.y, c.aabbMax.y);
                gMax.z = std::max(gMax.z, c.aabbMax.z);

                totalSurfelsLOD0 += c.surfels.size();
            }

            // Write File Header placeholder
            SFLWFileHeader header = {};
            header.magic = SFLW_MAGIC;
            header.version = SFLW_VERSION;
            header.numChunks = (uint32_t)chunks.size();
            header.maxLOD = maxLODs;
            header.totalSurfelsLOD0 = totalSurfelsLOD0;
            header.globalOriginX = 0.0;
            header.globalOriginY = 0.0;
            header.globalOriginZ = 0.0;
            header.globalBoundsMin = gMin;
            header.globalBoundsMax = gMax;
            header.splatRadius = splatRadius;
            header.sourceFileBytes = sourceFileBytes;

            sflwOut.write(reinterpret_cast<const char*>(&header), sizeof(SFLWFileHeader));

            std::vector<ChunkManifest> chunkManifests;
            chunkManifests.reserve(chunks.size());

            size_t totalRawBytes = 0;
            size_t totalCompressedBytes = 0;

            for (size_t i = 0; i < chunks.size(); i++)
            {
                auto& chunk = chunks[i];

                // 1. Decompose via Lifting Wavelet
                auto waveletResult = LiftingWavelet::DecomposeChunk(chunk.surfels, maxLODs, deadbandThreshold);

                ChunkManifest cm = {};
                cm.chunkId = chunk.chunkId;
                cm.aabbMin = chunk.aabbMin;
                cm.aabbMax = chunk.aabbMax;
                cm.center = chunk.center;
                cm.boundingRadius = chunk.boundingRadius;
                cm.numLODs = (uint32_t)waveletResult.lodLevels.size();

                for (const auto& lod : waveletResult.lodLevels)
                {
                    // 2. Quantize to 8-byte PackedSurfelGPU using dataset Global Bounding Box
                    // This guarantees that all streamed chunks unpack with the exact same global AABB in Mesh Shaders & GPU Sorting!
                    auto packedSurfels = Quantizer::QuantizeSurfels(lod.surfels, gMin, gMax);

                    // 3. Byte-Shuffle
                    size_t uncompressedBytes = packedSurfels.size() * sizeof(PackedSurfelGPU);
                    auto shuffled = ByteShuffle::Shuffle(
                        reinterpret_cast<const uint8_t*>(packedSurfels.data()),
                        packedSurfels.size(),
                        sizeof(PackedSurfelGPU)
                    );

                    // 4. Compress
                    auto compressed = ByteShuffle::CompressShuffled(shuffled);

                    uint64_t currentFileOffset = (uint64_t)sflwOut.tellp();
                    sflwOut.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());

                    ChunkLODHeader lh = {};
                    lh.lodLevel = lod.level;
                    lh.surfelCount = (uint32_t)packedSurfels.size();
                    lh.uncompressedByteSize = (uint32_t)uncompressedBytes;
                    lh.compressedByteSize = (uint32_t)compressed.size();
                    lh.fileOffset = currentFileOffset;
                    lh.geometricError = lod.geometricError;

                    cm.lods.push_back(lh);

                    totalRawBytes += uncompressedBytes;
                    totalCompressedBytes += compressed.size();
                }

                chunkManifests.push_back(std::move(cm));
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

            std::cout << "Successfully packaged " << totalSurfelsLOD0 << " surfels across " << chunks.size() << " chunks.\n";
            std::cout << "Uncompressed packed size: " << (totalRawBytes / 1024.0 / 1024.0) << " MB\n";
            std::cout << "Compressed package size:  " << (totalCompressedBytes / 1024.0 / 1024.0) << " MB\n";
            std::cout << "Compression ratio:        " << (totalRawBytes > 0 ? (float)totalRawBytes / totalCompressedBytes : 1.0f) << "x\n";

            return true;
        }

        struct SFLWPackageData
        {
            SFLWFileHeader header = {};
            std::vector<ChunkManifest> chunkManifests;
            std::vector<std::vector<PackedSurfelGPU>> chunkLOD0Surfels; // kept for existing callers (== chunkLODSurfels[c][0])
            std::vector<std::vector<std::vector<PackedSurfelGPU>>> chunkLODSurfels; // [chunkIndex][lodLevelIndex] -> surfels
            std::vector<OcclusionVoxelGPU> occlusionVoxels; // v3+ only; empty on older files or files with no volume baked
            uint64_t totalSurfels = 0;
            size_t totalCompressedBytes = 0;
        };

        static bool LoadPackage(const std::string& inputPath, SFLWPackageData& outPackage)
        {
            std::string sflwPath, jsonPath;
            ResolvePackagePaths(inputPath, sflwPath, jsonPath);

            std::ifstream sflwIn(sflwPath, std::ios::binary);
            if (!sflwIn.is_open())
            {
                std::cerr << "Failed to open .sflw file: " << sflwPath << std::endl;
                return false;
            }

            sflwIn.read(reinterpret_cast<char*>(&outPackage.header), sizeof(SFLWFileHeader));
            if (outPackage.header.magic != SFLW_MAGIC)
            {
                std::cerr << "Invalid SFLW magic header in " << sflwPath << std::endl;
                return false;
            }

            // splatRadius was appended to SFLWFileHeader in version 2. A version-1 file is shorter than
            // sizeof(SFLWFileHeader), so the read above ran past its true header boundary into the start
            // of chunk 0's compressed data (harmless -- every subsequent read seeks by absolute offset,
            // never relative to this position), leaving splatRadius holding misinterpreted chunk bytes
            // rather than a real value. Only trust it on version 2+; default to 1.0 otherwise.
            if (outPackage.header.version < 2)
            {
                outPackage.header.splatRadius = 1.0f;
            }

            // occlusionVoxelCount/Offset were appended in version 3, same reasoning as splatRadius above --
            // on a shorter (v1/v2) file these fields hold misinterpreted chunk bytes, not real values.
            if (outPackage.header.version < 3)
            {
                outPackage.header.occlusionVoxelCount = 0;
                outPackage.header.occlusionVoxelOffset = 0;
            }

            // manifestOffset was appended in version 4; same reasoning again.
            if (outPackage.header.version < 4)
            {
                outPackage.header.manifestOffset = 0;
            }
            if (outPackage.header.version < 5)
            {
                outPackage.header.sourceFileBytes = 0;
            }

            // v4+ packages carry their chunk manifest inside the .sflw; older ones kept it in a
            // companion .json next to the file.
            if (outPackage.header.version >= 4)
            {
                if (!ReadEmbeddedManifest(sflwIn, outPackage.header, outPackage.chunkManifests))
                {
                    std::cerr << "Failed to read embedded manifest in " << sflwPath << std::endl;
                    return false;
                }
            }
            else if (!ParseLegacyJsonManifest(jsonPath, outPackage.chunkManifests))
            {
                std::cerr << "Failed to open/parse companion .json manifest for pre-v4 package: " << jsonPath << std::endl;
                return false;
            }

            outPackage.chunkLOD0Surfels.resize(outPackage.chunkManifests.size());
            outPackage.chunkLODSurfels.resize(outPackage.chunkManifests.size());
            outPackage.totalSurfels = 0;
            outPackage.totalCompressedBytes = 0;

            for (size_t c = 0; c < outPackage.chunkManifests.size(); c++)
            {
                const auto& cm = outPackage.chunkManifests[c];
                if (cm.lods.empty()) continue;

                outPackage.chunkLODSurfels[c].resize(cm.lods.size());

                for (size_t l = 0; l < cm.lods.size(); l++)
                {
                    const auto& lodHeader = cm.lods[l];
                    outPackage.totalCompressedBytes += lodHeader.compressedByteSize;
                    std::vector<uint8_t> compressedBytes(lodHeader.compressedByteSize);
                    sflwIn.seekg(lodHeader.fileOffset, std::ios::beg);
                    sflwIn.read(reinterpret_cast<char*>(compressedBytes.data()), lodHeader.compressedByteSize);

                    std::vector<uint8_t> shuffled(lodHeader.uncompressedByteSize);
                    if (ByteShuffle::DecompressShuffled(compressedBytes.data(), compressedBytes.size(), shuffled.data(), lodHeader.uncompressedByteSize))
                    {
                        outPackage.chunkLODSurfels[c][l].resize(lodHeader.surfelCount);
                        ByteShuffle::Unshuffle(
                            shuffled.data(),
                            reinterpret_cast<uint8_t*>(outPackage.chunkLODSurfels[c][l].data()),
                            lodHeader.surfelCount,
                            sizeof(PackedSurfelGPU)
                        );
                        if (l == 0)
                        {
                            outPackage.totalSurfels += lodHeader.surfelCount;
                        }
                    }
                }

                // Kept for existing callers that only ever wanted the finest level.
                outPackage.chunkLOD0Surfels[c] = outPackage.chunkLODSurfels[c][0];
            }

            outPackage.occlusionVoxels.clear();
            if (outPackage.header.occlusionVoxelCount > 0)
            {
                outPackage.occlusionVoxels.resize(outPackage.header.occlusionVoxelCount);
                sflwIn.seekg(outPackage.header.occlusionVoxelOffset, std::ios::beg);
                sflwIn.read(reinterpret_cast<char*>(outPackage.occlusionVoxels.data()),
                    outPackage.header.occlusionVoxelCount * sizeof(OcclusionVoxelGPU));
            }

            sflwIn.close();
            return true;
        }
    };
}

