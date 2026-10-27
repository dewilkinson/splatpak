// ShadowMesh.cpp (open stand-in)
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The open build of gstx builds no shadow mesh: packages it writes carry no mesh section. It still reads and draws
// the meshes of packages written by the private build (ShadowMeshCodec.h is open code). See README.md.

#include "ShadowMeshBuilder.h"

namespace Splat
{
    bool ShadowMeshBuilder::Build(const SplatPointArray&, const SplatAttributeArray*, const XMFLOAT3&, const XMFLOAT3&,
                                  ShadowMesh& out, std::string& trace, const std::function<bool(float)>&)
    {
        out.levels.clear();
        trace += "ShadowMesh: not built (the open stand-in library has no mesh builder)\n";
        return false;
    }

    bool ShadowMeshBuilder::BuildOccluder(const std::vector<OcclusionVoxelGPU>&, const OcclusionMipTable&, ShadowMesh& out, std::string&)
    {
        out.levels.clear();
        return false;
    }
}
