// RdoProfile.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// An RDO profile (.rdoprofile, JSON): everything the Compression (RDO) menu holds for one scan, saved so it can be loaded
// again -- the scan's content hash and file, the bake parameters, the target, the override and the chosen encoding, the
// analysis constants, and every measured candidate (so the curve comes back without analysing again). A profile applies
// only to the scan whose hash it carries: loaded with that scan open, or by opening the .ply it names when that file still
// hashes the same (docs/design/gstx-rdo.md). One value per line, so the reader stays a line scanner.

#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "RdoAnalysis.h"

namespace Splat
{
    struct RdoProfile
    {
        static constexpr const char* kFormat = "splatpak-rdo-profile-1";
        uint64_t sourceHash = 0;
        std::string sourceFile;
        float chunkSize = 0.0f;
        int   maxLODs = 0;
        float deadbandMM = 0.0f;
        bool  occlusion = true;
        RdoAnalysis::Target target;
        bool  override = false;
        RdoSettings choice;
        RdoAnalysisResult analysis;           // Candidates and record sizes (the per-window tables are not kept)
        // The analysis constants it was measured with; a build with others measures differently
        int unitSplats = 0, sampleStride = 0, viewDirections = 0, fastQuality = 0, calibrationWindows = 0;

        bool ConstantsMatch() const
        {
            return unitSplats == (int)RdoAnalysis::kUnitSplats && sampleStride == (int)RdoAnalysis::kSampleStride &&
                   viewDirections == RdoAnalysis::kViewDirections && fastQuality == RdoAnalysis::kFastQuality &&
                   calibrationWindows == RdoAnalysis::kCalibrationWindows;
        }

        bool Write(const std::string& path) const
        {
            std::ofstream out(path);
            if (!out) return false;
            out << Text();
            return (bool)out;
        }

        std::string Text() const
        {
            std::ostringstream out;
            std::string file = sourceFile;
            for (char& c : file) if (c == '\\') c = '/'; // No escapes needed in the JSON
            char line[512];
            auto bits = [](const RdoSettings& s) { char b[16]; snprintf(b, sizeof(b), "%u,%u,%u", s.shBits[0], s.shBits[1], s.shBits[2]); return std::string(b); };
            out << "{\n";
            out << "  \"format\": \"" << kFormat << "\",\n";
            snprintf(line, sizeof(line), "  \"source_hash\": \"%016llx\",\n", (unsigned long long)sourceHash); out << line;
            out << "  \"source_file\": \"" << file << "\",\n";
            snprintf(line, sizeof(line), "  \"bake\": { \"chunk\": %.4f, \"levels\": %d, \"deadband_mm\": %.3f, \"occlusion\": %d },\n", chunkSize, maxLODs, deadbandMM, occlusion ? 1 : 0); out << line;
            snprintf(line, sizeof(line), "  \"target\": { \"mode\": %d, \"size_mb\": %.3f, \"ratio\": %.3f, \"quality_db\": %.2f, \"bandwidth_mbps\": %.3f, \"first_view_s\": %.2f, \"full_detail_s\": %.2f },\n",
                (int)target.mode, target.sizeMB, target.ratio, target.qualityDb, target.bandwidthMBps, target.firstViewSeconds, target.fullDetailSeconds); out << line;
            snprintf(line, sizeof(line), "  \"override\": %d,\n", override ? 1 : 0); out << line;
            snprintf(line, sizeof(line), "  \"choice\": { \"degree\": %u, \"bits\": \"%s\" },\n", choice.shDegree, bits(choice).c_str()); out << line;
            snprintf(line, sizeof(line), "  \"analysis\": { \"unit_splats\": %d, \"sample_stride\": %d, \"view_directions\": %d, \"fast_quality\": %d, \"calibration_windows\": %d, \"record_bytes\": %.0f, \"record_coarse_bytes\": %.0f, \"seconds\": %.1f },\n",
                (int)RdoAnalysis::kUnitSplats, (int)RdoAnalysis::kSampleStride, RdoAnalysis::kViewDirections, RdoAnalysis::kFastQuality, RdoAnalysis::kCalibrationWindows,
                analysis.recordBytes, analysis.recordCoarseBytes, analysis.seconds); out << line;
            out << "  \"candidates\": [\n";
            for (size_t i = 0; i < analysis.points.size(); i++)
            {
                const RdoPoint& p = analysis.points[i];
                snprintf(line, sizeof(line), "    { \"candidate_degree\": %u, \"bits\": \"%s\", \"bytes\": %.0f, \"coarse_bytes\": %.0f, \"mse\": %.9g, \"psnr\": %.4f, \"frontier\": %d, \"region_saving\": %.5f, \"view_bytes\": %.0f, \"view_first_bytes\": %.0f }%s\n",
                    p.settings.shDegree, bits(p.settings).c_str(), p.bytes, p.coarseBytes, p.mse, p.psnr, p.frontier ? 1 : 0, p.regionSaving, p.viewBytes, p.viewFirstBytes,
                    i + 1 < analysis.points.size() ? "," : "");
                out << line;
            }
            out << "  ]\n}\n";
            return out.str();
        }

        // False with a reason for a file that is not a profile of this format
        bool Read(const std::string& path, std::string& error)
        {
            std::ifstream in(path);
            if (!in) { error = "The profile could not be opened."; return false; }
            return Parse(in, error);
        }

