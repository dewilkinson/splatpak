# Command-line access to every entry point: design

Requirements: [docs/requirements/cli.md](../requirements/cli.md). Branch `headless-cli`.

## Shape

`SplatPakCli.exe` (`tools/SplatPakCli`), a console program beside `SplatPak.exe`. Not `splatpak.exe`: Windows file names
ignore case, so it would be `SplatPak.exe`. Subcommands, one dispatcher (`main`), exit codes 0 / 1 / 2.

| Command | Phase 1 | Phase 2 |
|---|---|---|
| `info`, `verify` | headless: `StreamPackager::LoadPackage` (header-only, decodes every payload) and `PackageView::Read` | adds the quality figures once packages record them |
| `set-background` | headless: `PackageView::Read` / `Write`, as `SplatPak.exe --set-background` | unchanged |
| `bake` | started `SplatPak.exe <scan> --bake-to <out>` (with its window) | **done:** `SplatPak.exe --headless --bake-to`, no window or device (below) |
| `view`, `capture`, `test ...` | start `SplatPak.exe` with the matching flags | unchanged: they need the renderer |
| `app -- <args>` | any `SplatPak.exe` argument, so every app flag is reachable now | unchanged |
| `analyze` | not yet | the RDO job (`RdoJob.h`) run locally, as `SplatPakWorker` runs it |
| `export` | not yet (open question in the requirements) | a level back to a `.ply` |

Forwarded commands make path arguments absolute first, because `SplatPak.exe` runs in its own folder to find its shaders;
options and plain numbers pass through as they are.

## Phase 2: the headless bake (done 2026-10-28)

Checking the four steps below showed none of them calls the renderer, and with `--bake-to` every operation already runs
inline (`UseBackgroundOps` is false). So instead of extracting them first, `SplatPak.exe --headless` builds the app object
without `RunFramework` (which is what creates the window and the D3D12 device), parses the command line, and runs
`SplatPakApp::RunHeadless`: `LoadConfigFile`, `LoadFile` (open, read and bake inline), then the windowed `--bake-to`'s own
`StepCliBake`. It is the same code, so the package is the same: the dwarves scan baked headless and windowed gave
byte-identical files (SHA-256 `D155DAE3...`, 5,583,912 bytes, 7 s against 8 s). `SplatPakCli bake` passes `--headless`.

The bake library below is still the way to give other hosts (the UE5 plugin) bake and IO functions to link; it is no
longer needed for the CLI.

## The bake library (later)

A package comes from four steps that live in `SplatPakApp` today:

1. `RunBake` (const, inputs as parameters): bounds, the detail grid, octree chunks, the wavelet decomposition and merged
   Gaussians, encode parameters, plus the panel's size, colour and entropy estimates.
2. `ApplyBake`: moves the result into the app's state (`m_chunks`, `m_waveletResult`, `m_splatParams`, ...).
3. The shadow mesh job (`m_shadowMeshJob`), started after a bake.
4. `ProcessAndExport`: writes the package from that state through `StreamPackager` (and the background Brotli
   precompression).

The library (`src/SplatBake`, Apache-2.0, linking `gstx`) gets a `BakeInput` (the scan's points and attributes), the
`BakeParams`, and progress / cancel callbacks, and returns a `BakedModel` holding what steps 1 and 3 produce; `WritePackage`
does step 4 from it. The app keeps its UI-only parts (the estimates shown on the panel can stay app-side or be an option)
and calls the library for 1, 3 and 4, so there is one bake path. Acceptance criterion 1 (byte-identical packages) is the
check that the extraction changed nothing.
