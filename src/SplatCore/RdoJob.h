// RdoJob.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// An RDO job and its part (docs/design/distributed-rdo.md): what a dispatcher hands a worker (SplatPakWorker.exe, on this
// machine, BLUESHELL or a partner's through the fleet relay) and what the worker hands back. A job names one scan (its file and
// content hash), the pyramid to rebuild from it (levels, deadband) and a share of the candidates; the part carries those
// candidates' measurements and their per-region tables, so the dispatcher can merge every part (RdoAnalysis::Merge) and finish
// the curve. JSON with one value per line, read by a line scanner, like RdoProfile.

#pragma once
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "RdoAnalysis.h"

namespace Splat
{
    namespace RdoJobIO
    {
        inline double Num(const std::string& line, const char* key, double fallback)
        {
            const std::string k = std::string("\"") + key + "\":";
            const size_t at = line.find(k);
            return at == std::string::npos ? fallback : atof(line.c_str() + at + k.size());
        }
        inline std::string Str(const std::string& line, const char* key)
        {
            const std::string k = std::string("\"") + key + "\": \"";
            const size_t at = line.find(k);
            if (at == std::string::npos) return std::string();
            const size_t end = line.find('"', at + k.size());
            return end == std::string::npos ? std::string() : line.substr(at + k.size(), end - at - k.size());
        }
        inline std::string Bits(const RdoSettings& s)
        {
            char b[16]; snprintf(b, sizeof(b), "%u,%u,%u", s.shBits[0], s.shBits[1], s.shBits[2]); return b;
        }
        inline void ParseBits(const std::string& text, RdoSettings& s)
        {
            unsigned a = 8, b = 8, c = 8;
            if (sscanf(text.c_str(), "%u,%u,%u", &a, &b, &c) == 3) { s.shBits[0] = (uint8_t)a; s.shBits[1] = (uint8_t)b; s.shBits[2] = (uint8_t)c; }
        }
        inline std::string Slashes(std::string p) { for (char& c : p) if (c == '\\') c = '/'; return p; }
    }

    struct RdoJob
    {
        static constexpr const char* kFormat = "splatpak-rdo-job-1";
        std::string scan;          // The .ply (a path the worker can read: a runner fetches the file first and rewrites this)
        uint64_t    scanHash = 0;  // Its content hash (BakeTuningCache::HashFile); the worker refuses a file that differs
        int         levels = 5;
        float       deadbandMM = 1.0f;
        std::vector<RdoSettings> candidates;

        std::string Text() const
        {
            using namespace RdoJobIO;
            std::ostringstream o;
            char line[256];
            o << "{\n  \"format\": \"" << kFormat << "\",\n";
            o << "  \"scan\": \"" << Slashes(scan) << "\",\n";
            snprintf(line, sizeof(line), "  \"scan_hash\": \"%016llx\",\n  \"levels\": %d,\n  \"deadband_mm\": %.4f,\n  \"candidates\": [\n",
                (unsigned long long)scanHash, levels, deadbandMM);
            o << line;
            for (size_t i = 0; i < candidates.size(); i++)
            {
                snprintf(line, sizeof(line), "    { \"candidate_degree\": %u, \"bits\": \"%s\" }%s\n", candidates[i].shDegree, Bits(candidates[i]).c_str(),
                    i + 1 < candidates.size() ? "," : "");
                o << line;
            }
            o << "  ]\n}\n";
            return o.str();
        }

