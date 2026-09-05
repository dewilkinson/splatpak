# Surfels

> Just want to use the app? See the [User Guide](docs/USER_GUIDE.md) instead — this README covers the technical architecture.

A GPU-driven DirectX 12 **mesh shader** renderer for massive surfel/point-cloud
datasets — wavelet-based multi-resolution LOD streaming and GPU silhouette
refinement, with geometry generated, culled, sorted, and cross-faded entirely
on-GPU. There is no vertex/index buffer and no `DrawInstanced` anywhere in the
pipeline: every splat is procedurally emitted by an amplification/mesh shader
pair each frame, directly from a `StructuredBuffer` of packed surfels.

**Core pieces:**

Built on top of AMD's [Cauldron](https://github.com/GPUOpen-LibrariesAndSDKs/Cauldron)
framework (vendored as a git submodule under `libs/cauldron`) for device/
swapchain/ImGui bootstrap — Cauldron ships no sample apps of its own, so
`src/DX12/` and `tools/SurfelsPreprocess/` are written directly against its
`FrameworkWindows` API.

## Solution & Workspace Structure

The solution is organized into Solution Explorer folders:

1. **`SplatLab` (Tools)** — source in `tools/SurfelsPreprocess/`. The primary
   application: preprocessor and viewer in one window, and the default startup
   project. This is what the User Guide describes.
   - Loaders for binary and ASCII `.ply` and for `.splat` (3D Gaussian Splat)
     files, plus a built-in synthetic urban-street benchmark generator.
   - 64-bit Morton Z-order curve spatial partitioning into cubic chunks.
   - Second-generation lifting wavelet decomposition with deadband sparsification.
   - 8-byte GPU surfel quantization (`10:10:10:2` position, `oct16` normal,
     `rgb565` colour), byte-shuffle transposition, and a run-length byte codec.
   - Optional interior occlusion volume bake.
   - The full renderer: mesh-shader splatting, GPU silhouette item-prepass,
     temporal accumulation (TAA) resolve, occlusion volume, and the streaming
     simulator with bandwidth/decay controls and the residency equalizer.
   - Accepts a `.ply`, `.splat`, or `.sflw` path on the command line (drag a
     file onto the executable). With no argument it loads the startup dataset
     from `config.json` (`startup_dataset`, default `assets/cthulu/cthulu.sflw`).

2. **`Surfels_DX12` (Apps)** — source in `src/DX12/`. The standalone viewer,
   with no preprocessing UI. Loads `scene.sflw` from its working directory (or
   a `models/` folder near it) and streams it; if none is found it generates a
   synthetic benchmark package on first launch. It shares the renderer and
   streaming manager with SplatLab but omits the silhouette prepass, TAA,
   occlusion volume, and streaming-simulator UI.

3. **Tests** — `tests/`. Small standalone console executables, each built as its
   own target: `TestBitonicCPU` (CPU reference for the bitonic sort network),
   `TestGPUSort` (runs `GPURadixSortCS.hlsl` on a raw D3D12 device and checks
   against a CPU sort), `TestGeometryCull` (culling-statistics math),
   `TestLoadPackage` (loads `models/venus.sflw` and prints chunk/surfel counts),
   `TestOcclusionVolume` (runs the occlusion volume generator on a package and
   prints its trace; optional resolution override and shave values), and
   `CompressVenus` (command-line packager: writes the same `.sflw` SplatLab
   would export for a `.ply` with default settings, occlusion volume included;
   used to regenerate the bundled assets).

4. **`bluesec-codec` (Core)** — source in `libs/bluesec-codec/`. A static library holding the
   lifting-wavelet decomposition, the byte-shuffle/RLE codec, and the interior occlusion
   volume generator: the three pieces of this project judged distinctive enough to be
   worth keeping as compiled objects rather than open source, even within this repo. Each
   module is a public header (data structures and function declarations only) plus a
   `.cpp` implementation; `SplatLab`, `Surfels_DX12`, and the console test tools all link
   against it. **This subtree is explicitly not covered by the repository's top-level
   Apache-2.0 `LICENSE`** — see `libs/bluesec-codec/README.md` for the licensing boundary
   and its limits (a static library keeps source out of ordinary distribution; it doesn't
   protect against disassembly of a shipped binary, and the GPU-side shader source under
   `src/DX12/Shaders/` still ships as plain text in `bin/ShaderLibDX/` regardless).

5. **Docs** — a build-nothing target that lists `README.md` and
   `docs/USER_GUIDE.md` in Solution Explorer for editing.

6. **ThirdParty/Cauldron** — every vendored Cauldron target, swept into one folder.

### Public repository

