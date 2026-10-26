# SH residuals: spherical harmonics coded against the coarser level

Status: implemented
Date: 2026-10-26
Owner: Dave Wilkinson

## Goal
Make packages smaller without losing anything, by storing each level's spherical-harmonics (SH) coefficients as the
difference from what the next coarser level predicts, instead of absolutely. The coarser levels keep the SH of real
neighbouring splats, so the prediction is close and the differences compress far better.

## User-visible behaviour
- Nothing to set: every bake writes residuals. The viewport, the decoded colours and the Unreal plugin are unchanged.
- Packages are format 14; older packages are baked again from their source (no format is read but the current one).
- Loading costs a little more time, as each level is restored from its parent.

## Requirements
1. Lossless: a package with residuals decodes to the same bytes as one without, at every SH Precision.
2. Every reader predicts exactly what the writer did, on any compiler: integer arithmetic only.
3. Within a chunk only: chunks stay independent, so streaming and decoding work per chunk, coarsest level first.
4. Records unchanged: the renderer and the Unreal plugin, which draws the DC colour only, read them as before.
5. Load time on Cthulhu grows by less than a second.

## Acceptance
- TestSplatCodec: every level identical to a plain RGB package of the same bake; a 5-bit package identical to the
  preview.
- Cthulhu at 8 bits: at least 15% smaller than without residuals.