        bool Parse(std::istream& in, std::string& error)
        {
            using namespace RdoJobIO;
            *this = RdoJob{};
            bool formatSeen = false;
            std::string line;
            while (std::getline(in, line))
            {
                if (line.find("\"format\":") != std::string::npos) formatSeen = (Str(line, "format") == kFormat);
                else if (line.find("\"scan\":") != std::string::npos) scan = Str(line, "scan");
                else if (line.find("\"scan_hash\":") != std::string::npos) scanHash = strtoull(Str(line, "scan_hash").c_str(), nullptr, 16);
                else if (line.find("\"levels\":") != std::string::npos) levels = (int)Num(line, "levels", 5.0);
                else if (line.find("\"deadband_mm\":") != std::string::npos) deadbandMM = (float)Num(line, "deadband_mm", 1.0);
                else if (line.find("\"candidate_degree\":") != std::string::npos)
                {
                    RdoSettings s;
                    s.shDegree = (uint32_t)Num(line, "candidate_degree", 3.0);
                    ParseBits(Str(line, "bits"), s);
                    candidates.push_back(s);
                }
            }
            if (!formatSeen) { error = "not an RDO job of this format"; return false; }
            if (scan.empty()) { error = "the job names no scan"; return false; }
            if (candidates.empty()) { error = "the job names no candidates"; return false; }
            if (levels < 1 || levels > 8) { error = "levels out of range"; return false; }
            return true;
        }
    };

    // A worker's answer: the job's candidates measured (an unfinished RdoAnalysisResult), and who measured them
    struct RdoPart
    {
        static constexpr const char* kFormat = "splatpak-rdo-part-1";
        uint64_t scanHash = 0;
        std::string build;          // The worker's build identity (the hash of its executable)
        std::string error;          // Set instead of results when the job failed
        RdoAnalysisResult result;

        std::string Text() const
        {
            using namespace RdoJobIO;
            std::ostringstream o;
            char line[512];
            o << "{\n  \"format\": \"" << kFormat << "\",\n";
            snprintf(line, sizeof(line), "  \"scan_hash\": \"%016llx\",\n", (unsigned long long)scanHash); o << line;
            o << "  \"build\": \"" << build << "\",\n";
            if (!error.empty()) { o << "  \"error\": \"" << Slashes(error) << "\"\n}\n"; return o.str(); }
            snprintf(line, sizeof(line), "  \"seconds\": %.2f,\n  \"units\": %zu,\n  \"record_bytes\": %.0f,\n  \"record_coarse_bytes\": %.0f,\n  \"level0_record_bytes\": %.0f,\n",
                result.seconds, result.units, result.recordBytes, result.recordCoarseBytes, result.level0RecordBytes);
            o << line;
            snprintf(line, sizeof(line), "  \"sphere\": \"%.6g,%.6g,%.6g,%.6g\",\n", result.center[0], result.center[1], result.center[2], result.radius);
            o << line;
            o << "  \"unit_centers\": \"";
            for (size_t i = 0; i < result.unitCenter.size(); i++) { snprintf(line, sizeof(line), "%s%.6g", i ? "," : "", result.unitCenter[i]); o << line; }
            o << "\",\n  \"unit_record_bytes\": \"";
            for (size_t i = 0; i < result.unitRecordBytes.size(); i++) { snprintf(line, sizeof(line), "%s%.0f", i ? "," : "", result.unitRecordBytes[i]); o << line; }
            o << "\",\n  \"candidates\": [\n";
            const size_t n = result.points.size();
            for (size_t c = 0; c < n; c++)
            {
                const RdoPoint& p = result.points[c];
                snprintf(line, sizeof(line), "    { \"candidate_degree\": %u, \"bits\": \"%s\", \"bytes\": %.0f, \"coarse_bytes\": %.0f, \"mse\": %.9g, \"psnr\": %.4f,",
                    p.settings.shDegree, Bits(p.settings).c_str(), p.bytes, p.coarseBytes, p.mse, p.psnr);
                o << line;
                // The candidate's per-region tables: size, then error, of every unit
                o << " \"unit_bytes\": \"";
                for (size_t u = 0; u < result.units; u++) { snprintf(line, sizeof(line), "%s%.0f", u ? "," : "", result.unitBytes.size() == result.units * n ? result.unitBytes[u * n + c] : 0.0); o << line; }
                o << "\", \"unit_error\": \"";
                for (size_t u = 0; u < result.units; u++) { snprintf(line, sizeof(line), "%s%.6g", u ? "," : "", result.unitError.size() == result.units * n ? result.unitError[u * n + c] : 0.0); o << line; }
                o << "\" }" << (c + 1 < n ? "," : "") << "\n";
            }
            o << "  ]\n}\n";
            return o.str();
        }

