# SFLW Package Format Specification

**Surfels Wavelet Stream Package (`.sflw`), Format Version 8**

Document status: Approved. Supersedes the description of format versions 1 through 7 that lived in code comments. This document is maintained with the format: every change to a structure, a field or a version number is reflected here in the same change set.

---

## 1. Overview

### 1.1 Scope

This specification defines the binary layout and the semantics of the `.sflw` container: the file that SplatLab writes when a point cloud or a 3D Gaussian Splatting scene is baked, and that SplatLab, the Surfels viewer and the console tools read. It covers:

- the file header and its version-dependent extent;
- the chunk manifest and the per-chunk level-of-detail (LOD) table;
- the two per-point record formats (the 8-byte oriented-disc surfel and the 20-byte 3D Gaussian);
- the spherical-harmonics stream of splat-mode packages;
- the optional interior occlusion volume and its mip table;
- the optional detail grid;
- conformance requirements for readers and writers.

### 1.2 Purpose

The container carries a multi-resolution, chunked, compressed representation of a point set so that a viewer can stream and draw any part of the model at any level of detail without decoding the whole file. Every offset in the file is absolute, so a reader may seek to any chunk level directly.

### 1.3 Exclusions

The entropy stage applied to each payload (Clause 8) is identified but its bit-level encoding is not specified here; it is defined by the codec library the package was written with. The algorithms that produce the coarser levels, the occlusion volume, the detail grid scores and the streaming order are likewise out of scope. Two implementations that follow this document interoperate at the container level; payload interchange additionally requires the same codec build.

## 2. Normative references

- IEEE Std 754-2019, *IEEE Standard for Floating-Point Arithmetic* (binary32 and binary16 encodings).
- Kerbl, Kopanas, Leimkühler, Drettakis, *3D Gaussian Splatting for Real-Time Radiance Field Rendering*, ACM TOG 42(4), 2023 (the spherical-harmonics basis and coefficient order, Clause 9.3).
- DirectXMath `XMFLOAT3`/`XMFLOAT4` component order (x, y, z, w) as used by the reference implementation.

## 3. Definitions, acronyms and abbreviations

- **AABB**: axis-aligned bounding box, stored as a minimum and a maximum corner.
- **chunk**: a spatial partition of the point set with its own LOD pyramid.
- **level, LOD**: one resolution of a chunk; level 0 is the finest.
- **record**: the fixed-size encoding of one point at one level (Clause 7).
- **surfel**: a point rendered as an oriented disc (record format 0).
- **splat**: a point rendered as a 3D Gaussian (record format 1).
- **SH**: spherical harmonics, the view-dependent colour terms of a splat.
- **payload**: the compressed byte string holding one level's records, or one level's SH stream.
- **DC term**: the view-independent (band 0) colour of a splat.
- **unorm(n)**: an unsigned integer of *n* bits representing the value *code / (2ⁿ − 1)* in [0, 1].

The key words **shall**, **shall not**, **should**, **should not** and **may** are to be interpreted as requirements, recommendations and permissions respectively.

## 4. Conventions

### 4.1 Byte order and alignment

All multi-byte integers and floating-point values are little-endian. Structures are laid out without padding except where stated; the layouts in this document give the byte offset of every field so that no assumption about compiler padding is required.

### 4.2 Types

| Name | Size | Meaning |
|---|---|---|
| `u8`, `u16`, `u32`, `u64` | 1, 2, 4, 8 | unsigned integer |
| `i8` | 1 | two's-complement signed integer |
| `f32` | 4 | IEEE 754 binary32 |
| `f64` | 8 | IEEE 754 binary64 |
| `f16` | 2 | IEEE 754 binary16 |
| `float3` | 12 | three `f32`: x, y, z |

### 4.3 Coordinate frame

Positions are in the model's frame with +Y up. A writer importing a 3D Gaussian Splatting scene (whose native frame has +Y down and +Z forward) shall apply a half turn about the X axis to positions, Gaussian rotations and spherical-harmonics coefficients (Clause 9.4) before encoding, so that a reader never needs to know the source convention.

## 5. File structure

A conforming file consists of, in order of appearance:

1. the file header (Clause 6), at offset 0;
2. the payload region: for each chunk in manifest order, for each of its levels finest-first, the level's record payload, immediately followed in splat-mode packages by that level's SH payload;
3. the occlusion voxel array (Clause 10), if present;
4. the detail grid (Clause 11), if present;
5. the SH table (Clause 9.5), if present;
6. the chunk manifest (Clause 6.3), at `manifestOffset`.

