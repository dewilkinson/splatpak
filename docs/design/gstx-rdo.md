# Design: Rate-distortion optimisation (RDO) for GSTx bakes

Requirements: the Goals section below (user, 2026-10-26: "an RDO module that will automatically compress and encode the data to fit best quality for a given target rate")
Status: agreed (user, 2026-10-26: "proceed with all as recommended")
Date: 2026-10-26

## Summary
Today the person baking picks the encoding by hand (SH Quality, SH Precision, deadband, levels) and judges the result by eye; for Cthulhu a manual sweep of 12 bakes found SH degree 3 at 4 bits 41% smaller than the default with no measurable loss, and dropping to degree 2 costing about 11 dB. An RDO module does that search automatically: given a target (a package size, a compression ratio, or a quality floor), it chooses the encoding of every unit of the package to minimise distortion D + lambda x rate R, and finds the lambda that lands on the target. It is built in three phases: a whole-package search with no format change, then per-chunk choices (format 15), then splat pruning and record precision.

## Phase 1: built (2026-10-26)
- `src/SplatCore/RdoAnalysis.h` (the analysis, open: it uses the codec only through its public calls), `RdoProfile.h` (profiles),
  the Compression (RDO) menu in `SplatPakApp`, `tests/TestRdo.cpp`. The analysis sits in the app rather than libs/gstx: nothing in
  it is proprietary, and it needs the packager's SH path.
- Cthulhu (3.3 million splats, 6 levels): 20 candidates in 58 s (TestRdo; 82 s in the app, which also hashes the scan). Predicted
  sizes are 0.6-1.4% above the 12 packages of the hand sweep. `SHCoding::ParentMaps` lets one parent map serve every candidate:
  the residual coding was 120 of the first 182 s.