`https://github.com/dewilkinson/splatlab` is a public mirror of this repository with
`bluesec-codec`'s proprietary implementation replaced by an open stand-in (see that
library's own README for exactly what differs) — the algorithm never appears anywhere in
its git history, not just at the current tip. It's generated, not hand-maintained: run
`python scripts/sync-public-repo.py` from a clean working tree to rebuild it from the
current state of this repo (strips the proprietary paths from every commit, drops in the
open stand-in, swaps a couple of private-repo-specific README passages, builds the result,
regenerates the bundled example packages so they're readable by the open codec, runs the
stress test, and pushes). Pass `--no-push` to inspect the result first. The script's own
header comment documents each step and the config that needs updating if a proprietary
module is ever renamed or moved again.

### Package format (`.sflw`)

A package is a single self-contained binary file (format version 5):

| Section | Contents |
|---|---|
| `SFLWFileHeader` | magic, version, chunk count, LOD count, global bounds, splat radius, absolute offsets of the occlusion volume and manifest, and the byte size of the original input file (so the compression ratio shown is always source file vs package file) |
| Chunk LOD payloads | one compressed blob per chunk per LOD level, in export order |
| Occlusion voxels | optional `OcclusionVoxelGPU` array (only when a volume was baked) |
| Manifest table | one `ChunkManifestRecord` per chunk (id, bounds, centre, radius, LOD count) followed by its `ChunkLODHeader` array (surfel count, byte sizes, payload offset, geometric error) |

Every payload offset is absolute, so the manifest is written last and the
header patched once all offsets are known. Packages written by format versions
1–3 kept the manifest in a companion `.json`; both apps still load those if the
`.json` sits next to the `.sflw`. See `src/DX12/Wavelet/WaveletTypes.h` for the
structs and version history.

### Helper scripts

`bin/` is build output and is not tracked, but two convenience launchers live there:

- `bin/launch_surfellab.cmd` — launches SplatLab, forwarding any arguments (so a file path can be dropped on it).
- `bin/launch.cmd` — launches the standalone viewer.

## Building

