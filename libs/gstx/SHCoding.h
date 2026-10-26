// SHCoding.h
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Spherical-harmonics coding of .gstx splat packages (Clauses 8.1 to 8.3 of docs/GSTX_FORMAT_SPECIFICATION.md), shared by
// the writer and every reader. Header-only, open code.
//
//   - RequantiseSH (SH Precision): snaps each coefficient's colour triple, in YCoCg-R space, to 2^(bits-1) - 1 levels per
//     sign of the signed byte range, keeping the 8-bit slots.
//   - Residuals (PAYLOAD_TRANSFORM_SH_RESIDUAL): within a chunk, every level but the coarsest stores its SH as the
//     difference, in YCoCg-R space and modulo 256, from a prediction: the SH of the nearest point of the next coarser
//     level (by the records' 16-bit positions), rescaled to the point's own SH scale and snapped like the stream. The
//     records keep their absolute SH scale, so decoding restores every byte exactly and the renderer is unchanged. All of
//     it is integer arithmetic, so every reader predicts exactly what the writer did.

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "ColourTransform.h"

namespace Splat
{
    struct SHCoding
    {
        // ---- SH Precision --------------------------------------------------------------------------------------------

        // Snapped value of every signed byte for 2^(bits-1) - 1 levels per sign (bits >= 8: unchanged): the nearest of the
        // levels at multiples of 127 / levels, both roundings half away from zero
        static void SnapTable(uint8_t bits, uint8_t table[256])
        {
            const int levels = bits >= 8 ? 127 : bits <= 1 ? 0 : (1 << (bits - 1)) - 1;
            for (int byte = 0; byte < 256; byte++)
            {
                const int v = (int8_t)byte, a = std::abs(v);
                int out = v;
                if (levels == 0) out = 0;
                else if (levels < 127)
                {
                    const int q = (2 * a * levels + 127) / 254;           // round(a / step), step = 127 / levels
                    const int s = (2 * q * 127 + levels) / (2 * levels); // round(q * step)
                    out = (v < 0 ? -1 : 1) * std::min(127, s);
                }
                table[byte] = (uint8_t)(int8_t)out;
            }
        }

        // The SH Precision of bands 1, 2, 3 in a header's payloadTransforms (PAYLOAD_SH_BITS_SHIFT), and back
        static uint32_t PackBits(const uint8_t bits[3])
        {
            uint32_t v = 0;
            for (int b = 0; b < 3; b++) v |= (uint32_t)(bits[b] >= 8 ? 0 : bits[b]) << (PAYLOAD_SH_BITS_SHIFT + 4 * b);
            return v;
        }
        static void UnpackBits(uint32_t transforms, uint8_t bits[3])
        {
            for (int b = 0; b < 3; b++)
            {
                const uint32_t v = (transforms >> (PAYLOAD_SH_BITS_SHIFT + 4 * b)) & 0xFu;
                bits[b] = (uint8_t)(v == 0 || v > 8 ? 8 : v);
            }
        }

        static int Band(uint32_t coefficient) { return coefficient < 3 ? 0 : coefficient < 8 ? 1 : 2; }

        // Snaps one RGB triple (in place) through YCoCg-R with the band's table; a triple whose decode would wrap (move a
        // channel by more than 64) is left exact.
        static void SnapTriple(uint8_t* rgb, const uint8_t* table)
        {
            uint8_t y = rgb[0], co = rgb[1], cg = rgb[2];
            ColourTransform::Forward(y, co, cg);
            uint8_t r = table[y], g = table[co], b = table[cg];
            ColourTransform::Inverse(r, g, b);
            if (std::abs((int)(int8_t)r - (int)(int8_t)rgb[0]) > 64 || std::abs((int)(int8_t)g - (int)(int8_t)rgb[1]) > 64 ||
                std::abs((int)(int8_t)b - (int)(int8_t)rgb[2]) > 64)
                return;
            rgb[0] = r; rgb[1] = g; rgb[2] = b;
        }

        // ---- Bookkeeping ---------------------------------------------------------------------------------------------

        // A record's SH scale (word 3, bits 16-31, an f16), or 0 when it has no SH (word 3, bit 8)
        static uint16_t RecordSHScale(const uint8_t* record)
        {
            uint32_t w3;
            memcpy(&w3, record + 12, 4);
            return (w3 & 0x100u) ? (uint16_t)(w3 >> 16) : (uint16_t)0;
        }

        // A positive f16 as mantissa * 2^exponent, both integers (mantissa 0 for zero)
        static void HalfParts(uint16_t h, int64_t& mantissa, int& exponent)
        {
            const int e = (h >> 10) & 0x1F;
            mantissa = h & 0x3FF;
            if (e == 0) exponent = -24;
            else { mantissa |= 0x400; exponent = e - 25; }
        }

