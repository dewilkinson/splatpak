#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include "../tools/SurfelsPreprocess/PLYLoader.h"
#include "../tools/SurfelsPreprocess/SpatialOctree.h"
#include "../tools/SurfelsPreprocess/StreamPackager.h"

int main(int argc, char** argv)
{
    std::string inputPLY = (argc > 1) ? argv[1] : "datasets/venus.ply";
    std::string outputBase = (argc > 2) ? argv[2] : "datasets/venus";

    std::cout << "Loading " << inputPLY << "..." << std::endl;
    std::vector<Surfels::SurfelVertex> surfels;
    double origin[3] = {0,0,0};
    if (!Surfels::PLYLoader::LoadPLY(inputPLY, surfels, origin))
    {
        std::cerr << "Failed to load " << inputPLY << std::endl;
        return 1;
    }

    std::cout << "Loaded " << surfels.size() << " points. Partitioning into octree chunks..." << std::endl;
    
    DirectX::XMFLOAT3 minP(1e9f, 1e9f, 1e9f), maxP(-1e9f, -1e9f, -1e9f);
    for (const auto& s : surfels)
    {
        minP.x = std::min(minP.x, s.position.x);
        minP.y = std::min(minP.y, s.position.y);
        minP.z = std::min(minP.z, s.position.z);
        maxP.x = std::max(maxP.x, s.position.x);
        maxP.y = std::max(maxP.y, s.position.y);
        maxP.z = std::max(maxP.z, s.position.z);
    }
    float maxDim = std::max(maxP.x - minP.x, std::max(maxP.y - minP.y, maxP.z - minP.z));
    float chunkSize = std::max(0.10f, maxDim / 4.0f);

    auto chunks = Surfels::SpatialOctree::PartitionIntoChunks(surfels, chunkSize);
    std::cout << "Partitioned into " << chunks.size() << " chunks (chunkSize=" << chunkSize << "m). Packaging to " << outputBase << ".sflw..." << std::endl;

    if (!Surfels::StreamPackager::PackageDataset(outputBase, chunks, 4, 0.003f))
    {
        std::cerr << "Failed to package dataset" << std::endl;
        return 1;
    }

    std::cout << "Successfully generated " << outputBase << ".sflw and " << outputBase << ".json!" << std::endl;
    return 0;
}