A reader shall locate every structure through the offsets in the header and the manifest and shall not rely on the order above, which is informative.

## 6. File header

### 6.1 Layout

The header is a single structure whose extent depends on the format version: each version appended fields to the previous layout without moving earlier fields. A reader shall read at most the number of bytes given in Table 6.2 for the file's version and shall treat every field beyond that extent as absent.

| Offset | Type | Field | Since | Description |
|---:|---|---|:-:|---|
| 0 | `u32` | `magic` | 1 | Shall be `0x574C4653` ("SFLW" as ASCII bytes `53 46 4C 57`). |
| 4 | `u32` | `version` | 1 | Format version, 1 to 8. |
| 8 | `u32` | `numChunks` | 1 | Number of chunks; shall be at least 1. |
| 12 | `u32` | `maxLOD` | 1 | Number of coarser levels requested at bake time (levels per chunk are at most `maxLOD + 1`). |
| 16 | `u64` | `totalSurfelsLOD0` | 1 | Sum of the level-0 record counts over all chunks. |
| 24 | `f64` | `globalOriginX` | 1 | Origin subtracted from the source coordinates (0 when none). |
| 32 | `f64` | `globalOriginY` | 1 | |
| 40 | `f64` | `globalOriginZ` | 1 | |
| 48 | `float3` | `globalBoundsMin` | 1 | Global AABB minimum corner. Position quantisation of every record spans this box. |
| 60 | `float3` | `globalBoundsMax` | 1 | Global AABB maximum corner. |
| 72 | `f32` | `splatRadius` | 2 | Disc radius scale the model was authored with (surfel rendering). |
| 76 | `u32` | `occlusionVoxelCount` | 3 | Number of occlusion voxels; 0 = no volume. |
| 80 | `u64` | `occlusionVoxelOffset` | 3 | Absolute offset of the voxel array (Clause 10); 0 when the count is 0. |
| 88 | `u64` | `manifestOffset` | 4 | Absolute offset of the embedded chunk manifest (Clause 6.3). |
| 96 | `u64` | `sourceFileBytes` | 5 | Size in bytes of the source file the package was baked from; 0 = unknown. |
| 104 | `u32` | `occlusionMipCount` | 6 | Number of mips in the voxel array, 0 to 8 (Clause 10.2). |
| 108 | `u32[8]` | `occlusionMipBlockCount` | 6 | Voxels per mip; the sum shall equal `occlusionVoxelCount`. |
| 140 | `f32[8]` | `occlusionMipCellSize` | 6 | Finest cube edge of each mip. |
| 172 | `u32[3]` | `detailGridDims` | 7 | Detail grid resolution (x, y, z); all 0 = no grid. |
| 184 | `f32` | `detailGridCellSize` | 7 | Cell edge in model units; the grid's origin is `globalBoundsMin`. |
| 188 | `u64` | `detailGridOffset` | 7 | Absolute offset of the detail grid (Clause 11). |
| 196 | `u32` | `surfelFormat` | 8 | Record format of every level payload: 0 = surfel (Clause 7.1), 1 = splat (Clause 7.2). |
| 200 | `u32` | `shDegree` | 8 | Spherical-harmonics degree kept in the SH stream: 0, 1, 2 or 3. Shall be 0 when `surfelFormat` is 0. |
| 204 | `u32` | `splatRecordBytes` | 8 | Bytes per record of the declared format: 8 for format 0, 20 for format 1. |
| 208 | `u32` | `shRecordBytes` | 8 | Bytes per point in the SH stream: `3 × ((shDegree + 1)² − 1)`; 0 when `shDegree` is 0. |
| 212 | `f32` | `splatScaleLog2Min` | 8 | log₂ of the scale that a scale byte of 0 decodes to (Clause 7.2). |
| 216 | `f32` | `splatScaleLog2Max` | 8 | log₂ of the scale that a scale byte of 255 decodes to. |
| 220 | `u64` | `shManifestOffset` | 8 | Absolute offset of the SH table (Clause 9.5); 0 = no SH stream. |
| 228 | | *end* | | |

### 6.2 Header extent by version

| Version | Header bytes | Notes |
|:-:|---:|---|
| 1 | 72 | |
| 2 | 76 | adds `splatRadius` |
| 3 | 88 | adds the occlusion voxel count and offset |
| 4 | 96 | adds the embedded manifest; earlier versions keep the manifest in a companion `.json` file (Annex A) |
| 5 | 104 | adds `sourceFileBytes` |
| 6 | 172 | adds the occlusion mip table |
| 7 | 196 | adds the detail grid |
| 8 | 228 | adds the record format, the splat quantisation parameters and the SH table |

