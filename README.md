# Surfels

A minimal DirectX 12 sample built on AMD's [Cauldron](https://github.com/GPUOpen-LibrariesAndSDKs/Cauldron)
framework, vendored as a git submodule under `libs/cauldron`. It's a bare Cauldron
bootstrap intended as the starting point for a surfel-based GI renderer, not a
finished one: it draws a procedural, GPU-instanced point-splat cloud (billboard
quads placed on a Fibonacci sphere) with an ImGui control panel, and nothing else
yet — no scene geometry, no lighting/GI pass, no depth buffer.

Cauldron itself ships no bundled sample apps (that changed at some point after
`glTFSample`/`FidelityFX-CAS` were built against it); this project's `src/DX12/`
was written directly against Cauldron's current `FrameworkWindows` API by reading
its header/source (device + swapchain are now owned by the framework base class,
`OnCreate()` takes no window handle, etc.) rather than copied from an existing
sample.

## What's here

- `src/DX12/SurfelsSample.{h,cpp}` — the app shell: window/input handling,
  ImGui panel, orbit camera, `WinMain`. Subclasses Cauldron's `FrameworkWindows`.
- `src/DX12/SurfelsRenderer.{h,cpp}` — the GPU side: descriptor heaps, upload
  heap, constant buffer ring, command list ring, root signature/PSO, and the
  per-frame render loop (clear backbuffer → draw instanced splats → ImGui → present).
- `src/DX12/Shaders/Surfels.hlsl` — vertex/pixel shaders for the splats. No
  vertex or index buffers: each quad corner comes from `SV_VertexID` and each
  surfel's position from `SV_InstanceID` via a Fibonacci-sphere formula.
- `libs/cauldron` — the Cauldron framework, as a git submodule.

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

This has been built and run end-to-end (not just compiled) on Windows with
Visual Studio, confirming device/swapchain creation, shader compilation, the
instanced splat draw, and the ImGui panel all work.

## Known gaps

- No depth buffer / depth test — overlapping splats just draw in instance
  order, not sorted. Fine for a placeholder, not for real surfel rendering.
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

The obvious next steps toward an actual surfel GI renderer: replace the
procedural Fibonacci-sphere placement with surfels seeded from real scene
geometry (e.g. loaded via Cauldron's glTF loader), add a depth buffer, and
add an irradiance-accumulation/shading pass instead of the flat hash-color
shading the splats currently get.
