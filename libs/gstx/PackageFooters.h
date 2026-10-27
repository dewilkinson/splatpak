// PackageFooters.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Trailing sections of a .gstx (docs/GSTX_FORMAT_SPECIFICATION.md, clause 14): optional text after everything the
// header's offsets reach, each written as its bytes, then their length (uint64, little-endian) and an 8-byte marker.
// Sections chain: a reader starts at the end of the file and steps back over each section it recognises by marker
// until it meets one it does not (or the package itself). Readers that know none of them never look, so adding a
// section is no format version change. Sections today: GSTXRDO1, the RDO profile (RdoProfile.h); GSTXVEW1, the view
// settings (PackageView.h).

#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Splat
{
    namespace PackageFooters
    {
        constexpr size_t kTailBytes = 16;     // uint64 length + 8-byte marker
        constexpr uint64_t kMaxSection = 64ull << 20;

        // Every marker a reader may step over, so a section is found wherever it sits in the chain
        inline const std::vector<std::string>& KnownMarkers()
        {
            static const std::vector<std::string> markers = { "GSTXRDO1", "GSTXVEW1" };
            return markers;
        }

        inline bool Append(const std::string& packagePath, const char marker[8], const std::string& text)
        {
            std::ofstream out(packagePath, std::ios::binary | std::ios::app);
            if (!out || text.empty()) return false;
            const uint64_t size = text.size();
            out.write(text.data(), (std::streamsize)text.size());
            out.write(reinterpret_cast<const char*>(&size), sizeof(size));
            out.write(marker, 8);
            return (bool)out;
        }

        // Writes this marker's section in place of any it already has: the chain is read back, the file cut where it
        // starts, and the other sections written again (oldest first) before this one. The package itself is untouched.
        inline bool Replace(const std::string& packagePath, const char marker[8], const std::string& text)
        {
            std::vector<std::pair<std::string, std::string>> others; // marker, text; newest first
            uint64_t chainStart = 0;
            {
                std::ifstream in(packagePath, std::ios::binary | std::ios::ate);
                if (!in || text.empty()) return false;
                std::streamoff end = in.tellg();
                for (int guard = 0; guard < 16 && end >= (std::streamoff)kTailBytes; guard++)
                {
                    char tail[kTailBytes];
                    in.seekg(end - (std::streamoff)kTailBytes);
                    in.read(tail, kTailBytes);
                    if (!in) return false;
                    bool known = false;
                    for (const std::string& m : KnownMarkers()) known = known || memcmp(tail + 8, m.data(), 8) == 0;
                    if (!known) break;
                    uint64_t size = 0;
                    memcpy(&size, tail, sizeof(size));
                    if (size == 0 || size > kMaxSection || size > (uint64_t)(end - (std::streamoff)kTailBytes)) return false;
                    const std::streamoff start = end - (std::streamoff)kTailBytes - (std::streamoff)size;
                    if (memcmp(tail + 8, marker, 8) != 0)
                    {
                        std::string body((size_t)size, '\0');
                        in.seekg(start);
                        in.read(&body[0], (std::streamsize)size);
                        if (!in) return false;
                        others.emplace_back(std::string(tail + 8, 8), std::move(body));
                    }
                    end = start;
                }
                chainStart = (uint64_t)end;
            }
            std::error_code ec;
            std::filesystem::resize_file(packagePath, chainStart, ec);
            if (ec) return false;
            for (auto it = others.rbegin(); it != others.rend(); ++it)
                if (!Append(packagePath, it->first.data(), it->second)) return false;
            return Append(packagePath, marker, text);
        }

        // The text of the section with this marker, walking back through the known sections from the end of the file
        inline bool Read(const std::string& packagePath, const char marker[8], std::string& text)
        {
            std::ifstream in(packagePath, std::ios::binary | std::ios::ate);
            if (!in) return false;
            std::streamoff end = in.tellg();
            for (int guard = 0; guard < 16 && end >= (std::streamoff)kTailBytes; guard++)
            {
                char tail[kTailBytes];
                in.seekg(end - (std::streamoff)kTailBytes);
                in.read(tail, kTailBytes);
                if (!in) return false;
                bool known = false;
                for (const std::string& m : KnownMarkers()) known = known || memcmp(tail + 8, m.data(), 8) == 0;
                if (!known) return false;
                uint64_t size = 0;
                memcpy(&size, tail, sizeof(size));
                if (size == 0 || size > kMaxSection || size > (uint64_t)(end - (std::streamoff)kTailBytes)) return false;
                const std::streamoff start = end - (std::streamoff)kTailBytes - (std::streamoff)size;
                if (memcmp(tail + 8, marker, 8) == 0)
                {
                    text.resize((size_t)size);
                    in.seekg(start);
                    in.read(&text[0], (std::streamsize)size);
                    return (bool)in;
                }
                end = start;
            }
            return false;
        }

        // The same, over a package already in memory (a host that keeps the file's bytes, such as the UE5 plugin's asset)
        inline bool ReadMemory(const uint8_t* data, size_t size, const char marker[8], std::string& text)
        {
            if (!data) return false;
            size_t end = size;
            for (int guard = 0; guard < 16 && end >= kTailBytes; guard++)
            {
                const uint8_t* tail = data + end - kTailBytes;
                bool known = false;
                for (const std::string& m : KnownMarkers()) known = known || memcmp(tail + 8, m.data(), 8) == 0;
                if (!known) return false;
                uint64_t length = 0;
                memcpy(&length, tail, sizeof(length));
                if (length == 0 || length > kMaxSection || length > (uint64_t)(end - kTailBytes)) return false;
                const size_t start = end - kTailBytes - (size_t)length;
                if (memcmp(tail + 8, marker, 8) == 0)
                {
                    text.assign(reinterpret_cast<const char*>(data + start), (size_t)length);
                    return true;
                }
                end = start;
            }
            return false;
        }
    }
}
