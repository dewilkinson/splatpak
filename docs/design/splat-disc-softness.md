# Design: Splat disc softness

Requirements: docs/requirements/splat-disc-softness.md
Status: agreed
Date: 2026-09-07

## Status, 2026-09-07 late
With the Gaussian level 0 switched off (`kSplatGaussianLevel0 = false`), the checkbox and its
read-out are greyed out; the saved value still sizes the discs, now for every level, and the
opacity floor applies to every disc. The arrival glow in splat mode stands.

## Revision, 2026-09-07 evening
Measured on the bundled Cthulhu: level 0 median footprint 0.00027, spacing 0.00051, ratio 0.53,
disc radius factor 1.50, exactly the clamp floor. The softness rule therefore changes no disc size
on the bundled package; only the opacity floor takes effect there. The floor applies to discs of
level 1 and up, which are now the only discs: level 0 never draws discs (see
splat-transition-shimmer, revision note), so the "level 0 discs keep raw opacity" clause is moot. Open question for the owner: the discs already match
the median level 0 Gaussian's width, so speckle at the coarse levels is not a width mismatch; the
remaining levers are a higher radius floor, the opacity floor, or per-record sizing, and which one
to try needs a decision before criteria 3 and 4 can be judged on this package.

## Summary
At package load the app measures the level 0 Gaussians' median in-plane footprint, divides it by
the level 0 point spacing to get a softness ratio, and converts it to a disc radius factor
(square root of 8 times the ratio, clamped to 1.5..4.0), because a disc with the reference
falloff of radius R is a Gaussian of sigma R over root 8. With the new Match Level 0 Softness
checkbox on, every disc level uses that factor instead of the fixed 1.5, and discs of level 1 and
up take an opacity floor carried in a new constant-buffer value; level 0 discs keep raw opacity so
the fade-in ends where it starts. The arrival glow that the disc modes compute in their colour
path is factored into a helper and applied in the splat colour path as well, driven by the chunk's
existing wave value, so the refinement visualizer works on Gaussians and discs. Fully resident
level 0 rendering is untouched. This lands after splat-transition-shimmer in the same working tree.

## Components touched
| Area | Files | Change |
|------|-------|--------|
| Softness statistic | `src/SurfelsCore/SurfelsApp.h`, `src/SurfelsCore/SurfelsApp.cpp` | `ComputeSplatSoftness()` after `ComputeLodSpacing()`; members for the ratio and factor; trace line; per-frame radius fill uses the factor when the checkbox is on. |
| Toggle, UI, config | `src/SurfelsCore/SurfelsApp.h`, `src/SurfelsCore/SurfelsApp.cpp` | `m_splatDiscMatchLevel0` (default true); checkbox with read-only ratio and factor text in the splat block of "2. 3D Viewport & Splat Sizing"; `splat_disc_match_level0` load and save; `State::splatDiscOpacityFloor`. |
| Renderer | `src/SurfelsCore/SurfelsRenderer.h`, `src/SurfelsCore/SurfelsRenderer.cpp` | `State::splatDiscOpacityFloor`; `SurfelsCB::splatDiscOpacityFloor` appended with padding; copied in `OnRender`. |
| Shaders | `src/SurfelsCore/Shaders/Surfels.hlsl` | `g_SplatDiscOpacityFloor`; disc alpha rule; `ArrivalGlow()` helper shared by `BuildSplat` and the splat colour path; `SplatFromSlot` passes the chunk wave; `BuildGaussianSplat` gains a `wave` parameter. |
| Docs | `docs/USER_GUIDE.md` | The checkbox, what the ratio means, and that the arrival glow now shows in splat mode. |

## Data flow
```mermaid
flowchart LR
  A[Level 0 records:\nscales decoded] --> B[median sqrt(s_max x s_mid)]
  C[lodSpacing[0]] --> D[ratio = footprint / spacing0]
  B --> D
  D --> E[factor = clamp(sqrt 8 x ratio, 1.5, 4)]
  E --> F{checkbox on?}
  F -->|yes| G[splatDiscRadius[l] = spacing_l x factor\nopacityFloor = 0.5]
  F -->|no| H[splatDiscRadius[l] = spacing_l x 1.5\nopacityFloor = 0]
  G --> I[shader disc branch:\nalpha = lod >= 1 ? max(o, floor) : o]
  J[chunk.isSilhouette = wave] --> K[SplatFromSlot -> BuildGaussianSplat(wave)]
  K --> L[ArrivalGlow(color, wave) after SplatColor\nwhen g_ShowChunkStream and 0 < wave < 0.99]
```

