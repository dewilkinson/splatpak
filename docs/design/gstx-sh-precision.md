# Design: SH Precision

Status: implemented
Date: 2026-10-26
Requirements: [gstx-sh-precision](../requirements/gstx-sh-precision.md)

## Summary
`RequantiseSH` (`src/SplatCore/StreamPackager.h`) snaps each SH coefficient's colour triple to 2^(bits−1) − 1
levels per sign, in YCoCg-R space, and returns the RGB bytes a reader will decode. The packager calls it right after
`SplatCodec::Encode`; the v11 colour transform then stores exactly the snapped Y, Co and Cg values, because the
transform is a bijection. SplatPak calls it at the same point when it builds the resident levels and the size
estimate, so the viewport and the estimate see the package's SH. No format change.

## Why snap in YCoCg space
Snapping in RGB leaves a small alphabet that the colour transform then spreads out again: on Cthulhu at 5 bits the
SH stream grew 13% under YCoCg. Snapping after the transform keeps the alphabet small where the entropy stage sees
it. Measured on Cthulhu (package size against 8 bits; colour error over all view directions, per splat, against
the 8-bit stream, as PSNR with colour in 0..1):

| Bits | Snapped in RGB | Snapped in YCoCg |
|---|---|---|
| 6 | −7.1%, 48.1 dB | −22.8%, 46.3 dB |
| 5 | −24.7%, 42.2 dB | −33.4%, 41.0 dB |
| 4 | −40.7%, 35.6 dB | −44.4%, 34.8 dB |

For reference, the 8-bit stream itself sits at 60.7 dB from the float source. A dead zone (rounding toward zero)
saved 0.5% for 1.6 dB at 4 bits under Brotli and was dropped.

## The wrap guard
Co and Cg are differences, so snapping them can move a decoded channel past the end of the signed byte range,
where 8-bit arithmetic wraps it to the other sign. Without a guard, 4% of coefficients at 5 bits decoded with
errors near the full range (worst splat 0.9). The writer decodes each snapped triple and keeps the exact one
wherever a channel would move by more than 64 (a quarter of the range); the worst splat at 5 bits is then 0.054.

## Files
| Area | Files |
|---|---|
| Requantiser | `src/SplatCore/StreamPackager.h` (`RequantiseSH`, `SplatBakeInput::shBits`) |
| Bake and UI | `src/SplatCore/SplatPakApp.cpp/.h` (`BakeParams::shPrecision`, `BakeSource::shBits`, the combo) |
| Tools and tests | `tests/CompressVenus.cpp` (`--sh-bits`), `tests/TestSplatCodec.cpp` (criterion 2) |
