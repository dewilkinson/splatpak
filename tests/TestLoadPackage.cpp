// TestLoadPackage.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Smoke test: loads a .sflw package (the path given, or models/venus.sflw) and prints its format
// version, payload codec and chunk/surfel counts, to sanity check StreamPackager::LoadPackage
// without needing the full viewer or preprocessor app.

#include <iostream>
#include "../src/SurfelsCore/StreamPackager.h"

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : "models/venus.sflw";
    Surfels::StreamPackager::SFLWPackageData pkg;
    std::string reason;
    bool ok = Surfels::StreamPackager::LoadPackage(path, pkg, &reason);
    if (!ok) std::cout << reason << std::endl;
    std::cout << "LoadPackage ok=" << ok << " version=" << pkg.header.version << " codec=" << Surfels::PayloadCodec::Name(pkg.header.payloadCodec)
              << " chunks=" << pkg.chunkManifests.size() << " surfels=" << pkg.totalSurfels << " bytes=" << pkg.totalCompressedBytes << std::endl;
    for (size_t i = 0; i < pkg.chunkManifests.size(); i++)
    {
        std::cout << "Chunk " << i << ": id=" << pkg.chunkManifests[i].chunkId << " LODs=" << pkg.chunkManifests[i].lods.size() << " surfels=" << pkg.chunkLOD0Surfels[i].size() << std::endl;
    }
    return ok ? 0 : 1;
}
