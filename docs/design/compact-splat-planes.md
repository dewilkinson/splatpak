# Design: compact splat planes

Requirements: the user, 2026-10-26: "find a compact representation for the numbers that is still randomly accessible",
for "saving on io time and access/cache"; "can we benefit from fp16, fixed point etc."
Status: agreed (user, 2026-10-26: "yes"); steps 1 (positions) and 2 (shape and colour) measured
Date: 2026-10-26
Related: [docs/perf/ply-loader.md](../perf/ply-loader.md) (the load times, step by step)

## Summary

A scan's splats are held as fixed-size planes, one per kind of field, each field in the representation that suits its
range: fixed point for positions, rotation and opacity, fp16 for colour and log-scales, block floating point for the
harmonics. Every plane has a fixed stride, so splat `i` is at a computed offset in each: random access, no
decompression. About 77 bytes per splat with 8-bit harmonics (120 with fp16 harmonics) against 248 in a 3DGS .ply and 260
in today's memory layout (`SplatPoint` + `SplatAttributes`). The hot data, positions, is 8 bytes per splat.

Rule throughout: **store compact, compute in fp32.** Values are decoded on read, the arithmetic is fp32, and they are
quantised once, on write. Nothing accumulates in fp16 or fixed point.

## The planes

| Plane | Contents | Encoding | Bytes |
|---|---|---|---|
| Positions | x, y, z | Fixed point, 21 bits per axis over the scan's bounds, packed in one `uint64` | 8 |
| Shape | log-scale x 3, opacity, rotation | log-scales fp16; opacity logit fp16; rotation smallest-three 3 x 16 bits + 2-bit index (in a `uint64`) | 6 + 2 + 8 = 16 |
| Colour | DC x 3 | 16-bit unorm | 6 |
| Harmonics | 45 coefficients | 8 bits each against a per-splat fp16 scale (the package's own precision); or fp16 | 47 (or 90) |

(The rotation is 16 bits per kept component, finer than the package's 10:10:10, and sits in a `uint64` with its index so
the record is an aligned 16 bytes; the earlier estimate of 71 bytes used 4.)

### Why each encoding

- **Positions, 21-bit fixed point.** fp16's precision is relative (at 10 m from the origin its step is about 8 mm, and
  it varies across the scene); fixed point is uniform over the bounds: 1 km / 2^21 = 0.48 mm, a 2 m statue 1 um. Three
  21-bit values fill 63 bits of a `uint64`. For scenes larger than a few km a per-block origin restores precision locally.
- **Log-scales, fp16.** The .ply already stores the log; fp16 keeps about three significant digits of it over any range.
  The package uses 8-bit log2 over the scan's range, so this is finer than anything downstream needs.
- **Opacity, fp16 logit** (the .ply's own form). Changed from 16-bit unorm after step 2's bake test: trained scans hold
  many opacities within 8e-6 of 1, unorm16 rounds them to exactly 1, and the merge raises the transmittance
  `1 - opacity` to a power, so 10% of Cthulhu's level 1 Gaussians moved a package step or more. The logit keeps both the
  opacity near 0 and the transmittance near 1 to about 0.2-0.7%.
- **Rotation, smallest-three.** A unit quaternion's largest component is recomputed from the other three; 16 bits each
  is finer than the package's 10.
- **Colour, 16-bit unorm.** The DC colour is clamped to [0, 1] on load, where unorm16's step (1.5e-5) is finer than
  fp16's near 1 (4.9e-4). The package uses 8 bits per channel.
- **Harmonics, block floating point.** Small values whose dynamic range varies per splat: one fp16 scale per splat and
  8 bits per coefficient is exactly what the package stores (`SplatCodec`), so the encoder loses nothing. fp16 per
  coefficient is the alternative if 8 bits proves tight; bf16 has too little mantissa (7 bits) for these magnitudes.

## What it buys

## Risks

- **Merges:** the coarse levels are averages of the quantised inputs. They are computed in fp32 and quantised once, so
  the error does not compound, but they can differ from today's by the input quantisation.
- **Repeated rounding:** the planes are the single truth; derived data is never written back into them.
- **Very large scenes:** 21 bits over 10 km is 5 mm; the per-block origin is the remedy if needed.

## Staging and measurement

Each step is measured (error against fp32, the time and bytes of the passes that use it) and its load times go into
[docs/perf/ply-loader.md](../perf/ply-loader.md).

1. **Positions** (`src/SplatCore/SplatPlanes.h`): encode and decode, `tests/TestSplatPlanes.cpp` on the bundled scans
   and the bicycle: round-trip error (max and RMS, in mm and as a fraction of the bounds), plane bytes, and the time of
   a bounds pass and a Morton pass over the plane against the same passes over `SplatPoint` arrays. Morton both ways:
   the runtime's float curve from decoded positions, and the integer curve.
2. **Shape and colour** (measured 2026-10-26, [docs/perf/ply-loader.md](../perf/ply-loader.md)): round-trip errors
   far below the package's steps; the bake fed from the decoded planes against the fp32 bake, Gaussian by Gaussian: none
   out by more than a package step at level 0, 0.003-0.019% at level 1, 0.06-0.36% at level 5 (the merge's
   keep-or-merge test flipping near its threshold). With the position plane as well, the decomposition re-partners
   0.3-50% of the points (its Morton order is tie-broken by input index and paired by parity; see the perf note), so
   the bake cannot read decoded positions until its order is defined from the plane: a 63-bit Morton code from the
   21-bit axes, measured with the integer-curve step.
3. **Harmonics:** 8-bit block floating point against fp16, by the same PSNR comparison.
4. **On-disk cache:** the planes written after the first open, keyed by the content hash (computed in the same pass as
   the load); reloads read the planes. Cold and warm times.
5. **The bake reads the planes directly** (LiftingWavelet, SplatCodec, SpatialOctree): the cache win. Agreed separately.