1. **Footprint statistic.** After `ComputeLodSpacing()` in the streaming set-up, if `m_splatMode`
   and level 0 has records: take up to 65536 records of `m_residentLODs[0].splats` at an even
   stride, decode the three scales as the shader does (`exp2(scaleLog2Min + byte / 255 *
   (scaleLog2Max - scaleLog2Min))` with `m_splatParams.scaleLog2Min/Max`), sort them, and take
   `sqrt(s[2] * s[1])` (the two largest, the in-plane footprint of a flat Gaussian). The median of
   those is `m_splatLevel0Footprint`. `m_splatSoftnessRatio = footprint / m_lodSpacing[0]` (0 when
   spacing is 0), `m_splatDiscMatchFactor = clamp(sqrt(8) * ratio, 1.5, 4.0)`. One trace line:
   `SplatSoftness: level 0 median footprint %.5f, spacing %.5f, ratio %.2f, disc radius factor %.2f`.
   Why `sqrt(8)`: the disc alpha is `exp(-4 (r/R)^2)` normalised, so its sigma is `R / sqrt(8)`;
   matching sigma to `ratio * spacing` gives `R = sqrt(8) * ratio * spacing`.
2. **Radius fill.** The existing per-frame loop uses `factor = m_splatDiscMatchLevel0 ?
   m_splatDiscMatchFactor : kSplatDiscCoverage`. Before the statistic exists (no package, or a
   disc package) the factor is `kSplatDiscCoverage`.
