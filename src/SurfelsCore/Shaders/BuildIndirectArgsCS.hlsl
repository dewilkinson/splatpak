// BuildIndirectArgsCS.hlsl
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Tiny compute shader that builds D3D12_DISPATCH_MESH_ARGUMENTS directly in VRAM from a
// GPU-written visible-surfel counter, so ExecuteIndirect can dispatch the mesh shader
// without a CPU round-trip.

RWByteAddressBuffer g_CounterBuffer : register(u0);      // Offset 0: visibleCount
RWByteAddressBuffer g_IndirectArgsBuffer : register(u1); // Offset 0: ThreadGroupCountX, Y, Z (12 bytes)

[numthreads(1, 1, 1)]
void main()
{
    // Read total visible surfel count from atomic counter
    uint visibleCount = g_CounterBuffer.Load(0);

    // Compute mesh shader threadgroup count (SURFELS_PER_GROUP = 32)
    uint groupCountX = (visibleCount + 31) / 32;

    // Write D3D12_DISPATCH_MESH_ARGUMENTS (uint3)
    g_IndirectArgsBuffer.Store(0, groupCountX);
    g_IndirectArgsBuffer.Store(4, 1);
    g_IndirectArgsBuffer.Store(8, 1);
}