        bool Parse(std::istream& in, std::string& error)
        {
            *this = RdoProfile{};
            bool formatSeen = false;
            std::string line;
            auto num = [&](const char* key, double fallback) -> double
            {
                const std::string k = std::string("\"") + key + "\":";
                const size_t at = line.find(k);
                return at == std::string::npos ? fallback : atof(line.c_str() + at + k.size());
            };
            auto str = [&](const char* key) -> std::string
            {
                const std::string k = std::string("\"") + key + "\": \"";
                const size_t at = line.find(k);
                if (at == std::string::npos) return std::string();
                const size_t end = line.find('"', at + k.size());
                return end == std::string::npos ? std::string() : line.substr(at + k.size(), end - at - k.size());
            };
            auto bits = [](const std::string& s, RdoSettings& out)
            {
                unsigned a = 8, b = 8, c = 8;
                if (sscanf(s.c_str(), "%u,%u,%u", &a, &b, &c) == 3) { out.shBits[0] = (uint8_t)a; out.shBits[1] = (uint8_t)b; out.shBits[2] = (uint8_t)c; }
            };
            while (std::getline(in, line))
            {
                if (line.find("\"format\":") != std::string::npos) formatSeen = (str("format") == kFormat);
                else if (line.find("\"source_hash\":") != std::string::npos) sourceHash = strtoull(str("source_hash").c_str(), nullptr, 16);
                else if (line.find("\"source_file\":") != std::string::npos) sourceFile = str("source_file");
                else if (line.find("\"bake\":") != std::string::npos)
                {
                    chunkSize = (float)num("chunk", 0.0); maxLODs = (int)num("levels", 0.0); deadbandMM = (float)num("deadband_mm", 0.0); occlusion = num("occlusion", 1.0) != 0.0;
                }
                else if (line.find("\"target\":") != std::string::npos)
                {
                    target.mode = (RdoAnalysis::TargetMode)(int)num("mode", 0.0);
                    target.sizeMB = num("size_mb", target.sizeMB); target.ratio = num("ratio", target.ratio); target.qualityDb = num("quality_db", target.qualityDb);
                    target.bandwidthMBps = num("bandwidth_mbps", target.bandwidthMBps); target.firstViewSeconds = num("first_view_s", target.firstViewSeconds);
                    target.fullDetailSeconds = num("full_detail_s", target.fullDetailSeconds);
                }
                else if (line.find("\"override\":") != std::string::npos) override = num("override", 0.0) != 0.0;
                else if (line.find("\"choice\":") != std::string::npos) { choice.shDegree = (uint32_t)num("degree", 3.0); bits(str("bits"), choice); }
                else if (line.find("\"analysis\":") != std::string::npos)
                {
                    unitSplats = (int)num("unit_splats", 0.0); sampleStride = (int)num("sample_stride", 0.0); viewDirections = (int)num("view_directions", 0.0);
                    fastQuality = (int)num("fast_quality", 0.0); calibrationWindows = (int)num("calibration_windows", 0.0);
                    analysis.recordBytes = num("record_bytes", 0.0); analysis.recordCoarseBytes = num("record_coarse_bytes", 0.0); analysis.seconds = num("seconds", 0.0);
                }
                else if (line.find("\"candidate_degree\":") != std::string::npos)
                {
                    RdoPoint p;
                    p.settings.shDegree = (uint32_t)num("candidate_degree", 3.0);
                    bits(str("bits"), p.settings);
                    p.bytes = num("bytes", 0.0); p.coarseBytes = num("coarse_bytes", 0.0); p.mse = num("mse", 0.0); p.psnr = num("psnr", 0.0);
                    p.frontier = num("frontier", 0.0) != 0.0; p.regionSaving = num("region_saving", 0.0);
                    p.viewBytes = num("view_bytes", 0.0); p.viewFirstBytes = num("view_first_bytes", 0.0);
                    analysis.points.push_back(p);
                }
            }
            if (!formatSeen) { error = "Not an RDO profile (or one written by a newer SplatPak)."; return false; }
            if (sourceHash == 0) { error = "The profile names no scan."; return false; }
            if (analysis.points.empty()) { error = "The profile holds no measured encodings."; return false; }
            return true;
        }
    };
    // A package's RDO profile, kept in the .gstx itself: the profile text after everything else, then its length (uint64)
    // and the marker "GSTXRDO1" as the file's last 16 bytes. Readers find every section through the header's offsets,
    // so the footer is invisible to them (no format version change); SplatPak reads it back when it opens the package.
    namespace PackageRdoFooter
    {
        constexpr char kMarker[8] = { 'G', 'S', 'T', 'X', 'R', 'D', 'O', '1' };

        inline bool Append(const std::string& packagePath, const std::string& text)
        {
            std::ofstream out(packagePath, std::ios::binary | std::ios::app);
            if (!out || text.empty()) return false;
            const uint64_t size = text.size();
            out.write(text.data(), (std::streamsize)text.size());
            out.write(reinterpret_cast<const char*>(&size), sizeof(size));
            out.write(kMarker, sizeof(kMarker));
            return (bool)out;
        }

        inline bool Read(const std::string& packagePath, std::string& text)
        {
            std::ifstream in(packagePath, std::ios::binary | std::ios::ate);
            if (!in) return false;
            const std::streamoff fileSize = in.tellg();
            if (fileSize < 16) return false;
            char tail[16];
            in.seekg(fileSize - 16);
            in.read(tail, 16);
            if (!in || memcmp(tail + 8, kMarker, sizeof(kMarker)) != 0) return false;
            uint64_t size = 0;
            memcpy(&size, tail, sizeof(size));
            if (size == 0 || size > (uint64_t)(fileSize - 16) || size > (64u << 20)) return false;
            text.resize((size_t)size);
            in.seekg(fileSize - 16 - (std::streamoff)size);
            in.read(&text[0], (std::streamsize)size);
            return (bool)in;
        }
    }
}