A file shorter than the extent of its declared version is truncated and shall be rejected.

### 6.3 Chunk manifest

At `manifestOffset` (version 4 and later) the file holds `numChunks` entries, each a chunk record immediately followed by its LOD headers.

Chunk record (44 bytes):

| Offset | Type | Field | Description |
|---:|---|---|---|
| 0 | `u32` | `chunkId` | Writer-assigned identifier. |
| 4 | `float3` | `aabbMin` | Chunk AABB minimum. |
| 16 | `float3` | `aabbMax` | Chunk AABB maximum. |
| 28 | `float3` | `center` | Bounding-sphere centre. |
| 40 | `f32` | `boundingRadius` | Bounding-sphere radius. |
| 44 | `u32` | `numLODs` | Number of LOD headers that follow; shall be at least 1. |

LOD header (32 bytes), `numLODs` of them, finest level first:

| Offset | Type | Field | Description |
|---:|---|---|---|
| 0 | `u32` | `lodLevel` | Level number; 0 is the finest. |
| 4 | `u32` | `surfelCount` | Records in the payload. |
| 8 | `u32` | `uncompressedByteSize` | Shall equal `surfelCount × splatRecordBytes` (8 for format 0). |
| 12 | `u32` | `compressedByteSize` | Bytes of the payload in the file. |
| 16 | `u64` | `fileOffset` | Absolute offset of the payload. |
| 24 | `f32` | `geometricError` | Positional error bound of this level relative to level 0, in model units. |

A reader shall verify that every payload lies within the file and that `uncompressedByteSize` agrees with the record size of the declared format before decoding any payload.

## 7. Record formats

All records of one file share one format, given by `surfelFormat`. Every record's position is quantised against the global AABB of the header, so records of different chunks and levels decode in one frame:

    x = globalBoundsMin.x + unorm(code_x) × (globalBoundsMax.x − globalBoundsMin.x)

with a small epsilon (1 × 10⁻⁵) added to each extent by the reference writer to keep the maximum corner representable.

### 7.1 Format 0: surfel (8 bytes)

| Bits | Field | Encoding |
|---|---|---|
| word 0, 0–9 | position x | unorm(10) over the global AABB |
| word 0, 10–19 | position y | unorm(10) |
| word 0, 20–29 | position z | unorm(10) |
| word 0, 30–31 | radius class | 0 to 3, a coarse size class the renderer maps to a disc radius multiplier (1, 1.5, 2.5, 4.5) |
| word 1, 0–15 | normal | octahedral mapping, two 8-bit signed components |
| word 1, 16–31 | colour | RGB 5:6:5 |

Word 0 occupies bytes 0–3 and word 1 bytes 4–7, each little-endian.

### 7.2 Format 1: splat (20 bytes)

Five little-endian 32-bit words:

| Word | Bits | Field | Encoding |
|:-:|---|---|---|
| 0 | 0–15 | position x | unorm(16) over the global AABB |
| 0 | 16–31 | position y | unorm(16) |
| 1 | 0–15 | position z | unorm(16) |
| 1 | 16–23 | scale x | unorm(8) over [`splatScaleLog2Min`, `splatScaleLog2Max`] in log₂ space: `scale = 2^(min + code/255 × (max − min))` |
| 1 | 24–31 | scale y | as scale x |
| 2 | 0–7 | scale z | as scale x |
| 2 | 8–15 | opacity | unorm(8), the Gaussian's peak alpha |
| 2 | 16–23 | colour R | unorm(8), DC term in display space (Clause 9.2) |
| 2 | 24–31 | colour G | unorm(8) |
| 3 | 0–7 | colour B | unorm(8) |
| 3 | 8–15 | flags | bit 0: the point has an entry with non-zero coefficients in the SH stream. Other bits shall be 0. |
| 3 | 16–31 | SH scale | `f16`; the factor that multiplies the point's signed SH bytes (Clause 9.1). 0 when flag bit 0 is clear. |
| 4 | 0–1 | dropped component | index (0 = x, 1 = y, 2 = z, 3 = w) of the quaternion component not stored |
| 4 | 2–11 | component a | unorm(10) over [−1/√2, +1/√2] |
| 4 | 12–21 | component b | unorm(10) over [−1/√2, +1/√2] |
| 4 | 22–31 | component c | unorm(10) over [−1/√2, +1/√2] |

