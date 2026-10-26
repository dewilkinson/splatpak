// TestLoadPackage.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Smoke test: loads a .gstx package (the path given, or assets/venus/venus.gstx) and prints its format
// version, payload codec and chunk/splat counts, to sanity check StreamPackager::LoadPackage
// without needing the full viewer or preprocessor app. With a second path, also compares the two packages' decoded
// records and SH level by level (the same bake written with different transforms must decode identically).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "../src/SplatCore/StreamPackager.h"
#include "../libs/gstx/SplatCodec.h"

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : "assets/venus/venus.gstx";
    Splat::StreamPackager::SFLWPackageData pkg;
    std::string reason;
    bool ok = Splat::StreamPackager::LoadPackage(path, pkg, &reason);
    if (!ok) std::cout << reason << std::endl;
    std::cout << "LoadPackage ok=" << ok << " version=" << pkg.header.version << " codec=" << Splat::PayloadCodec::Name(pkg.header.payloadCodec)
              << " chunks=" << pkg.chunkManifests.size() << " splats=" << pkg.totalSplats << " bytes=" << pkg.totalCompressedBytes << std::endl;
    std::cout << "Shadow mesh: ";
    if (pkg.shadowMesh.Empty()) std::cout << "none";
    for (size_t l = 0; l < pkg.shadowMesh.levels.size(); l++)
        std::cout << (l ? ", " : "") << "level " << l << " " << pkg.shadowMesh.levels[l].TriangleCount() << " triangles";
    if (!pkg.shadowMesh.Empty()) std::cout << " (section " << pkg.header.shadowMeshBytes << " bytes)";
    std::cout << std::endl;
    for (size_t i = 0; i < pkg.chunkManifests.size(); i++)
    {
        std::cout << "Chunk " << i << ": id=" << pkg.chunkManifests[i].chunkId << " LODs=" << pkg.chunkManifests[i].lods.size() << " splats=" << (pkg.chunkLODSplats[i].empty() ? 0 : pkg.chunkLODSplats[i][0].size()) << std::endl;
    }
    if (ok && getenv("PKG_LEVELSTATS"))
    {
        // Per level: mean opacity, mean DC colour, mean largest and smallest scale (decoded records)
        size_t maxL = 0;
        for (const auto& cm : pkg.chunkManifests) maxL = std::max(maxL, cm.lods.size());
        for (size_t l = 0; l < maxL; l++)
        {
            double n = 0, op = 0, r = 0, g = 0, b = 0, smax = 0, smin = 0;
            for (size_t c = 0; c < pkg.chunkLODSplats.size(); c++)
            {
                if (l >= pkg.chunkLODSplats[c].size()) continue;
                for (const auto& rec : pkg.chunkLODSplats[c][l])
                {
                    DirectX::XMFLOAT3 pos, sc, col; DirectX::XMFLOAT4 rot; float o, shs;
                    Splat::SplatCodec::DecodeRecord(rec, pkg.splatParams, pos, sc, rot, o, col, shs);
                    n++; op += o; r += col.x; g += col.y; b += col.z;
                    smax += std::max({ sc.x, sc.y, sc.z }); smin += std::min({ sc.x, sc.y, sc.z });
                }
            }
            if (n > 0) printf("L%zu: %.0f splats, opacity %.3f, colour %.3f %.3f %.3f, largest scale %.5f, smallest %.5f\n", l, n, op / n, r / n, g / n, b / n, smax / n, smin / n);
        }
    }
    if (ok && argc > 2)
    {
        Splat::StreamPackager::SFLWPackageData other;
        if (!Splat::StreamPackager::LoadPackage(argv[2], other, &reason)) { std::cout << "second package: " << reason << std::endl; return 1; }
        size_t levels = 0, recordDiff = 0, shDiff = 0;
        bool shape = other.chunkLODSplats.size() == pkg.chunkLODSplats.size();
        for (size_t c = 0; shape && c < pkg.chunkLODSplats.size(); c++)
        {
            shape = other.chunkLODSplats[c].size() == pkg.chunkLODSplats[c].size();
            for (size_t l = 0; shape && l < pkg.chunkLODSplats[c].size(); l++)
            {
                levels++;
                const auto& a = pkg.chunkLODSplats[c][l]; const auto& b2 = other.chunkLODSplats[c][l];
                if (a.size() != b2.size() || memcmp(a.data(), b2.data(), a.size() * sizeof(a[0])) != 0) recordDiff++;
                if (pkg.chunkLODSH[c][l] != other.chunkLODSH[c][l]) shDiff++;
            }
        }
        std::cout << "Compare: " << (shape ? "" : "different chunk/level layout; ") << levels << " levels, " << recordDiff << " with different records, "
                  << shDiff << " with different SH" << std::endl;
        ok = shape && recordDiff == 0 && shDiff == 0;
    }
    return ok ? 0 : 1;
}
