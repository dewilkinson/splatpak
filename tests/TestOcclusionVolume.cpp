// TestOcclusionVolume.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Console driver for the occlusion volume generator: loads a .sflw package, decodes its LOD0
// surfels, runs OcclusionVolume::BuildGrid + Bake at a few shave settings and prints the trace,
// so the generator can be exercised and tuned without the GUI or a GPU.
//   TestOcclusionVolume <package.sflw> [resolution|0=auto] [shave ...]

#include <iostream>
#include <string>
#include <vector>
#include "../src/SurfelsCore/StreamPackager.h"
#include "../libs/bluesec-codec/OcclusionVolume.h"

int main(int argc, char** argv)
{
    std::string path = (argc > 1) ? argv[1] : "assets/cthulu/cthulu.sflw";
    int resolution = (argc > 2) ? atoi(argv[2]) : 0;
    std::vector<float> shaves;
    for (int i = 3; i < argc; i++) shaves.push_back((float)atof(argv[i]));
    if (shaves.empty()) shaves = { 0.0f, 1.0f, 5.0f, 10.0f };

    Surfels::StreamPackager::SFLWPackageData pkg;
    if (!Surfels::StreamPackager::LoadPackage(path, pkg))
    {
        std::cerr << "Failed to load " << path << std::endl;
        return 1;
    }

    std::cout << "Package detail grid " << pkg.detailGrid.nx << "x" << pkg.detailGrid.ny << "x" << pkg.detailGrid.nz
              << " (" << pkg.detailGrid.occupiedCells << " occupied cells)" << std::endl;
    std::vector<Surfels::PackedSurfelGPU> packed;
    for (const auto& c : pkg.chunkLOD0Surfels) packed.insert(packed.end(), c.begin(), c.end());
    auto points = Surfels::Quantizer::UnquantizeSurfels(packed, pkg.header.globalBoundsMin, pkg.header.globalBoundsMax);
    std::cout << "Loaded " << points.size() << " LOD0 surfels from " << pkg.chunkManifests.size() << " chunks; package volume had "
              << pkg.occlusionVoxels.size() << " cubes in " << pkg.occlusionMips.mipCount << " mips (";
    for (uint32_t k = 0; k < pkg.occlusionMips.mipCount; k++)
        std::cout << (k ? " / " : "") << pkg.occlusionMips.blockCount[k] << " @ " << pkg.occlusionMips.cellSize[k] << " m";
    std::cout << ")" << std::endl;

    Surfels::OcclusionVolume::Grid grid;
    std::string trace;
    Surfels::OcclusionVolume::BuildGrid(points, pkg.header.globalBoundsMin, pkg.header.globalBoundsMax, grid, trace, resolution);
    std::cout << trace;
    if (!grid.valid) { std::cerr << "Grid build failed" << std::endl; return 1; }

    for (float s : shaves)
    {
        std::vector<Surfels::OcclusionVoxelGPU> out;
        Surfels::OcclusionMipTable mips;
        std::string t;
        Surfels::OcclusionVolume::Bake(grid, s, Surfels::OcclusionVolume::ColorGrade{}, out, mips, t);
        std::cout << t;
    }
    return 0;
}