        // The records' 16-bit positions (word 0: x, y; word 1: z)
        static void RecordPosition(const uint8_t* record, uint32_t p[3])
        {
            uint32_t w0, w1;
            memcpy(&w0, record, 4); memcpy(&w1, record + 4, 4);
            p[0] = w0 & 0xFFFFu; p[1] = w0 >> 16; p[2] = w1 & 0xFFFFu;
        }

        // For every record of a level, the index of the nearest record of the next coarser level (squared distance between
        // 16-bit positions, ties to the lowest index): exact, so a reader finds the same parent as the writer. Through a
        // grid over the coarser level's own extent, about two of its records per cell.
        static void ParentMap(const uint8_t* child, size_t childCount, const uint8_t* parent, size_t parentCount, size_t recordBytes,
                              std::vector<uint32_t>& out)
        {
            out.assign(childCount, 0);
            if (parentCount == 0) return;
            uint32_t lo[3] = { 0xFFFFu, 0xFFFFu, 0xFFFFu }, hi[3] = { 0, 0, 0 };
            std::vector<uint32_t> pos(parentCount * 3);
            for (size_t j = 0; j < parentCount; j++)
            {
                RecordPosition(parent + j * recordBytes, &pos[j * 3]);
                for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], pos[j * 3 + a]); hi[a] = std::max(hi[a], pos[j * 3 + a]); }
            }
            const uint32_t extent = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }) + 1;
            const uint32_t across = std::max(1u, std::min(256u, (uint32_t)std::cbrt((double)parentCount / 2.0)));
            const uint32_t cell = std::max(1u, (extent + across - 1) / across);
            int dim[3];
            for (int a = 0; a < 3; a++) dim[a] = (int)((hi[a] - lo[a]) / cell) + 1;
            auto cellOfPos = [&](const uint32_t* p, int c[3])
            {
                for (int a = 0; a < 3; a++)
                {
                    const int64_t v = ((int64_t)p[a] - (int64_t)lo[a]) / (int64_t)cell;
                    c[a] = (int)std::max<int64_t>(0, std::min<int64_t>(dim[a] - 1, p[a] < lo[a] ? -1 : v));
                }
            };
            const size_t cells = (size_t)dim[0] * dim[1] * dim[2];
            std::vector<uint32_t> start(cells + 1, 0), order(parentCount), cellOf(parentCount);
            for (size_t j = 0; j < parentCount; j++)
            {
                int c[3];
                cellOfPos(&pos[j * 3], c);
                cellOf[j] = (uint32_t)(((size_t)c[2] * dim[1] + c[1]) * dim[0] + c[0]);
                start[cellOf[j] + 1]++;
            }
            for (size_t c = 1; c < start.size(); c++) start[c] += start[c - 1];
            std::vector<uint32_t> fill(start.begin(), start.end() - 1);
            for (size_t j = 0; j < parentCount; j++) order[fill[cellOf[j]]++] = (uint32_t)j; // Ascending index within a cell
            const int maxRing = std::max({ dim[0], dim[1], dim[2] });
            for (size_t i = 0; i < childCount; i++)
            {
                uint32_t p[3];
                RecordPosition(child + i * recordBytes, p);
                int c[3];
                cellOfPos(p, c);
                uint64_t best = UINT64_MAX;
                uint32_t bestIndex = 0;
                for (int r = 0; r <= maxRing; r++)
                {
                    // Every record outside the rings searched so far is at least r - 1 cells away on some axis (also from a
                    // point outside the grid, which is farther still)
                    if (r > 0 && best != UINT64_MAX)
                    {
                        const uint64_t reach = (uint64_t)(r - 1) * cell;
                        if (reach * reach > best) break;
                    }
                    for (int z = std::max(0, c[2] - r); z <= std::min(dim[2] - 1, c[2] + r); z++)
                        for (int y = std::max(0, c[1] - r); y <= std::min(dim[1] - 1, c[1] + r); y++)
                            for (int x = std::max(0, c[0] - r); x <= std::min(dim[0] - 1, c[0] + r); x++)
                            {
                                if (std::max({ std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2]) }) != r) continue;
                                const size_t cl = ((size_t)z * dim[1] + y) * dim[0] + x;
                                for (uint32_t k = start[cl]; k < start[cl + 1]; k++)
                                {
                                    const uint32_t j = order[k];
                                    const int64_t dx = (int64_t)pos[j * 3] - p[0], dy = (int64_t)pos[j * 3 + 1] - p[1], dz = (int64_t)pos[j * 3 + 2] - p[2];
                                    const uint64_t dd = (uint64_t)(dx * dx + dy * dy + dz * dz);
                                    if (dd < best || (dd == best && j < bestIndex)) { best = dd; bestIndex = j; }
                                }
                            }
                }
                out[i] = bestIndex;
            }
        }

        // The ratio of two f16 scales as a fixed-point factor with 24 fraction bits, rounded half up and held to 2^31 (at
        // and beyond 128 every nonzero byte clamps anyway)
        static int64_t ScaleFactor(uint16_t parentScale, uint16_t childScale)
        {
            int64_t mp, mc; int ep, ec;
            HalfParts(parentScale, mp, ep);
            HalfParts(childScale, mc, ec);
            if (mp == 0 || mc == 0) return 0;
            const int shift = ep - ec + 24; // factor = mp / mc * 2^shift
            constexpr int64_t kMax = (int64_t)1 << 31;
            if (shift > 40) return kMax;
            if (shift < -40) return 0;
            int64_t f;
            if (shift >= 0) { const int64_t num = mp << shift; f = (2 * num + mc) / (2 * mc); }
            else { const int64_t den = mc << -shift; f = (2 * mp + den) / (2 * den); }
            return std::min(f, kMax);
        }

        // The prediction of a point's SH bytes: the parent's SH (bytes times its scale) at the point's own scale, rounded
        // half away from zero, clamped and snapped like the stream (bandTables[3] from SnapTable). Scales are the records'
        // f16 bits (RecordSHScale); 0 = none.
        static void Predict(const uint8_t* parentSH, uint16_t parentScale, uint16_t childScale, size_t shBytes, const uint8_t bandTables[3][256],
                            bool snap, uint8_t* out)
        {
            const int64_t f = ScaleFactor(parentScale, childScale);
            for (size_t k = 0; k < shBytes; k++)
            {
                const int s = (int8_t)parentSH[k];
                const int64_t q = ((int64_t)std::abs(s) * f + ((int64_t)1 << 23)) >> 24;
                out[k] = (uint8_t)(int8_t)((s < 0 ? -1 : 1) * (int)std::min<int64_t>(127, q));
            }
            if (snap)
                for (size_t k = 0; k + 2 < shBytes; k += 3) SnapTriple(out + k, bandTables[Band((uint32_t)(k / 3))]);
        }

        // ---- Residuals over one chunk's levels (finest first) ------------------------------------------------------

        // Writer: sh[l] holds level l's SH stream (interleaved, absolute, already snapped). The coarsest level is left
        // absolute; every finer level becomes YCoCg(point) - YCoCg(prediction), byte by byte modulo 256. The caller then
        // applies the colour transform to the coarsest level only.
        // Every level's parent map (ParentMap of level l into level l + 1), for a caller that codes one set of records with
        // several SH choices (the RDO analysis): the maps depend only on the records' positions
        static void ParentMaps(const std::vector<const uint8_t*>& records, const std::vector<size_t>& counts, size_t recordBytes,
                               std::vector<std::vector<uint32_t>>& out)
        {
            out.assign(records.size() > 0 ? records.size() - 1 : 0, {});
            for (size_t l = 0; l + 1 < records.size(); l++)
                ParentMap(records[l], counts[l], records[l + 1], counts[l + 1], recordBytes, out[l]);
        }

        static void EncodeResiduals(const std::vector<const uint8_t*>& records, const std::vector<size_t>& counts, size_t recordBytes,
                                    std::vector<std::vector<uint8_t>*>& sh, size_t shBytes, const uint8_t bits[3],
                                    const std::vector<std::vector<uint32_t>>* parentMaps = nullptr)
        {
            if (sh.size() < 2 || shBytes == 0) return;
            uint8_t tables[3][256];
            for (int b = 0; b < 3; b++) SnapTable(bits[b], tables[b]);
            const bool snap = bits[0] < 8 || bits[1] < 8 || bits[2] < 8;
            // From the finest level up, so every prediction reads its parent's absolute bytes
            for (size_t l = 0; l + 1 < sh.size(); l++)
            {
                std::vector<uint32_t> built;
                if (!parentMaps || l >= parentMaps->size()) ParentMap(records[l], counts[l], records[l + 1], counts[l + 1], recordBytes, built);
                const std::vector<uint32_t>& parent = (parentMaps && l < parentMaps->size()) ? (*parentMaps)[l] : built;
                std::vector<uint8_t>& level = *sh[l];
                const std::vector<uint8_t>& coarser = *sh[l + 1];
                std::vector<uint8_t> prediction(shBytes);
                for (size_t i = 0; i < counts[l]; i++)
                {
                    const uint32_t j = parent[i];
                    if (counts[l + 1] == 0) std::fill(prediction.begin(), prediction.end(), (uint8_t)0); // No parent: the residual is the YCoCg value
                    else Predict(coarser.data() + (size_t)j * shBytes, RecordSHScale(records[l + 1] + (size_t)j * recordBytes),
                                 RecordSHScale(records[l] + i * recordBytes), shBytes, tables, snap, prediction.data());
                    uint8_t* point = level.data() + i * shBytes;
                    for (size_t k = 0; k + 2 < shBytes; k += 3)
                    {
                        uint8_t a0 = point[k], a1 = point[k + 1], a2 = point[k + 2];
                        uint8_t p0 = prediction[k], p1 = prediction[k + 1], p2 = prediction[k + 2];
                        ColourTransform::Forward(a0, a1, a2);
                        ColourTransform::Forward(p0, p1, p2);
                        point[k] = (uint8_t)(a0 - p0); point[k + 1] = (uint8_t)(a1 - p1); point[k + 2] = (uint8_t)(a2 - p2);
                    }
                }
            }
        }

        // Reader: undoes EncodeResiduals after the coarsest level's colour transform has been undone. From the coarsest
        // level down, so every prediction reads its parent's restored bytes.
        static void DecodeResiduals(const std::vector<const uint8_t*>& records, const std::vector<size_t>& counts, size_t recordBytes,
                                    std::vector<std::vector<uint8_t>*>& sh, size_t shBytes, const uint8_t bits[3])
        {
            if (sh.size() < 2 || shBytes == 0) return;
            uint8_t tables[3][256];
            for (int b = 0; b < 3; b++) SnapTable(bits[b], tables[b]);
            const bool snap = bits[0] < 8 || bits[1] < 8 || bits[2] < 8;
            for (size_t l = sh.size() - 1; l-- > 0;)
            {
                std::vector<uint32_t> parent;
                ParentMap(records[l], counts[l], records[l + 1], counts[l + 1], recordBytes, parent);
                std::vector<uint8_t>& level = *sh[l];
                const std::vector<uint8_t>& coarser = *sh[l + 1];
                std::vector<uint8_t> prediction(shBytes);
                for (size_t i = 0; i < counts[l]; i++)
                {
                    const uint32_t j = parent[i];
                    if (counts[l + 1] == 0) std::fill(prediction.begin(), prediction.end(), (uint8_t)0); // No parent: the residual is the YCoCg value
                    else Predict(coarser.data() + (size_t)j * shBytes, RecordSHScale(records[l + 1] + (size_t)j * recordBytes),
                                 RecordSHScale(records[l] + i * recordBytes), shBytes, tables, snap, prediction.data());
                    uint8_t* point = level.data() + i * shBytes;
                    for (size_t k = 0; k + 2 < shBytes; k += 3)
                    {
                        uint8_t p0 = prediction[k], p1 = prediction[k + 1], p2 = prediction[k + 2];
                        ColourTransform::Forward(p0, p1, p2);
                        uint8_t a0 = (uint8_t)(point[k] + p0), a1 = (uint8_t)(point[k + 1] + p1), a2 = (uint8_t)(point[k + 2] + p2);
                        ColourTransform::Inverse(a0, a1, a2);
                        point[k] = a0; point[k + 1] = a1; point[k + 2] = a2;
                    }
                }
            }
        }
    };

    // SH Precision over a whole stream (count entries of shRecordBytes signed bytes), bits per band 1, 2, 3. Each
    // coefficient's (R, G, B) is taken to YCoCg-R, each of Y, Co and Cg is snapped (SHCoding::SnapTable), and the triple is
    // taken back to RGB; a triple whose decode would move a channel by more than a quarter of the range keeps its exact
    // value. Measured on Cthulhu against the 8-bit stream (colour error over all view directions, per splat): 6 bits -23%
    // package size at 46 dB, 5 bits -33% at 41 dB, 4 bits -44% at 35 dB (docs/private/sh-compression-study.md).
    inline void RequantiseSH(uint8_t* sh, size_t count, uint32_t shRecordBytes, const uint8_t bits[3])
    {
        if (bits[0] >= 8 && bits[1] >= 8 && bits[2] >= 8) return;
        if (shRecordBytes < 3) return;
        uint8_t tables[3][256];
        for (int b = 0; b < 3; b++) SHCoding::SnapTable(bits[b], tables[b]);
        const uint32_t coeffs = shRecordBytes / 3;
        for (size_t i = 0; i < count; i++)
            for (uint32_t k = 0; k < coeffs; k++)
            {
                if (bits[SHCoding::Band(k)] >= 8) continue;
                SHCoding::SnapTriple(sh + i * shRecordBytes + 3 * k, tables[SHCoding::Band(k)]);
            }
    }
}