Prerequisites (per [Cauldron's own README](https://github.com/GPUOpen-LibrariesAndSDKs/Cauldron)):

- CMake 3.24+
- Visual Studio 2019 or newer (MSVC toolset 142+), with the Windows 10 SDK
- No Vulkan SDK needed — the root `CMakeLists.txt` forces `GFX_API=DX12`

```bat
git clone --recurse-submodules https://github.com/dewilkinson/surfels.git
cd surfels
mkdir build && cd build
cmake .. -G "Visual Studio 18 2026" -A x64
```

Use whichever Visual Studio generator matches your install (`"Visual Studio 17 2022"` works the same way).

**You don't need to build Cauldron.** `libs/cauldron-prebuilt/lib/{Debug,Release}/` ships prebuilt
`Cauldron_Common`/`Cauldron_DX12`/`ImGUI` static libraries (checked in via [Git LFS](https://git-lfs.com/),
so `git lfs install` once per machine before cloning, or `git lfs pull` after if you already cloned
without it) — the build links against those directly instead of compiling Cauldron's ~50 source files.
This is controlled by the `CAULDRON_USE_PREBUILT` CMake option (`ON` by default); it's still Cauldron's
own source/headers doing the work, this only skips recompiling three of its libraries, and it falls back
to a normal from-source build automatically if the prebuilt `.lib` files aren't present (e.g. LFS objects
not pulled). Pass `-DCAULDRON_USE_PREBUILT=OFF` to force a from-source build regardless (useful if you're
patching Cauldron itself, or building for a toolset/platform the prebuilt libs don't cover).

The `.ply` assets under `assets/` are also LFS objects.

Open the generated solution in `build/` (`Surfels_DX12.slnx` with the VS 2026
generator, `Surfels_DX12.sln` with older ones) and build/run, or build from the
command line with `cmake --build . --config Release`. SplatLab is pinned as the
startup project. Executables and shaders land in `bin/`: every `.hlsl` under
`src/DX12/Shaders/` is copied to `bin/ShaderLibDX/` so `CompileShaderFromFile`
can find it at runtime, and the post-build step also clears Cauldron's on-disk
shader cache under `%LOCALAPPDATA%\AMD\Cauldron\ShaderCacheDX` so edited
shaders are always recompiled.

**Use a Visual Studio generator, not Ninja.** Cauldron's own `src/Common` and
`src/DX12` CMakeLists.txt both copy overlapping FidelityFX headers into the
same `bin/ShaderLibDX` output path. MSBuild tolerates that; Ninja's single
global build graph rejects it as "multiple rules generate ...". If you open
this folder directly in Visual Studio (rather than running `cmake` yourself),
check `CMakeSettings.json` — it needs `"generator"` set to a Visual Studio
generator matching your installed version (it ships set to `"Visual Studio 18
2026 Win64"`), not the CMake Tools default of `"Ninja"`.

If you already cloned without `--recurse-submodules`, run
`git submodule update --init --recursive` first.

`CMakeSettings.json` defines both `x64-Debug` and `x64-Release` configurations
(pick one from Visual Studio's configuration dropdown); both build to the same
`bin/` output, with Debug binaries getting a `d` suffix (`SplatLabd.exe`
vs `SplatLab.exe`). Release builds keep full optimization but also emit PDBs
(`/Zi` + `/DEBUG`) so crashes in the shipping configuration are debuggable.

### Runtime configuration

SplatLab reads an optional `config.json` (searched in the working directory
and a few parent directories; `surfels_config.ini` is also accepted). Keys
that are read: `startup_dataset` (path loaded when no file is given on the
command line), `benchmark_dataset`, and `occlusion_shave_bias` (extra cells,
positive or negative, added to the occlusion volume's unconditional cull band
around the sampled surface; use it when a noisy cloud still shows cubes poking
through). The file SplatLab writes when none exists also lists
`fallback_synthetic_points`, `default_chunk_size`, `default_max_lods`, and
`default_deadband_mm`, but those are informational and not currently parsed.
Without a config file the built-in defaults apply and the bundled
`assets/cthulu/cthulu.sflw` is loaded. A second bundled package,
`assets/venus/venus.sflw`, can be opened from the File menu. Both are
regenerated with `CompressVenus` whenever the pipeline or the occlusion volume
generator changes, so they always match the current format.

## Known gaps

- Requires D3D12 Mesh Shader Tier 1 (Shader Model 6.5+) hardware/driver —
  checked via `D3D12_FEATURE_D3D12_OPTIONS7` on startup, with a message box
  and clean exit if unsupported. Any DX12 Ultimate-class GPU (RTX 20-series+,
  RDNA2+) has this.
- The main splat pass renders with a depth buffer bound but hardware depth
  test/write both disabled (`DepthEnable = FALSE`, `DepthWriteMask = ZERO`,
  `DepthFunc = ALWAYS`) — intentional, not a placeholder gap: overlapping
  splats are alpha-blended, so correct visual order comes from the GPU bitonic
  depth sort rather than from per-pixel hardware Z-testing, which would
  incorrectly reject translucent surfaces behind whatever drew first. Enabling
  real depth test/write on this pass would break blending, not improve it. The
  separate GPU silhouette item-prepass and the occlusion-volume cube pass
  (SplatLab only) are a different story — they use a real depth test
  (`DepthEnable = TRUE`, `DepthFunc = LESS`) since they need correct
  nearest-item-wins occlusion, not blending.
- `.sog` (PlayCanvas Spatially Ordered Gaussians) support exists as a loader
  header (`SOGLoader.h`, with its own ZIP/DEFLATE and WIC-based WebP decode)
  but is not yet wired into the file dialog or `LoadFile` dispatch.
- The chunk codec is a byte-shuffle plus run-length byte coder, not a real
  entropy coder. `ZstdDecompressor.h` is named for the intended eventual
  replacement and today just wraps `ByteShuffle`.
- No Agility SDK opt-in (see the comment in `SurfelsSample.cpp`) — uses
  whatever D3D12 runtime Windows provides.
- Both apps call `InitDirectXCompiler()` (from `Common/base/DXCHelper.h`)
  before `CreateShaderCache()` — easy to miss since the current
  `DX12/base/ShaderCompilerHelper.h` doesn't mention it at all; skip it and
  every shader compile silently fails with a `SpvSize != 0` assert and no
  useful error message (see the next point).
- Cauldron's own `DXCHelper.cpp` (`DXCompileToDXO`) has a use-after-free in
  its error-reporting path: it releases `pLibrary`, then on the failure
  branch calls `pLibrary->GetBlobAsUtf8(...)` on the already-released
  pointer. In practice this swallows the real DXC error text instead of
  crashing outright, so a genuine shader compile error just looks like the
  assert above with nothing in `Cauldron.log` to explain it. Not patched
  here (it's vendored code); if a shader ever fails to compile, don't trust
  the log until this is fixed upstream or patched locally.
- Each app's post-build step overwrites Cauldron's vendored DXC
  (`dxcompiler.dll`/`dxil.dll`, v1.6.2106.3 from 2021) with the Windows
  SDK's redistributable copy from `Windows Kits/10/Redist/D3D/x64`, purely
  because that path is hardcoded to this dev machine's SDK install. If that
  path doesn't exist, CMake just warns and leaves Cauldron's older DXC in
  place — which turned out not to matter for the actual bug above, but is
  still worth having a newer compiler.

## Where this goes next

Real scene geometry is already the normal path (`.ply`/`.splat` loading, the
wavelet LOD hierarchy, GPU streaming) rather than a placeholder — the
obvious next step toward an actual surfel GI renderer is an
irradiance-accumulation/shading pass, since splats currently get simple
per-surfel colour with a flat two-sided diffuse (`|N·L|`) term and no global
illumination.

## License

This project is licensed under the [Apache License, Version 2.0](LICENSE).