        bool Parse(std::istream& in, std::string& parseError)
        {
            using namespace RdoJobIO;
            *this = RdoPart{};
            bool formatSeen = false;
            std::string line;
            std::vector<std::vector<double>> ub, ue;
            while (std::getline(in, line))
            {
                if (line.find("\"format\":") != std::string::npos) formatSeen = (Str(line, "format") == kFormat);
                else if (line.find("\"scan_hash\":") != std::string::npos) scanHash = strtoull(Str(line, "scan_hash").c_str(), nullptr, 16);
                else if (line.find("\"build\":") != std::string::npos) build = Str(line, "build");
                else if (line.find("\"error\":") != std::string::npos) error = Str(line, "error");
                else if (line.find("\"seconds\":") != std::string::npos) result.seconds = Num(line, "seconds", 0.0);
                else if (line.find("\"units\":") != std::string::npos) result.units = (size_t)Num(line, "units", 0.0);
                else if (line.find("\"record_bytes\":") != std::string::npos) result.recordBytes = Num(line, "record_bytes", 0.0);
                else if (line.find("\"record_coarse_bytes\":") != std::string::npos) result.recordCoarseBytes = Num(line, "record_coarse_bytes", 0.0);
                else if (line.find("\"level0_record_bytes\":") != std::string::npos) result.level0RecordBytes = Num(line, "level0_record_bytes", 0.0);
                else if (line.find("\"sphere\":") != std::string::npos)
                    sscanf(Str(line, "sphere").c_str(), "%f,%f,%f,%f", &result.center[0], &result.center[1], &result.center[2], &result.radius);
                else if (line.find("\"unit_centers\":") != std::string::npos)
                {
                    std::stringstream s(Str(line, "unit_centers")); std::string item;
                    while (std::getline(s, item, ',')) result.unitCenter.push_back((float)atof(item.c_str()));
                }
                else if (line.find("\"unit_record_bytes\":") != std::string::npos)
                {
                    std::stringstream s(Str(line, "unit_record_bytes")); std::string item;
                    while (std::getline(s, item, ',')) result.unitRecordBytes.push_back(atof(item.c_str()));
                }
                else if (line.find("\"candidate_degree\":") != std::string::npos)
                {
                    RdoPoint p;
                    p.settings.shDegree = (uint32_t)Num(line, "candidate_degree", 3.0);
                    ParseBits(Str(line, "bits"), p.settings);
                    p.bytes = Num(line, "bytes", 0.0); p.coarseBytes = Num(line, "coarse_bytes", 0.0); p.mse = Num(line, "mse", 0.0); p.psnr = Num(line, "psnr", 0.0);
                    result.points.push_back(p);
                    auto list = [](const std::string& text) { std::vector<double> v; std::stringstream s(text); std::string item; while (std::getline(s, item, ',')) v.push_back(atof(item.c_str())); return v; };
                    ub.push_back(list(Str(line, "unit_bytes")));
                    ue.push_back(list(Str(line, "unit_error")));
                }
            }
            if (!formatSeen) { parseError = "not an RDO part of this format"; return false; }
            const size_t n = result.points.size();
            result.unitBytes.assign(result.units * n, 0.0);
            result.unitError.assign(result.units * n, 0.0);
            for (size_t c = 0; c < n; c++)
                for (size_t u = 0; u < result.units; u++)
                {
                    if (u < ub[c].size()) result.unitBytes[u * n + c] = ub[c][u];
                    if (u < ue[c].size()) result.unitError[u * n + c] = ue[c][u];
                }
            return true;
        }
    };
}
