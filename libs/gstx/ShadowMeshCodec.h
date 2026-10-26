// ShadowMeshCodec.h
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Encodes and decodes the shadow mesh section of a .gstx package (format version 12, Clause 12 of
// docs/GSTX_FORMAT_SPECIFICATION.md). Open code, inside the codec library so every writer and reader (SplatPak, the
// console tools, the UE5 plugin) shares one implementation, like PayloadDecode.
//
// Section layout (little-endian):
//   u32 magic "GSMS", u32 levelCount, f32[3] boundsMin, f32[3] boundsMax   (positions are quantised over these bounds)
//   per level: u32 vertexCount, u32 triangleCount, u32 compressedBytes, u32 uncompressedBytes
//   per level, in order: one Brotli stream of compressedBytes that decodes to uncompressedBytes bytes:
//     positions  vertexCount x (u16 x, u16 y, u16 z), byte-plane transposed (stride 6)
//     colours    vertexCount x (u8 r, u8 g, u8 b), byte-plane transposed (stride 3)
//     indices    triangleCount x 3 x (u16 when vertexCount <= 65536, else u32), byte-plane transposed

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ShadowMesh.h"

namespace Splat
{
    struct ShadowMeshCodec
    {
        static constexpr uint32_t kMagic = 0x534D5347; // "GSMS" as bytes 47 53 4D 53

        // Appends the section's bytes to out. Brotli quality 11. Returns false for an empty mesh.
        static bool Encode(const ShadowMesh& mesh, std::vector<uint8_t>& out);

        // Decodes a section; false (with a reason) when the bytes are not a well-formed section.
        static bool Decode(const uint8_t* data, size_t size, ShadowMesh& out, std::string& error);

        // The section's level and triangle counts without decoding the payloads (for listings and tests)
        static bool ReadCounts(const uint8_t* data, size_t size, std::vector<uint32_t>& vertexCounts,
                               std::vector<uint32_t>& triangleCounts, std::string& error);
    };
}
