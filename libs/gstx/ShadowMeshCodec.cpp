// ShadowMeshCodec.cpp
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// See ShadowMeshCodec.h.

#include "ShadowMeshCodec.h"
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Splat
{
    namespace
    {
        template <typename T> void Put(std::vector<uint8_t>& out, const T& v)
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + sizeof(T));
        }
        template <typename T> bool Get(const uint8_t* data, size_t size, size_t& at, T& v)
        {
            if (at + sizeof(T) > size) return false;
            memcpy(&v, data + at, sizeof(T));
            at += sizeof(T);
            return true;
        }
        // Byte-plane transpose of count elements of stride bytes, appended to out; and back
        void Transpose(const uint8_t* src, size_t count, size_t stride, std::vector<uint8_t>& out)
        {
            const size_t base = out.size();
            out.resize(base + count * stride);
            for (size_t b = 0; b < stride; b++)
                for (size_t i = 0; i < count; i++) out[base + b * count + i] = src[i * stride + b];
        }
        void Untranspose(const uint8_t* src, size_t count, size_t stride, uint8_t* dst)
        {
            for (size_t b = 0; b < stride; b++)
                for (size_t i = 0; i < count; i++) dst[i * stride + b] = src[b * count + i];
        }
        size_t IndexBytes(uint32_t vertexCount) { return vertexCount <= 65536u ? 2u : 4u; }
    }

    bool ShadowMeshCodec::Encode(const ShadowMesh& mesh, std::vector<uint8_t>& out)
    {
        if (mesh.Empty()) return false;
        ShadowMeshVec3 mn{ 1e30f, 1e30f, 1e30f }, mx{ -1e30f, -1e30f, -1e30f };
        for (const auto& level : mesh.levels)
            for (const auto& p : level.positions)
            {
                mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
                mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
            }
        const float ext[3] = { std::max(1e-9f, mx.x - mn.x), std::max(1e-9f, mx.y - mn.y), std::max(1e-9f, mx.z - mn.z) };

        Put(out, kMagic);
        Put(out, (uint32_t)mesh.levels.size());
        Put(out, mn.x); Put(out, mn.y); Put(out, mn.z);
        Put(out, mx.x); Put(out, mx.y); Put(out, mx.z);
        std::vector<std::vector<uint8_t>> payloads;
        for (const auto& level : mesh.levels)
        {
            const uint32_t nv = (uint32_t)level.positions.size(), nt = (uint32_t)level.TriangleCount();
            std::vector<uint8_t> raw;
            std::vector<uint16_t> q((size_t)nv * 3);
            for (uint32_t i = 0; i < nv; i++)
            {
                const float p[3] = { level.positions[i].x, level.positions[i].y, level.positions[i].z }, m[3] = { mn.x, mn.y, mn.z };
                for (int k = 0; k < 3; k++)
                    q[(size_t)i * 3 + k] = (uint16_t)std::lround(std::clamp((p[k] - m[k]) / ext[k], 0.0f, 1.0f) * 65535.0f);
            }
            Transpose(reinterpret_cast<const uint8_t*>(q.data()), nv, 6, raw);
            std::vector<uint8_t> rgb((size_t)nv * 3);
            for (uint32_t i = 0; i < nv; i++)
            {
                const uint32_t c = i < level.colours.size() ? level.colours[i] : 0xFFFFFFFFu;
                rgb[(size_t)i * 3] = (uint8_t)c; rgb[(size_t)i * 3 + 1] = (uint8_t)(c >> 8); rgb[(size_t)i * 3 + 2] = (uint8_t)(c >> 16);
            }
            Transpose(rgb.data(), nv, 3, raw);
            if (IndexBytes(nv) == 2)
            {
                std::vector<uint16_t> idx(level.indices.begin(), level.indices.end());
                Transpose(reinterpret_cast<const uint8_t*>(idx.data()), idx.size(), 2, raw);
            }
            else
            {
                Transpose(reinterpret_cast<const uint8_t*>(level.indices.data()), level.indices.size(), 4, raw);
            }
            std::vector<uint8_t> packed(BrotliEncoderMaxCompressedSize(raw.size()) + 64);
            size_t packedSize = packed.size();
            if (!BrotliEncoderCompress(11, 24, BROTLI_MODE_GENERIC, raw.size(), raw.data(), &packedSize, packed.data())) return false;
            packed.resize(packedSize);
            Put(out, nv); Put(out, nt); Put(out, (uint32_t)packed.size()); Put(out, (uint32_t)raw.size());
            payloads.push_back(std::move(packed));
        }
        for (const auto& p : payloads) out.insert(out.end(), p.begin(), p.end());
        return true;
    }

    bool ShadowMeshCodec::ReadCounts(const uint8_t* data, size_t size, std::vector<uint32_t>& vertexCounts,
                                     std::vector<uint32_t>& triangleCounts, std::string& error)
    {
        size_t at = 0;
        uint32_t magic = 0, levels = 0;
        float bounds[6];
        if (!Get(data, size, at, magic) || magic != kMagic) { error = "The shadow mesh section does not start with its signature."; return false; }
        if (!Get(data, size, at, levels) || levels == 0 || levels > 8) { error = "The shadow mesh section declares an implausible level count."; return false; }
        for (float& b : bounds) if (!Get(data, size, at, b)) { error = "The shadow mesh section is truncated."; return false; }
        vertexCounts.clear(); triangleCounts.clear();
        uint64_t payloadTotal = 0;
        for (uint32_t l = 0; l < levels; l++)
        {
            uint32_t nv, nt, cb, ub;
            if (!Get(data, size, at, nv) || !Get(data, size, at, nt) || !Get(data, size, at, cb) || !Get(data, size, at, ub))
            {
                error = "The shadow mesh section is truncated.";
                return false;
            }
            if ((uint64_t)ub != (uint64_t)nv * 9 + (uint64_t)nt * 3 * IndexBytes(nv)) { error = "A shadow mesh level's sizes do not agree."; return false; }
            vertexCounts.push_back(nv); triangleCounts.push_back(nt);
            payloadTotal += cb;
        }
        if (at + payloadTotal > size) { error = "The shadow mesh section is truncated."; return false; }
        return true;
    }

    bool ShadowMeshCodec::Decode(const uint8_t* data, size_t size, ShadowMesh& out, std::string& error)
    {
        out.levels.clear();
        std::vector<uint32_t> nvs, nts;
        if (!ReadCounts(data, size, nvs, nts, error)) return false;
        size_t at = 8;
        float mn[3], mx[3];
        for (float& b : mn) Get(data, size, at, b);
        for (float& b : mx) Get(data, size, at, b);
        std::vector<uint32_t> cbs, ubs;
        for (size_t l = 0; l < nvs.size(); l++)
        {
            uint32_t nv, nt, cb, ub;
            Get(data, size, at, nv); Get(data, size, at, nt); Get(data, size, at, cb); Get(data, size, at, ub);
            cbs.push_back(cb); ubs.push_back(ub);
        }
        for (size_t l = 0; l < nvs.size(); l++)
        {
            const uint32_t nv = nvs[l], nt = nts[l];
            std::vector<uint8_t> raw(ubs[l]);
            size_t decoded = raw.size();
            if (BrotliDecoderDecompress(cbs[l], data + at, &decoded, raw.data()) != BROTLI_DECODER_RESULT_SUCCESS || decoded != raw.size())
            {
                error = "A shadow mesh level does not decode.";
                out.levels.clear();
                return false;
            }
            at += cbs[l];
            ShadowMeshLevel level;
            std::vector<uint16_t> q((size_t)nv * 3);
            Untranspose(raw.data(), nv, 6, reinterpret_cast<uint8_t*>(q.data()));
            level.positions.resize(nv);
            for (uint32_t i = 0; i < nv; i++)
                level.positions[i] = ShadowMeshVec3{ mn[0] + (mx[0] - mn[0]) * q[(size_t)i * 3] / 65535.0f,
                                                     mn[1] + (mx[1] - mn[1]) * q[(size_t)i * 3 + 1] / 65535.0f,
                                                     mn[2] + (mx[2] - mn[2]) * q[(size_t)i * 3 + 2] / 65535.0f };
            std::vector<uint8_t> rgb((size_t)nv * 3);
            Untranspose(raw.data() + (size_t)nv * 6, nv, 3, rgb.data());
            level.colours.resize(nv);
            for (uint32_t i = 0; i < nv; i++)
                level.colours[i] = 0xFF000000u | rgb[(size_t)i * 3] | ((uint32_t)rgb[(size_t)i * 3 + 1] << 8) | ((uint32_t)rgb[(size_t)i * 3 + 2] << 16);
            const uint8_t* ip = raw.data() + (size_t)nv * 9;
            level.indices.resize((size_t)nt * 3);
            if (IndexBytes(nv) == 2)
            {
                std::vector<uint16_t> idx(level.indices.size());
                Untranspose(ip, idx.size(), 2, reinterpret_cast<uint8_t*>(idx.data()));
                std::copy(idx.begin(), idx.end(), level.indices.begin());
            }
            else
            {
                Untranspose(ip, level.indices.size(), 4, reinterpret_cast<uint8_t*>(level.indices.data()));
            }
            for (uint32_t i : level.indices)
                if (i >= nv) { error = "A shadow mesh triangle refers to a vertex that does not exist."; out.levels.clear(); return false; }
            out.levels.push_back(std::move(level));
        }
        return true;
    }
}