The rotation is a unit quaternion (x, y, z, w) stored by the smallest-three method: the largest-magnitude component is dropped, the quaternion having first been negated if that component was negative, and the other three are stored in x, y, z, w order skipping the dropped one. A reader reconstructs the dropped component as `sqrt(max(0, 1 − a² − b² − c²))`.

The Gaussian's covariance is `R · diag(scale²) · Rᵀ`, where `R` is the rotation matrix of the quaternion.

## 8. Payloads

A level's records are laid out contiguously (`surfelCount × splatRecordBytes` bytes), transposed into byte planes (byte *k* of every record forms plane *k*, planes in order) and passed through the codec library's entropy stage to produce the payload of `compressedByteSize` bytes. A reader reverses both steps. The SH stream of a level (Clause 9.5) is treated identically with a stride of `shRecordBytes`.

The entropy stage is codec-defined (Clause 1.3). A payload the linked codec cannot decode shall be reported as such and shall not be silently skipped.

## 9. Spherical harmonics (splat mode)

### 9.1 SH stream

For a level with *N* records and `shRecordBytes = 3 × K` (K = 3, 8 or 15 for degree 1, 2 or 3), the SH stream holds *N* entries of `3 × K` bytes. Entry *i* belongs to record *i* of the same level. Byte `3 × k + c` of an entry is an `i8` holding coefficient *k* (0 ≤ k < K) of colour channel *c* (0 = R, 1 = G, 2 = B). The decoded coefficient is `i8 × shScale` of that record. A record whose flag bit 0 is clear has an all-zero entry and shall be treated as having no view-dependent colour.

### 9.2 Colour model

A splat's colour for a viewing direction **d** (unit vector from the camera to the splat, in the model frame) is

    colour = max(0, DC + Σ_k coeff_k · Y_k(d))

where DC is the record's 8-bit colour and *Y_k* are the real spherical-harmonics basis functions of bands 1 to `shDegree` in the order of Clause 9.3. The DC term is stored already offset and scaled: a writer importing 3DGS coefficients `f_dc` shall store `clamp(0.5 + 0.28209479 × f_dc, 0, 1)`. Colours are display-space values; a renderer that composites in display space uses them unchanged.

### 9.3 Basis order

Coefficient index *k* counts through the bands in the 3D Gaussian Splatting order:

| k | Band | Basis (up to the constant factor of the reference implementation) |
|:-:|:-:|---|
| 0, 1, 2 | 1 | −y, z, −x |
| 3–7 | 2 | xy, yz, 2z² − x² − y², xz, x² − y² |
| 8–14 | 3 | y(3x² − y²), xyz, y(4z² − x² − y²), z(2z² − 3x² − 3y²), x(4z² − x² − y²), z(x² − y²), x(x² − 3y²) |

A writer importing a 3DGS `.ply` shall take `f_rest` in its channel-major order (all coefficients of R, then G, then B) and store them coefficient-major as described in Clause 9.1.

### 9.4 Frame change

The half turn about X of Clause 4.3 maps (x, y, z) to (x, −y, −z). Each basis function of Clause 9.3 is either unchanged or negated by this map; a writer shall negate the coefficients of the functions that change sign, namely k = 0, 1, 3, 6, 8, 10, 11 and 13. The rotation quaternion (x, y, z, w) becomes (w, −z, y, −x).

### 9.5 SH table

At `shManifestOffset` the file holds one entry per LOD header of the manifest, in manifest order (chunk by chunk, level by level):

| Offset | Type | Field | Description |
|---:|---|---|---|
| 0 | `u64` | `fileOffset` | Absolute offset of the level's SH payload. |
| 8 | `u32` | `compressedByteSize` | Payload bytes; 0 = no SH stream for this level. |
| 12 | `u32` | `uncompressedByteSize` | Shall equal `surfelCount × shRecordBytes` when non-zero. |

## 10. Interior occlusion volume

### 10.1 Voxel array

At `occlusionVoxelOffset`, `occlusionVoxelCount` voxels of 20 bytes each:

| Offset | Type | Field | Description |
|---:|---|---|---|
| 0 | `float3` | `center` | Cube centre. |
| 12 | `f32` | `halfSize` | Half the cube edge. |
| 16 | `u32` | `packedColor` | Bits 0–15: RGB 5:6:5 colour. Bits 16–21: exposed-face mask, one bit per face in −Z, +Z, −X, +X, −Y, +Y order. Bit 22: mask-valid flag (clear on files written before the mask existed, meaning all faces exposed). |

