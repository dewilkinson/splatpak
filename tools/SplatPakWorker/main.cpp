// SplatPakWorker -- SplatPak's compute worker (docs/design/distributed-rdo.md)
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Headless and windowless: runs one RDO job (RdoJob.h) and writes its part. The same program serves every place work runs --
// this machine, BLUESHELL, a partner's machine through the fleet relay -- so every one measures alike: the pyramid is rebuilt
// from the .ply exactly as SplatPak's bake builds it (the whole model decomposed, coarse levels merged), and the analysis is
// RdoAnalysis's.
//
//   SplatPakWorker --job <job.json | -> [--out <part.json>]    the part on stdout without --out; progress on stderr
//   SplatPakWorker --version                                   the build identity (the hash of this executable)
//
// Progress lines on stderr: "progress <0..1> <stage>". Exit code 0 with a part written; 1 with a part carrying the error.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>
#include "../../src/SplatCore/PLYLoader.h"
#include "../../src/SplatCore/RdoJob.h"
#include "../../src/SplatCore/BakeTuningCache.h"
#include "../../libs/gstx/LiftingWavelet.h"

using namespace Splat;

static std::string BuildIdentity()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)BakeTuningCache::HashFile(path));
    return hex;
}

static uint32_t SourceDegree(const SplatAttributeArray& attrs)
{
    uint32_t degree = 0;
    for (const auto& a : attrs)
        for (int k = 44; k >= 0 && degree < 3; k--)
            if (a.sh[k] != 0.0f) { degree = std::max(degree, k >= 24 ? 3u : k >= 9 ? 2u : 1u); break; }
    return degree;
}

static int Finish(RdoPart& part, const std::string& outPath)
{
    const std::string text = part.Text();
    if (outPath.empty()) fwrite(text.data(), 1, text.size(), stdout);
    else { std::ofstream o(outPath, std::ios::binary); o << text; }
    return part.error.empty() ? 0 : 1;
}

int main(int argc, char** argv)
{
    std::string jobPath, outPath;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "--version") { printf("%s\n", BuildIdentity().c_str()); return 0; }
        if (a == "--job" && i + 1 < argc) jobPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
    }
    if (jobPath.empty()) { fprintf(stderr, "usage: SplatPakWorker --job <job.json | -> [--out <part.json>] | --version\n"); return 2; }

    RdoPart part;
    part.build = BuildIdentity();
    RdoJob job;
    std::string error;
    bool parsed;
    if (jobPath == "-") parsed = job.Parse(std::cin, error);
    else { std::ifstream in(jobPath); parsed = in && job.Parse(in, error); if (!in) error = "cannot read " + jobPath; }
    if (!parsed) { part.error = "bad job: " + error; return Finish(part, outPath); }
    part.scanHash = job.scanHash;

    auto progress = [](float f, const char* stage) { fprintf(stderr, "progress %.3f %s\n", f, stage); fflush(stderr); return true; };
    progress(0.0f, "Checking the scan");
    if (job.scanHash != 0 && BakeTuningCache::HashFile(job.scan) != job.scanHash)
    {
        part.error = "the scan is missing or differs from the job's (hash mismatch): " + job.scan;
        return Finish(part, outPath);
    }

    progress(0.0f, "Reading the scan");
    SplatPointArray points;
    SplatAttributeArray attrs;
    double origin[3] = {};
    if (!PLYLoader::LoadPLY(job.scan, points, origin, &attrs) || attrs.empty())
    {
        part.error = "cannot read " + job.scan + " as a Gaussian splat scan";
        return Finish(part, outPath);
    }
    const uint32_t sourceDegree = SourceDegree(attrs);

    // The pyramid as the bake builds it (SplatPakApp::RunBake): the whole model decomposed, the coarse levels merged
    progress(0.0f, "Building the levels");
    WaveletDecompositionResult wavelet = LiftingWavelet::DecomposeChunk(points, (uint32_t)job.levels, job.deadbandMM / 1000.0f);
    SplatCodec::MergeLevels(wavelet, attrs);
    SplatEncodeParams params;
    XMFLOAT3 mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
    for (const auto& p : points)
    {
        mn.x = std::min(mn.x, p.position.x); mn.y = std::min(mn.y, p.position.y); mn.z = std::min(mn.z, p.position.z);
        mx.x = std::max(mx.x, p.position.x); mx.y = std::max(mx.y, p.position.y); mx.z = std::max(mx.z, p.position.z);
    }
    params.aabbMin = mn; params.aabbMax = mx;
    SplatCodec::ComputeScaleRange(attrs, (uint32_t)job.levels, params.scaleLog2Min, params.scaleLog2Max);
    std::vector<const SplatPointArray*> levels;
    levels.push_back(&points);
    for (size_t l = 1; l < wavelet.lodLevels.size(); l++) levels.push_back(&wavelet.lodLevels[l].splats);

    if (!RdoAnalysis::Analyse(levels, attrs, &wavelet.mergedAttributes, params, sourceDegree, progress, part.result, &job.candidates))
        part.error = "the analysis failed (no candidate within the scan's SH degree " + std::to_string(sourceDegree) + "?)";
    return Finish(part, outPath);
}
