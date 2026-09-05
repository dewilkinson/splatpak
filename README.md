# Surfels

> Just want to use the app? See the [User Guide](USER_GUIDE.md) instead — this README covers the technical architecture.

A GPU-driven DirectX 12 **mesh shader** renderer for massive surfel/point-cloud
datasets — wavelet-based multi-resolution LOD streaming and GPU silhouette
refinement, with geometry generated, culled, sorted, and cross-faded entirely
on-GPU. There is no vertex/index buffer and no `DrawInstanced` anywhere in the
pipeline: every splat is procedurally emitted by an amplification/mesh shader
pair each frame, directly from a `StructuredBuffer` of packed surfels.

**Core pieces:**

- **Wavelet LOD hierarchy.** Offline preprocessing (`SurfelsPreprocess`) runs a
  second-generation lifting wavelet decomposition with deadband sparsification
  over Morton/Z-order-partitioned chunks, producing a multi-resolution `.sflw`
  package. At runtime, chunks stream in and out of a bandwidth-throttled GPU
  ring buffer based on screen-space error and camera distance, with dithered
  (Bayer-pattern) cross-fades between LOD levels so refinement never pops.
- **GPU silhouette detection.** A compute pass renders a low-res item/depth
  buffer, extracts screen-space silhouette edges directly on the GPU, and
  biases streaming to refine those edges toward the finest LOD first — so
  contours stay crisp while the rest of a receding object coarsens.
- **GPU-driven culling and sorting.** Per-chunk AABB frustum culling and
  normal-cone backface culling run in the amplification shader; a GPU bitonic
  radix sort keeps overlapping splats correctly depth-ordered for alpha
  blending — all without a CPU round-trip.
- **Streaming controls for experimentation.** Bandwidth throttling, an
  LRU-style decay pass for forcing cache eviction, and a live residency
  equalizer visualizing exactly which chunks are resident, transitioning, or
  silhouette-locked at each LOD level.

Built on top of AMD's [Cauldron](https://github.com/GPUOpen-LibrariesAndSDKs/Cauldron)
framework (vendored as a git submodule under `libs/cauldron`) for device/
swapchain/ImGui bootstrap — Cauldron ships no sample apps of its own, so
`src/DX12/` and `tools/SurfelsPreprocess/` are written directly against its
`FrameworkWindows` API.

## Solution & Workspace Structure

The solution contains two complementary projects organized into **Apps** and **Tools**:

1. **`Surfels_DX12` (Apps)**: The real-time DirectX 12 Mesh Shader Viewer:
   - Dynamic progressive disk streaming of multi-resolution wavelet packages (`.sflw` + `.json`).
   - Normal-oriented elliptical discs with procedural circular pixel clipping.
   - Screen-Space Error (SSE) hierarchical AutoLOD selection and frustum culling.
   - Built-in CPU/GPU frame profiler and real-time streaming telemetry HUD.

2. **`SurfelsPreprocess` (Tools)**: The offline PLY and dataset processing tool:
   - Built-in synthetic urban street benchmark generator (`--generate <N>`).
   - Binary & ASCII `.ply` parser.
   - 64-bit Morton Z-order curve spatial partitioning into cubic chunks.
   - Second-Generation Lifting Wavelet decomposition with deadband sparsification.
   - 8-byte GPU surfel quantization (`10:10:10:2` pos, `oct16` normal, `rgb565` color) and stream compression.

### Convenient Scripts & Visual Studio Targets
- In Visual Studio's **Select Startup Item** dropdown, you can pick:
  - `Surfels_DX12 (DirectX 12 Viewer)`
  - `SurfelsPreprocess (Generate 300K Benchmark)`
  - `SurfelsPreprocess (Process Custom PLY)`
- Or run helper scripts in `bin/`:
  - `bin/launch.cmd`: Launches the viewer.
  - `bin/preprocess_benchmark.cmd`: Regenerates a 300K benchmark dataset.
  - `bin/preprocess_ply.cmd`: Drag-and-drop any `.ply` file to convert it for streaming.

## Building