### 10.2 Mip table

Version 6 and later describe the array as `occlusionMipCount` consecutive mips, mip 0 first, of `occlusionMipBlockCount[m]` voxels each with finest cube edge `occlusionMipCellSize[m]`. A reader shall treat a version 3 to 5 file, or a table whose counts do not sum to `occlusionVoxelCount`, as a single mip covering the whole array.

## 11. Detail grid

At `detailGridOffset`, `detailGridDims[0] × detailGridDims[1] × detailGridDims[2]` bytes, one `u8` score per cell, x fastest. Cell (i, j, k) covers the box from `globalBoundsMin + (i, j, k) × detailGridCellSize` of edge `detailGridCellSize`. The score's meaning is codec-defined; a reader shall accept any value and a value of 0 denotes an empty cell.

## 12. Conformance

### 12.1 Writers

A conforming writer:

1. shall write `version` 8 and every field of Clause 6.1;
2. shall write `surfelFormat`, `splatRecordBytes`, `shDegree` and `shRecordBytes` consistently with Clauses 6.1 and 7;
3. shall write every payload with the record format declared in the header;
4. shall, for `surfelFormat` 1 with `shDegree` > 0, write the SH table with exactly one entry per LOD header;
5. shall write `splatScaleLog2Max` greater than `splatScaleLog2Min` when `surfelFormat` is 1;
6. should choose the scale range so that no source scale is clamped and the growth of the coarser levels remains representable.

### 12.2 Readers

A conforming reader:

1. shall reject a file whose `magic` differs from Clause 6.1 or whose `version` is 0 or greater than 8;
2. shall read only the header extent of the file's version (Table 6.2) and shall supply the defaults of Annex B for absent fields;
3. shall validate the manifest against the file size and the declared record size before decoding;
4. shall reject a `surfelFormat` it does not implement, and shall treat every pre-version-8 file as `surfelFormat` 0;
5. may ignore the SH stream (drawing the DC colour only) but shall then still validate the SH table;
6. shall report a payload the linked codec cannot decode as an error.

## Annex A (informative): pre-version-4 companion manifest

Versions 1 to 3 kept the chunk manifest in a JSON file next to the package (`<name>.json`) with the same fields as Clause 6.3. Readers of this specification's reference implementation still accept such files.

## Annex B (informative): defaults for absent header fields

| Field | Default when absent |
|---|---|
| `splatRadius` | 1.0 |
| `occlusionVoxelCount`, `occlusionVoxelOffset` | 0 |
| `manifestOffset` | 0 (companion JSON manifest) |
| `sourceFileBytes` | 0 |
| `occlusionMipCount` | 0 (one mip covering the array) |
| `detailGridDims`, `detailGridCellSize`, `detailGridOffset` | 0 (no grid) |
| `surfelFormat` | 0 |
| `shDegree`, `shRecordBytes`, `shManifestOffset` | 0 |
| `splatRecordBytes` | 8 |
| `splatScaleLog2Min`, `splatScaleLog2Max` | 0 (unused for format 0) |

## Annex C (informative): rendering model of format 1

The reference renderer draws a splat package the way the PlayCanvas / SuperSplat viewer draws a 3D Gaussian Splatting scene: the covariance of Clause 7.2 is projected with the perspective Jacobian at the splat's view-space position, dilated by 0.3 pixel on the diagonal, and eigen-decomposed; a quad of half-axes `2 × sqrt(2 × λ)` pixels is emitted, shrunk to where the Gaussian times opacity falls below 1/255; the fragment alpha is `(e^(−4A) − e^(−4)) / (1 − e^(−4)) × opacity` with A the squared distance in quad units; splats are composited far to near with premultiplied alpha, in display space by default.

## Revision history

| Version | Date | Change |
|:-:|---|---|
| 8 | 2026-09-07 | Record format field, the 20-byte splat record, the SH stream and table, splat scale range. This document created. |
| 7 | 2026-09-07 | Detail grid. |
| 6 | 2026-09-05 | Occlusion volume mip table. |
| 5 | 2026 | Source file size. |
| 4 | 2026 | Embedded chunk manifest. |
| 3 | 2026 | Interior occlusion volume. |
| 2 | 2026 | Disc radius scale. |
| 1 | 2026 | Initial format. |
