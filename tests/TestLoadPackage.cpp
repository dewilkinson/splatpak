// TestLoadPackage.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Smoke test: loads a .gstx package (the path given, or assets/venus/venus.gstx) and prints its format
// version, payload codec and chunk/splat counts, to sanity check StreamPackager::LoadPackage
// without needing the full viewer or preprocessor app.

#include <iostream>
#include "../src/SplatCore/StreamPackager.h"

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : "assets/venus/venus.gstx";
    Splat::StreamPackager::SFLWPackageData pkg;
    std::string reason;
    bool ok = Splat::StreamPackager::LoadPackage(path, pkg, &reason);
    if (!ok) std::cout << reason << std::endl;
    std::cout << "LoadPackage ok=" << ok << " version=" << pkg.header.version << " codec=" << Splat::PayloadCodec::Name(pkg.header.payloadCodec)
              << " chunks=" << pkg.chunkManifests.size() << " splats=" << pkg.totalSplats << " bytes=" << pkg.totalCompressedBytes << std::endl;
    for (size_t i = 0; i < pkg.chunkManifests.size(); i++)
    {
        std::cout << "Chunk " << i << ": id=" << pkg.chunkManifests[i].chunkId << " LODs=" << pkg.chunkManifests[i].lods.size() << " splats=" << pkg.chunkLOD0Splats[i].size() << std::endl;
    }
    return ok ? 0 : 1;
}