Prerequisites (per [Cauldron's own README](https://github.com/GPUOpen-LibrariesAndSDKs/Cauldron)):

- CMake 3.24+
- Visual Studio 2019 or newer, with the Windows 10 SDK
- No Vulkan SDK needed — the root `CMakeLists.txt` forces `GFX_API=DX12`

```bat
git clone --recurse-submodules <this repo>
cd surfels
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
```

**You don't need to build Cauldron.** `libs/cauldron-prebuilt/lib/{Debug,Release}/` ships prebuilt
`Cauldron_Common`/`Cauldron_DX12`/`ImGUI` static libraries (checked in via [Git LFS](https://git-lfs.com/),
so `git lfs install` once per machine before cloning, or `git lfs pull` after if you already cloned
without it) — the build links against those directly instead of compiling Cauldron's ~50 source files.
This is controlled by the `CAULDRON_USE_PREBUILT` CMake option (`ON` by default); it's still Cauldron's
own source/headers doing the work, this only skips recompiling three of its libraries, and it falls back
to a normal from-source build automatically if the prebuilt `.lib` files aren't present (e.g. LFS objects
not pulled). Pass `-DCAULDRON_USE_PREBUILT=OFF` to force a from-source build regardless (useful if you're
patching Cauldron itself, or building for a toolset/platform the prebuilt libs don't cover).

Open `build/Surfels_DX12.sln` in Visual Studio and build/run, or build from the
command line with `cmake --build . --config Debug`. The executable and its
shaders land in `bin/` (`bin/ShaderLibDX/Surfels.hlsl` gets copied there by the
CMake build so `CompileShaderFromFile` can find it at runtime).

**Use a Visual Studio generator, not Ninja.** Cauldron's own `src/Common` and
`src/DX12` CMakeLists.txt both copy overlapping FidelityFX headers into the
same `bin/ShaderLibDX` output path. MSBuild tolerates that; Ninja's single
global build graph rejects it as "multiple rules generate ...". If you open
this folder directly in Visual Studio (rather than running `cmake` yourself),
check `CMakeSettings.json` — it needs `"generator"` set to a Visual Studio
generator matching your installed version (e.g. `"Visual Studio 17 2022
Win64"`), not the CMake Tools default of `"Ninja"`.

If you already cloned without `--recurse-submodules`, run
`git submodule update --init --recursive` first.

`CMakeSettings.json` defines both `x64-Debug` and `x64-Release` configurations
(pick one from Visual Studio's configuration dropdown); both build to the same
`bin/` output, with Debug binaries getting a `d` suffix (`Surfels_DX12d.exe`
vs `Surfels_DX12.exe`).

This has been built and run end-to-end (not just compiled) on Windows with
Visual Studio, confirming device/swapchain creation, mesh shader compilation,
the `DispatchMesh` splat draw, and the ImGui panel all work, in both Debug
and Release.

## Known gaps

- Requires D3D12 Mesh Shader Tier 1 (Shader Model 6.5+) hardware/driver —
  checked via `D3D12_FEATURE_D3D12_OPTIONS7` on startup, with a message box
  and clean exit if unsupported. Any DX12 Ultimate-class GPU (RTX 20-series+,
  RDNA2+) has this.
- The main splat pass renders with a depth buffer bound (`D32_FLOAT`, cleared
  every frame) but hardware depth test/write both disabled
  (`DepthWriteMask = ZERO`, `DepthFunc = ALWAYS`) — intentional, not a
  placeholder gap: overlapping splats are alpha-blended, so correct visual
  order comes from the GPU bitonic radix depth sort (front-to-back is wrong
  for blending; the sort orders them back-to-front) rather than from
  per-pixel hardware Z-testing, which would incorrectly reject translucent
  surfaces behind whatever drew first. Enabling real depth test/write on
  this pass would break blending, not improve it. The separate GPU
  silhouette item-prepass (`SurfelsPreprocess` only) is a different story —
  it does use a real depth test (`DepthEnable = TRUE`, `DepthFunc = LESS`)
  since it needs correct nearest-item-wins occlusion, not blending.
- No Agility SDK opt-in (see the comment in `SurfelsSample.cpp`) — uses
  whatever D3D12 runtime Windows provides.
- `SurfelsSample.cpp` calls `InitDirectXCompiler()` (from
  `Common/base/DXCHelper.h`) before `CreateShaderCache()` — easy to miss
  since the current `DX12/base/ShaderCompilerHelper.h` doesn't mention it at
  all; skip it and every shader compile silently fails with a
  `SpvSize != 0` assert and no useful error message (see the next point).
- Cauldron's own `DXCHelper.cpp` (`DXCompileToDXO`) has a use-after-free in
  its error-reporting path: it releases `pLibrary`, then on the failure
  branch calls `pLibrary->GetBlobAsUtf8(...)` on the already-released
  pointer. In practice this swallows the real DXC error text instead of
  crashing outright, so a genuine shader compile error just looks like the
  assert above with nothing in `Cauldron.log` to explain it. Not patched
  here (it's vendored code); if a shader ever fails to compile, don't trust
  the log until this is fixed upstream or patched locally.
- The build's post-build step overwrites Cauldron's vendored DXC
  (`dxcompiler.dll`/`dxil.dll`, v1.6.2106.3 from 2021) with the Windows
  SDK's redistributable copy from `Windows Kits/10/Redist/D3D/x64`, purely
  because that path is hardcoded to this dev machine's SDK install. If that
  path doesn't exist, CMake just warns and leaves Cauldron's older DXC in
  place — which turned out not to matter for the actual bug above, but is
  still worth having a newer compiler.

## Where this goes next

Real scene geometry is already the normal path (`.ply`/`.splat` loading, the
wavelet LOD hierarchy, GPU streaming) rather than a placeholder -- the
obvious next step toward an actual surfel GI renderer is an
irradiance-accumulation/shading pass, since splats currently get simple
per-vertex color with a flat diffuse (`N.L`) term and no global illumination.

## License

This project is licensed under the [Apache License, Version 2.0](LICENSE).
