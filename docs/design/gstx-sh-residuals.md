# Design: SH residuals against the coarser level

Status: implemented
Date: 2026-10-26
Requirements: [gstx-sh-residuals](../requirements/gstx-sh-residuals.md)

## Summary
Within a chunk, every level's SH stream except the coarsest's holds residuals: for each splat, the difference between
its SH and a prediction from its parent, the nearest splat of the next coarser level. The difference is taken in
YCoCg-R space, byte by byte modulo 256, so it is exact. Records keep their absolute SH scale, so nothing else changes.
Format 14: `payloadTransforms` bit 2, and the SH Precision of each band in bits 8-19, which the reader needs to snap
its prediction exactly as the writer did. Spec Clause 8.2.

## Files

| Area | Files | Change |
|---|---|---|
| Coder | `libs/gstx/SHCoding.h` (new, open, header-only) | `ParentMap` (exact nearest neighbour on 16-bit positions, ties to the lowest index, through a grid over the parent level's extent); `ScaleFactor` and `Predict` (integer); `EncodeResiduals`/`DecodeResiduals`; `SnapTable` (integer) and `RequantiseSH` moved here from StreamPackager.h; `PackBits`/`UnpackBits` |
| Format | `libs/gstx/ColourTransform.h`, `src/SplatCore/WaveletTypes.h`, `docs/GSTX_FORMAT_SPECIFICATION.md` | `PAYLOAD_TRANSFORM_SH_RESIDUAL`, the precision fields, `PAYLOAD_TRANSFORMS_DEFAULT` (what a writer sets) apart from `_KNOWN` (what a reader accepts); version 14; Clause 8.2 |
| Writer | `src/SplatCore/StreamPackager.h` | Every level of a chunk encoded first, then the residuals finest first, then YCoCg on the coarsest level's SH only |
| Reader | `src/SplatCore/StreamPackager.h` | YCoCg undone on the coarsest level only; after all payloads are read, each chunk restored coarsest first, chunks across the cores |
| Unreal | `UE5SplatStream/.../SplatPackageCore` | Version 14; the plugin draws the DC colour only and never decodes SH, so it only accepts the bits |

## Decisions
- **Integer prediction.** The parent's SH times the ratio of the two f16 SH scales: the ratio is an exact fixed-point
  factor (24 fraction bits) from the halves' integer significands and exponents, applied with a shift. Floating point
  could round differently on another compiler or with other flags, and a reader that predicts one byte differently
  restores wrong colours silently.
- **Nearest parent, not inverse-distance weighting.** One parent, found exactly, is what the measurement used (-31% of
  the SH stream on six chunks) and is cheap; a weighted prediction would need more arithmetic to stay exact.
- **Snapped prediction.** At reduced SH Precision the prediction is snapped like the stream, so residuals stay on the
  lattice of the snapped values. At 8 bits snapping changes nothing and is skipped.
- **Empty parent level.** The prediction is zero, so the residual is the plain YCoCg value.

## Measured (Cthulhu, 3.32M splats, SH degree 3)

| SH Precision | Without residuals | With residuals | Change | With residuals, against 8 bits |
|---|---|---|---|---|
| 8 bits | 279.0 MB | 216.8 MB | -22.3% | |
| 6 bits | | 186.4 MB | | -14.0% |
| 5 bits | 185.6 MB | 169.0 MB | -9.0% | -22.1% |
| 4 bits | | 144.5 MB | | -33.4% |

The SH Precision menu and tooltip give the last column.

Load (TestLoadPackage): 2.95 s without, 3.45 s with (the first version took 12.3 s: a fixed 64-cell grid over the
whole model put a chunk in a few crowded cells, and 45 integer divisions per splat; now a grid per level and one
factor per splat, chunks in parallel).
