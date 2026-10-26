// BakeTuningCache.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The bake settings chosen for each scan, remembered against a hash of the scan file's contents
// (bake_tunings.json, beside config.json). A bake the user starts records its settings; opening the same
// unchanged file again bakes with them instead of the defaults from the scan's size. Any change to the file
// changes its hash, so a stale choice is never applied to a different scan.

#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace Splat
{
    struct BakeTuning
    {
        float chunkSize = 0.0f;
        int   maxLODs = 0;
        int   shQuality = 2;   // SH degree kept, minus one
        int   shPrecision = 0; // Index into SplatPakApp::kSHPrecisionBits
        int   shBits[3] = { 0, 0, 0 }; // Bits per band (an RDO choice); 0 = shPrecision's bits for every band
        float deadbandMM = 0.0f;
        bool  occlusion = true;
        std::string file;      // The file it was last baked from (for the reader of the JSON; the hash is the key)
    };

    namespace BakeTuningCache
    {
        constexpr const char* kFile = "bake_tunings.json";

        // 64-bit hash of a file's whole contents (FNV-1a over 64-bit words, the tail byte by byte, the length mixed in);
        // 0 when the file cannot be read. About a second for an 800 MB scan.
        inline uint64_t HashFile(const std::string& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) return 0;
            uint64_t h = 1469598103934665603ull, length = 0;
            std::vector<char> buf(1 << 20);
            while (in)
            {
                in.read(buf.data(), (std::streamsize)buf.size());
                const size_t n = (size_t)in.gcount();
                length += n;
                size_t i = 0;
                for (; i + 8 <= n; i += 8)
                {
                    uint64_t w;
                    memcpy(&w, buf.data() + i, 8);
                    h = (h ^ w) * 1099511628211ull;
                }
                for (; i < n; i++) h = (h ^ (uint8_t)buf[i]) * 1099511628211ull;
            }
            h = (h ^ length) * 1099511628211ull;
            return h == 0 ? 1 : h;
        }

        inline std::map<uint64_t, BakeTuning> Load()
        {
            std::map<uint64_t, BakeTuning> out;
            std::ifstream in(kFile);
            std::string line;
            while (std::getline(in, line))
            {
                // One entry per line: "<hash>": { "chunk": .., "levels": .., "sh_quality": .., "sh_precision": .., "deadband_mm": .., "occlusion": .., "file": ".." }
                const size_t q0 = line.find('"'), q1 = (q0 == std::string::npos) ? q0 : line.find('"', q0 + 1);
                if (q1 == std::string::npos || line.find('{') == std::string::npos) continue;
                const uint64_t hash = strtoull(line.substr(q0 + 1, q1 - q0 - 1).c_str(), nullptr, 16);
                if (hash == 0) continue;
                auto num = [&](const char* key) -> double
                {
                    const size_t k = line.find(std::string("\"") + key + "\":");
                    return k == std::string::npos ? -1.0 : atof(line.c_str() + k + strlen(key) + 3);
                };
                BakeTuning t;
                t.chunkSize = (float)num("chunk");
                t.maxLODs = (int)num("levels");
                t.shQuality = (int)num("sh_quality");
                t.shPrecision = (int)num("sh_precision");
                t.deadbandMM = (float)num("deadband_mm");
                t.occlusion = num("occlusion") != 0.0;
                const size_t f = line.find("\"file\": \"");
                if (f != std::string::npos) t.file = line.substr(f + 9, line.find('"', f + 9) - f - 9);
                const size_t b = line.find("\"sh_bits\": \"");
                if (b != std::string::npos) sscanf(line.c_str() + b + 12, "%d,%d,%d", &t.shBits[0], &t.shBits[1], &t.shBits[2]);
                if (t.chunkSize > 0.0f && t.maxLODs > 0 && t.shQuality >= 0 && t.shPrecision >= 0 && t.deadbandMM >= 0.0f) out[hash] = t;
            }
            return out;
        }

        inline void Store(uint64_t hash, const BakeTuning& t)
        {
            if (hash == 0) return;
            std::map<uint64_t, BakeTuning> all = Load();
            all[hash] = t;
            std::ofstream out(kFile);
            out << "{\n";
            size_t i = 0;
            for (const auto& kv : all)
            {
                std::string file = kv.second.file;
                for (char& c : file) if (c == '\\') c = '/'; // No escapes needed in the JSON
                char line[1024];
                snprintf(line, sizeof(line), "  \"%016llx\": { \"chunk\": %.4f, \"levels\": %d, \"sh_quality\": %d, \"sh_precision\": %d, \"sh_bits\": \"%d,%d,%d\", \"deadband_mm\": %.3f, \"occlusion\": %d, \"file\": \"%s\" }%s\n",
                    (unsigned long long)kv.first, kv.second.chunkSize, kv.second.maxLODs, kv.second.shQuality, kv.second.shPrecision,
                    kv.second.shBits[0], kv.second.shBits[1], kv.second.shBits[2], kv.second.deadbandMM,
                    kv.second.occlusion ? 1 : 0, file.c_str(), ++i < all.size() ? "," : "");
                out << line;
            }
            out << "}\n";
        }
    }
}
