# bluesec-codec

This directory holds the algorithmic core of Surfels: the lifting-wavelet LOD decomposition
(`LiftingWavelet.h`/`.cpp`), the compression codec (`ByteShuffle.h`/`.cpp`), and the interior
occlusion volume generator (`OcclusionVolume.h`/`.cpp`). It builds as a single static library,
`bluesec-codec`, that `SplatLab`, `Surfels_DX12`, and the console test tools link against.

## Licensing boundary

**This subtree is not covered by the repository's top-level `Apache License, Version 2.0`
(see `/LICENSE`).** Every file in this directory carries its own "All Rights Reserved" /
proprietary notice instead of the Apache SPDX tag used elsewhere in the repo. If this
repository, or any part of its git history, is ever made public, shared with a third party,
or relicensed, this directory needs to be excluded or explicitly re-licensed as part of that
decision -- don't assume the top-level `LICENSE` file extends to it.

## Why a static library

Each module here is split into a public header (data structures and function
declarations -- the API contract callers need) and a `.cpp` implementation (the actual
algorithm). Only the headers are visible to anything that links against the compiled
`bluesec-codec.lib`; the `.cpp` sources never need to leave this directory for the project to
build and run. This is a source-distribution boundary, not an anti-reverse-engineering
measure -- a shipped binary can still be disassembled by a sufficiently motivated party, DXIL
shaders in particular ship as plain-text `.hlsl` next to every executable today (see
`bin/ShaderLibDX/`) and are not addressed by this split at all. Treat this as the first layer
(keeping source out of ordinary distribution and off anyone's screen who doesn't need it),
not the only one -- NDAs on anyone who does see this source, and a decision about whether the
GPU-side logic in `src/DX12/Shaders/Surfels.hlsl` needs a similar treatment (shipping
precompiled DXIL instead of source), are the natural next steps if this needs to hold up
against a more determined party.
