// TestLoadPackage.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Smoke test: loads a known .sflw package and prints its chunk/surfel counts, to sanity
// check StreamPackager::LoadPackage without needing the full viewer or preprocessor app.

#include <iostream>
#include "../src/SurfelsCore/StreamPackager.h"

int main()
{
    Surfels::StreamPackager::SFLWPackageData pkg;
    bool ok = Surfels::StreamPackager::LoadPackage("models/venus.sflw", pkg);
    std::cout << "LoadPackage ok=" << ok << " chunks=" << pkg.chunkManifests.size() << " surfels=" << pkg.totalSurfels << " bytes=" << pkg.totalCompressedBytes << std::endl;
    for (size_t i = 0; i < pkg.chunkManifests.size(); i++)
    {
        std::cout << "Chunk " << i << ": id=" << pkg.chunkManifests[i].chunkId << " LODs=" << pkg.chunkManifests[i].lods.size() << " surfels=" << pkg.chunkLOD0Surfels[i].size() << std::endl;
    }
    return ok ? 0 : 1;
}