3. **Opacity floor.** `State::splatDiscOpacityFloor = m_splatDiscMatchLevel0 ? 0.5f : 0.0f`, copied
   into a new `SurfelsCB` float. In the shader's disc branch: `gs.alpha = (lod >= 1) ?
   max(opacity, g_SplatDiscOpacityFloor) : opacity`. The dual quad's disc alpha (level 0) is raw.
   Why 0.5: coarse records already carry the bake's opacity boost, so the floor only catches the
   sparse low tail that reads as holes; 0.5 is high enough that a lone low record no longer punches
   through and low enough that overlapping neighbours, not the floor, set the surface density.
4. **Arrival glow.** Factor the `else if (g_ShowChunkStream == 1)` block of `BuildSplat` into
   `void ArrivalGlow(inout float3 color, float w, float lighting)` with the same arithmetic, called
   from `BuildSplat` with its `lighting` (unchanged output). `SplatFromSlot` reads
   `chunk.isSilhouette` into `wave` (0 when not chunked) and passes it to `BuildGaussianSplat`,
   which passes it into `SplatColor`. Inside `SplatColor`, after the `max(color, 0)` clamp and
   before the optional `SRGBToLinear` conversion, call `ArrivalGlow(color, wave, 1.0)` when
   `g_ShowChunkStream == 1 && wave > 0.0 && wave < 0.99` (the `>= 0.99` value is the lavender edge
   highlight, which stays disabled). The glow is therefore applied in display space, as the disc
   modes do, and is the same tint for all three representations (Gaussian, disc, dual).
5. **UI and config.** Checkbox in the `if (m_splatMode)` block of "2. 3D Viewport & Splat Sizing"
   after Splat Blending, saving `config.json` on toggle like the hints checkbox; below it
   `TextDisabled("Level 0 softness %.2f x spacing -> disc radius %.2f x spacing", ratio, factor)`
   with a tooltip explaining the derivation in one sentence. Config: `splat_disc_match_level0`
   true/false, parsed like `show_control_hints`, written in the save block.

## Render path impact
- **mesh / VS SM6 / VS SM5:** one more constant-buffer float, one `max`, and the glow helper
  called from the shared builder. Nothing path-specific; the legacy compiler already compiles the
  glow code in `BuildSplat`.

## Public / private placement
All public app code in `src/SurfelsCore` and `docs/`. The statistic reads records already decoded
for rendering; the codec library is not involved.

## Decisions
- **Widen discs rather than blur the frame**: identical softening for a Gaussian-profile disc, no
  second render target, no extra passes, no colour bleeding past the rim. Rejected: a
  coverage-weighted separable blur driven by an accumulated disc mask (about 0.5 ms at 1080p and
  softens the rim by the blur width).
- **Statistic from the two largest scales**: the in-plane footprint is what the viewer sees; the
  smallest scale is the Gaussian's thickness. Median rather than mean so a few huge floaters do not
  inflate the factor.
- **Clamp 1.5..4.0**: below 1.5 the discs would be narrower than today; above 4 fill cost
  (proportional to the square) exceeds the performance constraint on dense views.
- **Floor for level 1 and up only**: level 0 discs exist only to hand over to the same records'
  Gaussians, so their alpha must stay the Gaussians' alpha or the fade-in shows a density step.
- **Glow via the chunk's existing wave value**: the app already writes it into
  `MeshletChunkGPU::isSilhouette` for every mode; no new field.
- **One checkbox governs both radius and floor**: the requirement asks for one toggle; a second
  control for the floor would be a tuning knob the owner has been removing elsewhere.

## Test plan
- Existing test exes unaffected; they must still build.
- App smoke (tester), Viewer, bundled Cthulhu:
  1. Checkbox present in splat mode only, ratio and factor text shown; round trip through
     `config.json` (criterion 2). Trace line at load (criterion 8).
  2. Speckle metric at forced level 1 on a contrasting clear colour, checkbox on vs off (criterion 3).
  3. Hand-over metric: 5G refill, pause at fade start and end, mean difference on vs off (criterion 4).
  4. Level 0 settled view pixel-identical on/off and to the current build (criterion 5).
  5. Three paths at level 1 (criterion 6). Dispatch time at level 1 within 1.5x (criterion 7).
  6. Glow: refill with Show Streaming Arrivals on, screenshot shows tinted chunks; sliders change
     them; off matches the current build (criterion 9).
- No new test exe.

## Risks
- **Fill cost** at the top of the clamp on very dense packages; the constraint's 1.5x bound is the
  check and the clamp ceiling can be lowered.
- **Packages with tiny level 0 scales** (ratio under 0.53) get the 1.5 floor and no change; the
  displayed ratio makes that visible rather than silent.
- **Speckle from opacity variation at level 0 during the fade-in** is deliberately not floored;
  if it reads as holes, the remedy is a floor applied to the dual quad's disc alpha only, a one-line
  change, accepting a small density step at the end of the fade.
- **Glow on Gaussians** brightens semi-transparent splats; with heavy overlap it may look stronger
  than on discs. The Intensity slider already scales it.

## Implementation brief (for the coder agent)
1. `src/SurfelsCore/SurfelsApp.h`: add `bool m_splatDiscMatchLevel0 = true; float m_splatLevel0Footprint = 0.0f; float m_splatSoftnessRatio = 0.0f; float m_splatDiscMatchFactor = 1.5f;` and `void ComputeSplatSoftness();`. Done when: compiles.
2. `src/SurfelsCore/SurfelsApp.cpp`: implement `ComputeSplatSoftness()` per Data flow step 1, call it right after `ComputeLodSpacing()`; reset the three values to 0 / 0 / 1.5 wherever `m_lodSpacing` is cleared. Done when: the trace line appears on loading Cthulhu with a ratio between 0.1 and 3 (criterion 8).
3. `src/SurfelsCore/SurfelsApp.cpp` per-frame fill: use the factor per Data flow step 2 and set `m_state.splatDiscOpacityFloor` per step 3. Done when: toggling the checkbox visibly changes disc size at a forced level 1.
4. `src/SurfelsCore/SurfelsApp.cpp` UI and config per Data flow step 5. Done when: criterion 2.
5. `src/SurfelsCore/SurfelsRenderer.h` / `.cpp`: `State::splatDiscOpacityFloor = 0.0f`; `SurfelsCB::splatDiscOpacityFloor` appended after the last field with padding so the struct stays a multiple of 16 (keep the existing `static_assert`s passing); copy in `OnRender`. Done when: both asserts pass and the constant-buffer layout in `Surfels.hlsl` mirrors it.
6. `src/SurfelsCore/Shaders/Surfels.hlsl`: mirror the CB field; disc alpha rule per step 3; `ArrivalGlow` helper per step 4 with `BuildSplat` calling it (its output must be unchanged: same expressions, same order); `SplatColor` gains a `wave` parameter and applies the glow before the linear conversion; `SplatFromSlot` reads `chunk.isSilhouette` into `wave`; `BuildGaussianSplat` gains `float wave` after `fade` and passes it to `SplatColor`. Done when: all three paths compile and criterion 9's glow shows on the splat model.
7. `docs/USER_GUIDE.md`: describe the checkbox and its read-out in the Renderer tab section, and add "also in Gaussian splat mode" to the Show Streaming Arrivals description. Done when: present.
8. Build `Surfels_DX12`, `SplatLab` and the test targets in Release; smoke the three render paths; run the test exes. Do NOT commit; report `git status --short` and `git diff --stat`.
