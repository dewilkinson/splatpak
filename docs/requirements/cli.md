# Command-line access to every entry point

Status: proposed
Date: 2026-10-28
Owner: Dave Wilkinson

## Goal
Every operation SplatPak offers can be run from a command line, scripted and unattended, with the same results as the
app: baking a scan into a package, reading and inspecting packages, editing a package's settings, compression analysis,
captures and the test suites. Operations that need no GPU run without a window, so they work on a build machine, in CI
and from the UE5 demo's scripts.

## User-visible behaviour
- One console program, `splatpak` (`SplatPakCli.exe`, installed beside `SplatPak.exe` and shipped in the release and the
  zip), with subcommands. `splatpak help` lists them; `splatpak <command> --help` describes one.
- Headless subcommands (no window, no GPU):
  - `bake <scan.ply|.splat|.sog> -o <package.gstx> [bake options]`: the app's Bake then Save, with the bake panel's
    settings as options (`--chunk-size`, `--max-lods`, `--sh-degree`, `--sh-bits`, `--deadband-mm`, `--occlusion`,
    `--shadow-mesh`, `--no-precompress`, `--remember`), and the remembered settings for that scan by default, as the app
    does.
  - `info <package.gstx> [--json]`: format version, record format, levels with splat counts and sizes, SH degree and
    bits, chunking, the shadow and occluder meshes, the view settings (background, training cameras), the package size
    against the scan and, when the package records them, the quality figures.
  - `verify <package.gstx>`: decodes every level and section and reports any error; exit code 1 on a bad package.
  - `export <package.gstx> -o <scan.ply> [--level N]`: a level back to a standard 3DGS `.ply`.
  - `set-background <package.gstx> solid <r,g,b> | gradient <r,g,b> <r,g,b> | none`: as the app's `--set-background`
    today.
  - `analyze <scan.ply> [--out report.json]`: the Rate Distortion Optimization analysis (SplatPakWorker's job, run
    locally).
- Window subcommands (they start SplatPak's renderer, and say so): `capture` (the camera captures and view captures),
  `test lod-quality|lod-calibration|stream|occlusion|ring` (today's `--lod-quality-test` and the other suites), and
  `view <package>` (opens the app on it).
- Every subcommand: exit code 0 on success, 1 on failure, 2 on a usage error; progress on stderr as
  `progress <0..1> <stage>` (SplatPakWorker's form); results on stdout, `--json` where a result is structured.
- The app's existing command-line flags keep working (`SplatPak.exe --bake-to`, `--set-background`, the test flags), so
  scripts written today don't break.

## Library API (the "IO functions")
- The headless subcommands are thin wrappers over a C++ API any host can link, the UE5 plugin and its demo included:
  load a scan, bake (parameters in, a baked model out, with progress and cancel callbacks), write a package, read a
  package (header, levels, sections, view settings), edit a package's view settings in place, and export a level.
- The app's Bake, Save and Open go through the same API, so a package baked by `splatpak bake` is byte-identical to one
  the app bakes with the same settings.

## Non-goals
- Training Gaussian scenes from photos, or any new bake method.
- A GUI-free renderer: captures and visual tests keep using SplatPak's D3D12 renderer.
- Linux or macOS builds.

## Constraints
- Public / private placement: the bake's algorithms stay in `libs/gstx` (private source, prebuilt `gstx.lib` in the
  public tree under the GSTx Binary License). The CLI, the loaders and the API's headers are app-side and public
  (Apache-2.0), like `SplatPakWorker`. The public build links the open stand-ins, as the app does today.
- No package format change.
- The app's behaviour, UI and performance are unchanged by the refactor.

## Acceptance criteria
1. `splatpak bake` on each bundled scan with the bundled packages' settings writes a package byte-identical to the one
   SplatPak's Bake + Save writes with the same settings.
2. `bake`, `info`, `verify`, `export` and `set-background` run on a machine with no GPU (the WARP-less test: no D3D12
   device is created; checked by running them in a Windows Sandbox without a GPU).
3. `splatpak export` then `splatpak bake` on the exported `.ply` gives a package whose level 0 matches the original's
   within the codec's quantisation.
4. Every app command-line flag listed by `SplatPak.exe --help` today has a `splatpak` subcommand or option that does the
   same thing.
5. The release zip and installer include `SplatPakCli.exe`, and the UE5 demo has a `Scripts/BakeGstx.ps1` that bakes a
   scan with it and imports the result.
6. The existing tests (`TestLoadPackage`, `TestSplatCodec`, `CompressVenus`, the stress test) pass, plus a new CLI test
   that runs each headless subcommand on the dwarves package and checks exit codes and output.

## Open questions (the user's call)
- One program with subcommands (`splatpak bake ...`, proposed) or one per operation (`SplatPakBake.exe`, ...)?
- Should window subcommands live in the CLI (it starts the renderer) or stay as `SplatPak.exe` flags that the CLI only
  forwards to?
- `export` to `.ply`: wanted? It makes the round trip testable, but it hands anyone with a package a standard scan back.
