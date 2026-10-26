# Design: colour decorrelation before compression (YCoCg)

Status: implemented
Date: 2026-10-25
Requirements: [gstx-colour-decorrelation](../requirements/gstx-colour-decorrelation.md)

## Summary
A writer turns the RGB bytes of every splat record (bytes 10–12) and of every SH coefficient (each R, G, B triple in
the SH entry) into reversible YCoCg-R, computed in 8-bit modular arithmetic, before the byte shuffle. A reader undoes
it after the unshuffle. The header's padding word at offset 204 becomes `payloadTransforms`, a bit set; version 11.
The figures shown come from the bake's size-estimate sample (entropy and Brotli size with and without the transform)
and, during a save, from the full payloads (entropy only, which is cheap).

## Files

| Area | Files | Change |
|---|---|---|
| Transform | `libs/gstx/ColourTransform.h` (new, open, header-only) | `Forward`/`Inverse` on one triple; `ApplyRecords`/`UndoRecords` (stride 20, offset 10); `ApplySH`/`UndoSH` (every triple of an entry); `PlaneEntropy` helper |
| Format | `src/SplatCore/WaveletTypes.h`, `docs/GSTX_FORMAT_SPECIFICATION.md` | Version 11; `payloadTransforms` at offset 204 (bit 0 records, bit 1 SH); spec clause 8.1 |
| Writer | `src/SplatCore/StreamPackager.h` | Apply before the shuffle when writing; set the header bits; entropy before/after for the save line |
| Readers | `src/SplatCore/StreamPackager.h` (load), `libs/gstx/PayloadDecode.*`, streaming runtime decode path | Undo after the unshuffle when the header bit is set |
| Estimate and UI | `src/SplatCore/SplatPakApp.cpp/.h` | The size estimate runs both variants on its sample; `BakeResult` carries the figures; Statistics row; save stage text |
| Tools | `tools/CompressVenus`, `tests/TestSplatCodec.cpp`, `tests/TestLoadPackage.cpp` | `--no-colour-transform` flag; byte-identical round trip check |
| UE5Splats | `Plugins/SplatPak/Source/SplatPakRendering/Private/SplatPackageCore.cpp`, its tests; ThirdParty GSTx headers via `BuildCodec.ps1` | Read version 11 and undo the transform |

## The transform
For one triple (r, g, b) of unsigned bytes, all arithmetic modulo 256, and `h(v)` the arithmetic right shift by one
of `v` taken as a signed byte (floor of v / 2):

```
forward:  co = r − b;   t = b + h(co);   cg = g − t;   y = t + h(cg);   store (y, co, cg)
inverse:  t = y − h(cg);   g = cg + t;   b = t − h(co);   r = b + co
```

Each step adds a function of values the other side already has, so the inverse is exact for every input, including
SH bytes, which are signed. The SH entry stores coefficient k's channels at bytes 3k, 3k+1, 3k+2 (Clause 9.1), so
every triple is transformed in place; the record's colour is bytes 10, 11, 12.

## Format (version 11)
- `payloadTransforms` (u32) at offset 204, formerly padding that writers set to 0. Bit 0: record colour is YCoCg-R.
  Bit 1: SH triples are YCoCg-R. Other bits are 0. A reader that meets an unknown bit rejects the package.
- Version 10 and older: the field reads 0, so the same reader code handles them with no version branch beyond
  accepting 11. The header stays 208 bytes.
- The shadow mesh design moves to version 12 (header 208 → 224 as designed).

## The figures
- Bake: the size estimate already encodes every level and compresses a fixed sample of each class. It now measures
  the sample's mean plane entropy and Brotli size twice, without and with the transform, and the package estimate
  uses the transformed size. Cost: one extra compression of the sample (the estimate takes about 4 s on Cthulhu, of
  which the compression is under half).
- Save: the full payloads' plane entropy before and after, computed while encoding (one histogram pass per
  payload); no second compression.
- Shown as "YCoCg: entropy −x% · size −y% (sampled)" in Statistics, and "entropy −x%" in the save stage.

## Order of work
1. `ColourTransform.h` with a unit test of the inverse over all 2²⁴ triples. Done when: exhaustive round trip.
2. Format and writer and reader in StreamPackager; CompressVenus flag. Done when: criteria 1, 2, 3 (SplatPak side).
3. PayloadDecode and the streaming runtime path. Done when: stream test and calibration runs match.
4. Estimate figures, Statistics row, save stage, trace. Done when: criteria 4, 5.
5. Spec clause 8.1 and the header table. Done when: criterion 8.
6. UE5Splats: headers via BuildCodec, SplatPackageCore, tests, RenderCheck; Fab staging refreshed. Done when:
   criterion 6.
7. Regenerate the bundled packages; measure load and bake time. Done when: criterion 7.
