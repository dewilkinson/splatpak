#include <iostream>
#include "../tools/SurfelsPreprocess/StreamPackager.h"

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
