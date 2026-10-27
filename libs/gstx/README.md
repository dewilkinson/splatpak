# gstx (public build)

This is the public version of this repository. The algorithmic core of the project lives in
this directory: the lifting-wavelet LOD decomposition, the compression codec, the interior
occlusion volume generator, and the streaming scheduler's ordering (which regions of a model
stream first and how a frame's bandwidth is shared out, with the detail grid it reads). In
the private original those modules are proprietary source, built as a static library and
licensed separately from the rest of the project.

The public repository ships that same algorithm core in **two forms**, and the CMake file in
this directory picks between them automatically:

## 1. Prebuilt proprietary library (default)

`prebuilt/Release/gstx.lib` and `prebuilt/Debug/gstx.lib` are the real
algorithms, compiled from the private repository's source and checked in here through Git
LFS (the same arrangement as `libs/cauldron-prebuilt/`). When they are present, every
consumer (`SplatPak`, `SplatPakViewer`, the console test tools) links against them and behaves
exactly like the private build: the Splat Generator bakes a genuine interior occlusion
volume, the LOD pyramid is the real lifting wavelet, packages are entropy-coded, and `.gstx`
files are interchangeable with those written by the private build.

- The binaries are MSVC x64 static libraries, compiled `/MD` (Release) and `/MDd` (Debug)
  with embedded debug information (`/Z7`), against the headers in this directory. They were
  built with MSVC 14.51 (Visual Studio 2026) and need an MSVC 14.51 or newer toolset to link:
  they reference STL runtime helpers such as `__std_rotate` that older toolsets (Visual
  Studio 2022's MSVC 14.44, for example) do not provide. CMake checks the toolset and uses
  the open stand-ins below on anything older.
- **Licensing:** the two `.lib` files are proprietary object code, **not** covered by the
  repository's top-level Apache-2.0 `LICENSE`. They are licensed under the
  [GSTx Binary License](prebuilt/LICENSE.txt)
  (SPDX `LicenseRef-Blueshell-GSTx-Binary`): you may use them, copy them together
  with this repository (clones and forks included), and link them into builds of this
  project that you distribute, commercially or otherwise; you may not distribute them on
  their own or reverse engineer them. Everything else in this directory (the headers and
  the open `.cpp` files) is Apache-2.0 like the rest of the repository.
- If a clone has the `.lib` files as ~130-byte Git LFS pointer files instead of real
  libraries, run `git lfs pull` and re-configure. CMake detects that case and falls back to
  the open stand-ins rather than failing the link.

## 2. Open stand-ins (fallback)

When the prebuilt library is unavailable (LFS objects not pulled, a non-MSVC or non-x64
toolchain, or configuring with `-DGSTX_USE_PREBUILT=OFF`), the simplified
Apache-2.0 implementations in this directory are compiled instead:

- `LiftingWavelet.h`/`.cpp` — plain decimation (every other point per level) rather than a
  true lifting-wavelet transform.
- `ByteShuffle.h`/`.cpp` — the byte-plane transpose, and a pass-through in place of the
  run-length stage that packages before format v9 were entropy-coded with, so this build
  cannot read those. Packages written now (v9) go through Brotli in both builds and compress
  the same: `PayloadDecode.h`/`.cpp` (decoding) and `src/SplatCore/PayloadCodec.h` (encoding)
  are open code, and Brotli (`libs/brotli/`, MIT) is compiled into this library either way.
- `OcclusionVolume.h`/`.cpp` — a no-op; the fallback build has no interior occlusion volume
  feature at all. `SplatPak` still renders a volume that a package already carries, but the
  Splat Generator's *Generate Interior Occlusion Volume* control bakes nothing.
- `StreamOrder.h`/`.cpp` — plain chunk order: blocks grouped by octant of the bounding
  octahedron, listed coarse-to-fine in chunk-index order and served round-robin, with view-driven
  requests ranked by frustum band and level only. The private build's scheduler decides which
  regions stream first and how bandwidth is shared across the visible faces.
- `DetailHeatmap.h`/`.cpp` — occupancy only: every occupied cell gets the same score, so the grid
  a package carries is valid but drives no ordering. The private build scores each cell.
- `SplatCodec.h`/`.cpp` — splat mode's record encoder and decoder. The stand-in writes and reads
  the documented 20-byte Gaussian record with plain rounding but keeps no spherical harmonics
  above the DC term (its packages have an empty SH stream) and grows the coarser levels by a
  fixed factor. The private build quantises and derives the levels differently.
- `StreamRuntime.h`/`.cpp` — the streaming runtime. The stand-in cuts each level into blocks of
  64 points, delivers them coarsest level first in chunk order within the bandwidth budget, and
  draws one model-wide level, swapping to the next as soon as it is complete. Greedy keeps
  fetching finer levels; Conservative stops at the target level. The private build streams and
  refines block by block, with cross-fades, per-block levels and decay.
- `ShadowMesh.h`/`.cpp` — builds no shadow mesh: packages this build writes have no mesh section
  (format v12). It still reads and draws the mesh of a package written by the private build: the
  section codec, `ShadowMeshCodec.h`/`.cpp`, is open code. The private build bakes a smooth mesh
  following the visible surface in three levels of detail.
- `CodecBuild.h`/`.cpp` — reports which build is linked. The app shows a one-line banner row
  (`CODEC: open stand-in ...`) whenever the stand-ins are running, naming what is missing.

Every module keeps the exact struct layouts and function signatures of the private build's
public API, so the rest of the codebase compiles and links unchanged against either form --
only the object code behind the headers differs. The CMake configure output says which one
was chosen (`gstx: linking the prebuilt proprietary library` or
`gstx: compiling the open stand-in implementation`).

The rest of this repository's git history is real and unaltered. These specific file paths
are the one exception: the proprietary algorithm source that used to live here (at this
path, under an earlier name at `libs/SplatCore/`, and earlier still at
`tools/SplatPak/`) was removed from every commit rather than rewritten in place, so
it never appears anywhere in this history -- these files exist only as of the commit that
added this public build.
