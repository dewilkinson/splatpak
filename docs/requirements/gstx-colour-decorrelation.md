# Colour decorrelation before compression (YCoCg)

Status: implemented
Date: 2026-10-25
Owner: Dave Wilkinson

## Goal
Make `.gstx` packages smaller, losslessly, by decorrelating colour before the entropy stage: each record's RGB
colour and each spherical-harmonics coefficient's RGB triple are stored as reversible YCoCg. On Cthulhu this cut the
SH stream by 22.6% and the records by 2.6% under Brotli (a six-chunk sample, all levels). SplatLab shows how much
the transform lowered the data's entropy and what it actually saved.

## User-visible behaviour
- **Smaller packages, same picture.** Every package saved by SplatLab or CompressVenus applies the transform. The
  decoded model is bit-identical to today's: same records, same SH bytes, same render.
- **Entropy reduction shown.** The Package Bake tab's Statistics (4-Tier Compression Results) gains a row for the
  colour transform with two numbers side by side: the entropy reduction (mean order-0 entropy of the byte planes,
  before vs after) and the size reduction after Brotli. Both come from the bake's existing package-size sample, so
  they appear after Bake without a save. The entropy figure alone can mislead (delta coding cut it by 16% while
  growing the file), so it is never shown without the size figure.
- **Save dialog.** During a save, the compression stage names the transform and shows the entropy reduction
  measured on the full data being written, for example "Compressing the payloads (YCoCg: entropy −26.6%)".
- **Trace.** The bake and the save each write one trace line with both figures, for records and SH separately.
- **Older packages load as before.** Packages from version 10 and earlier carry no transform and open unchanged.

## Non-goals
- Delta coding of records or SH: measured, and it makes Brotli output larger.
- Lossy SH requantisation, SH on the lifting pyramid, band culling and palettes (the SH study's later steps); each
  is its own feature and format version.
- A switch to turn the transform off in the UI. CompressVenus gets a flag for A/B measurement only.
- Point-format packages (`recordFormat` 0): their colour layout differs and they are legacy; unchanged.

## Constraints
- Lossless and exact: the inverse restores every byte; a round trip through save and load matches byte for byte.
- Format: version 11. The transform is declared in the header's existing padding at offset 204, so the header stays
  208 bytes. The shadow mesh moves to version 12.
- Readers: SplatLab and SplatLabViewer (StreamPackager), the streaming runtime, the codec library's PayloadDecode,
  CompressVenus and the tests, and the UE5 plugin's format core all read version 11.
- Decode cost: undoing the transform adds no more than 5% to a package's load time.
- Bake time: computing the figures adds no more than 10% to a bake.

## Public / private placement
YCoCg-R is a published, standard transform: the forward and inverse live in an open header in the codec library,
and the format specification describes it exactly so third-party readers can implement it. No proprietary method is
involved.

## Acceptance criteria
1. A package saved from Cthulhu's `.ply` is at least 12% smaller than one saved with the transform disabled, with the
   same parameters (CompressVenus flag), and both load.
2. Loading that package gives records and SH bytes identical to the untransformed package (TestSplatCodec round trip
   and a byte comparison of every level).
3. The header of a new package says version 11 and declares the record and SH transforms; version 10 packages (the
   bundled ones before regeneration) still open in SplatLab, the viewer and the UE plugin.
4. The Statistics panel shows the transform row with both an entropy and a size figure after a bake.
5. The save dialog's compression stage shows the entropy reduction; the trace has one line per bake and per save
   with record and SH figures.
6. The UE plugin's automation tests pass on a version 11 package, and RenderCheck renders it.
7. Load time of Cthulhu's package rises by no more than 5%; bake time by no more than 10%.
8. The format specification describes version 11 and the transform precisely enough to implement a reader.
