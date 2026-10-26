# SH Precision: fewer bits per spherical-harmonics coefficient

Status: implemented
Date: 2026-10-26
Owner: Dave Wilkinson

## Goal
Let a bake trade a little view-dependent colour accuracy for a much smaller package, by storing the
spherical-harmonics (SH) coefficients at fewer than 8 bits. The default stays lossless.

## User-visible behaviour
- **SH Precision** on the Package Bake tab, under SH Quality: 8 bits (lossless, the default), 6, 5 or 4 bits.
  Each entry names its measured size saving on Cthulhu, and the tooltip gives the colour error.
- It is a bake parameter: changing it marks the bake pending, and after Bake the viewport draws exactly the SH the
  package will hold, so the result can be judged before saving. The size estimate follows it.
- Saving writes the precision that was baked. CompressVenus takes `--sh-bits b1,b2,b3`.

## Non-goals
- A new format version: requantised values still occupy their 8-bit slots, so every reader, including the UE
  plugin, opens the packages unchanged.
- Per-band or per-splat precision in the UI, band culling and SH on the lifting pyramid (later steps of
  [the SH study](../private/sh-compression-study.md)).

## Acceptance criteria
1. At 8 bits the package is byte-identical in content to one written before this change.
2. A package written at 5 bits loads to exactly the SH the viewport drew after Bake (TestSplatCodec).
3. On Cthulhu, 6, 5 and 4 bits make the package at least 20%, 30% and 40% smaller than 8 bits, and the colour
   error over all view directions stays at or above 45, 40 and 34 dB.
4. Bake time at 8 bits is unchanged.