- Per-region estimate (phase 2's gain, without format 15): 0-3.5% on the high-quality frontier (degree 3, 4 bits: 0%; 5 bits: 1%),
  more only on degree 2 points, which the frontier rejects. Venus (4.7 million splats, 91 s): 0-4.9% on the same points.
  Recommendation: no format 15 for per-region SH choices; a whole-package choice gets nearly all of the gain. Decided
  (user, 2026-10-26): format 15 is skipped for now. (Venus's own
  win is at package level: degree 3 at 4 bits predicts 233 MB against 395 MB at 8 bits.)

## Goals
1. A **Target** control on the Package Bake tab: a package size in MB, a compression ratio, or a minimum quality (PSNR in dB); Bake then picks the encoding itself and reports the size and predicted quality it reached.
2. **Best quality for the rate**: for a given size, no hand-picked combination of the controls the module searches should beat it (checked against sweeps like the Cthulhu one).
3. **Measured, not guessed**: the predicted quality is checked against rendered views (the `--capture-views` comparison) on the bundled scans.
4. The choice is **remembered per scan** (bake_tunings.json, keyed by the file's hash), as hand-picked settings are now.
5. Bakes stay **reasonably fast**: phase 1 adds well under a minute to a Cthulhu bake.

## UI (user, 2026-10-26: the curve, with streaming time as a second way to set the target)
The SH Quality and SH Precision combos give way to a **Compression (RDO)** menu of its own on the Package Bake tab: a new collapsible menu bar like the other sections (its open state remembered), so the curve, the sliders and the readouts have room (user, 2026-10-26). It holds:
- **Analyse** runs the search's scoring pass and draws the scan's own rate-distortion curve: the best encoding at every size (the lower convex hull of the candidates), quality in dB against package size in MB.
- **A handle on the curve** marks the chosen point; dragging it moves along the curve. The target can also be typed:
  - **Size** (MB) or **ratio** (x of the source);
  - **Quality** (dB), the smallest package that reaches it;
  - **Streaming time**: a network profile (3G, 4G, 5G, or MB/s), the time to the first view and the time to full detail. The first sizes the coarse levels (the two pinned levels must arrive within it), the second the whole package; the handle shows where that lands on the curve.
- Underneath, read-only: the size, the predicted quality (PSNR and SSIM), and the settings the point implies (SH degree, bits per band, deadband). **Override settings** unlocks them for an expert; the curve then shows the overridden point against the hull.
- Bake encodes at the chosen point; the setting is remembered per scan (bake_tunings.json), as the target, not as the derived settings, so a re-tuned search still honours it.

```
Compression                         [Analyse]
 quality (dB)
 44 |                  .----*
 40 |          .--o----'
 36 |      .--'   ^ chosen
 32 |   .-'
    +----------------------------  package (MB)
     100   150   200   250
 Target: (o) size [165 MB]  ( ) ratio [4.8x]  ( ) quality [39 dB]
         ( ) streaming: [4G v]  first view [2.0] s  full detail [60] s
 -> 165 MB, 39.0 dB, SSIM 0.993   SH degree 3, 4 bits (auto)   [ ] Override settings
```

## How the six parts of RDO map onto GSTx

**1-2. Distortion.** Two measures, one fast for the search and one exact for the check:
- *Search proxy*, per splat: the colour error of its spherical harmonics over a fixed set of view directions (against the source's full-precision SH), plus position, scale and opacity error turned into colour error, each weighted by the splat's visual contribution (opacity x projected area at the level's typical viewing distance). Summed per unit. It needs no rendering, so every candidate of every unit can be scored.
- *Check*, per package: rendered views against a lossless reference, PSNR and SSIM (the `--capture-views` mode added for the Cthulhu sweep, whose own noise floor is about 38 dB at its view distance). Used to calibrate the proxy and to report the result.
- Levels are weighted by how they are seen: a coarse level is only drawn far away, where its error covers few pixels, so its distortion counts less (the level's screen-space weight from the LOD rule).

**3. Rate.** Every chunk level is compressed as its own Brotli stream (records and SH separately), so rates **add up across units with no shared coder context**: one unit's choice never changes another's rate, except through SH prediction between levels (below). The search uses exact bytes from Brotli at a fast quality setting (calibrated against the final quality-11 encode), cached per unit and candidate; side information (headers, manifest) is a fixed cost.

**4. Decision space** (by phase):
| Choice | Phase 1 (whole package) | Phase 2 (per chunk level) | Phase 3 |
|---|---|---|---|
| SH degree (0-3) | yes | yes | |
| SH bits per band (8/6/5/4, per band) | yes | yes | lower depths |
| Deadband (wavelet detail zeroing) | yes | yes | |
| Levels | yes | | |
| Record precision (position, scale, rotation, colour, opacity bits) | | | yes (format) |
| Pruning splats of negligible contribution | | | yes |

**5. Search.** The unit is the chunk level, the package's natural unit of coding and of streaming.
- Phase 1: a small grid over the whole-package options (about 50 combinations), each scored by the proxy and by exact rate on a sample of units, then the best for the target encoded in full.
- Phase 2: for a given lambda each unit takes the candidate minimising D + lambda R; a bisection on lambda meets the target. SH residual coding predicts a level's harmonics from the next coarser level, so a coarse unit's choice changes its children's rate: units are decided coarse to fine, each given its parent's decision (greedy along the chain; no trellis needed at five levels).

**6. Constraints.** The coarse levels stream first, so their size is capped (first frame fast on a slow link). GPU memory follows the record and SH sizes, so a GPU budget can be a second constraint. Decoding stays deterministic and integer-exact (the SH residual path).

## Components touched
| Area | Files | Change |
|---|---|---|
| Library (private) | `libs/gstx/Rdo.h/.cpp` (new) | Distortion proxy, rate measurement with the per-unit cache, the phase 1 grid search, the phase 2 lambda search. |
| Open stand-in | `scripts/public-release-stubs/libs/gstx/Rdo.cpp` | Phase 1 only (the grid search is not proprietary). |
| App | `SplatPakApp` Package Bake tab | Target control (size, ratio or quality); the result's size and predicted quality; remembered per scan. |
| Format (phase 2) | `WaveletTypes.h`, `StreamPackager.h`, spec | Format 15: SH degree and per-band bits per chunk level in the manifest; the SH stream addressed per chunk with its own stride. |
| Renderers (phase 2) | `Splat.hlsl`, the UE plugin | Mixed SH degrees: each meshlet carries its SH offset and degree. |
| Tests | `tests/TestRdo.cpp` (new) | On Cthulhu and Venus: phase 1 meets a size target within 2% and is no worse than the hand sweep at that size; the proxy ranks candidates as the rendered check does. |

## Decisions to make
- **How the target is given**: decided (user, 2026-10-26): the curve with a handle, typed size, ratio or quality, and streaming time as a second way to set it.
- **Format 15 for phase 2**: per-chunk choices need it; the UE plugin follows.
- **Pruning (phase 3)** removes splats: the package no longer carries the whole scan. (Proposed: off unless the target cannot be met otherwise.)
