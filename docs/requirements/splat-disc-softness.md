# Splat disc softness

Status: agreed
Date: 2026-09-07
Owner: Dave Wilkinson

## Status, 2026-09-07 late
With the Gaussian level 0 switched off, the checkbox is greyed out and its saved value still
applies to every disc level; criteria 4 and 5 do not apply while it is off.

## Revision, 2026-09-07 evening
Measured on the bundled Cthulhu: level 0 median footprint 0.00027, spacing 0.00051, ratio 0.53,
disc radius factor 1.50, exactly the clamp floor. The softness rule therefore changes no disc size
on the bundled package; only the opacity floor takes effect there. The floor applies to discs of
level 1 and up, which are now the only discs: level 0 never draws discs (see
splat-transition-shimmer, revision note), so the "level 0 discs keep raw opacity" clause is moot. Open question for the owner: the discs already match
the median level 0 Gaussian's width, so speckle at the coarse levels is not a width mismatch; the
remaining levers are a higher radius floor, the opacity floor, or per-record sizing, and which one
to try needs a decision before criteria 3 and 4 can be judged on this package.

## Goal
Reduce the speckle of the disc-rendered levels in splat mode, and reduce the perceivable difference
when a level 0 chunk hands over from discs to Gaussians, so that a streamed model reads as one
continuous surface throughout its refill. Also make the refinement visualizer's arrival glow work
in splat mode, where it has never been drawn. Builds on splat-coarse-level-billboards and
splat-transition-shimmer.

## User-visible behaviour
- A **Match Level 0 Softness** checkbox in the Renderer tab, splat mode only, on by default,
  persisted in `config.json` as `splat_disc_match_level0`.
- When on, the disc radius is no longer the fixed 1.5 times the level spacing. It is derived per
  package from the level 0 Gaussians themselves: the median footprint of the level 0 records
  relative to the level 0 point spacing gives a softness ratio, and every disc level takes the
  radius that gives its Gaussian-profile discs the same ratio to their own spacing. The derived
  ratio and the resulting radius factor are shown read-only next to the checkbox, and the factor is
  clamped to a sane range so a package with unusual scales cannot produce giant or vanishing discs.
- When on, discs of level 1 and above also take an opacity floor so that a record with low opacity
  does not read as a hole in an otherwise solid surface. Level 0 discs (chunks mid cross-fade or
  mid fade-in) keep their raw opacity so they match the Gaussians they are about to become. Level 0
  fully resident Gaussians are not affected.
- When off, the discs behave as today: 1.5 times spacing, the record's opacity as is.
- The hand-over from discs to Gaussians (the fade-in from splat-transition-shimmer) is unchanged in
  mechanism; with the checkbox on, the two ends of that fade look closer, so the fade is less
  visible.
- **Arrival glow in splat mode.** With Show Streaming Arrivals on, newly delivered chunks in splat
  mode flash and settle with the same orange tint, hue and intensity controls as the disc modes,
  on Gaussians and discs alike. With it off, or for chunks that were not just delivered, the splat
  colour is exactly as today.
- No screen-space blur pass. A post-filter would soften the rim back into the background and
  cost a second render target and two full-resolution passes; the same softness is obtained by
  widening the discs, whose Gaussian profile makes the two equivalent.

## Non-goals
- Any change to level 0 fully resident rendering.
- A blur or filter pass on the frame.
- Per-splat disc sizes from each record's own scale (a flat Gaussian seen edge-on would become a
  fat disc poking past the rim).
- Changes to the bake, the package format or the codec library.

## Constraints
- Render paths: mesh, VS SM6, VS SM5 identical images.
- Modes: Studio and Viewer identical.
- Package compatibility: derived from data already in every v8 splat package; nothing added.
- Performance: fill cost of the disc levels grows with the square of the radius factor. With the
  clamp the main splat dispatch at a forced level 1 view must stay within 1.5 times the current
  build's time at the same view. The level 0 fully resident view is unchanged.
- The reference Gaussian math stays untouched; the glow is an additive tint taken only when the
  visualizer is on and the chunk has just arrived.

## Public / private placement
Entirely public app code in `src/SurfelsCore` (record statistics at load, two constant-buffer
values, the disc alpha rule, the splat-mode glow, one checkbox and config key). Nothing in
`libs/bluesec-codec`.

## Acceptance criteria
1. `Surfels_DX12`, `SplatLab` and the test executables build in Release for all three render paths.
2. The Renderer tab shows the checkbox in splat mode only, with the derived ratio and radius
   factor displayed; toggling it persists across a restart via `config.json`.
3. Speckle: bundled Cthulhu, Viewer, temporal filter off, preview level forced to 1, background
   set to a contrasting clear colour. With the checkbox on, the fraction of pixels inside the
   model's level 0 silhouette whose colour is within 10% of the clear colour is at most one third of
   the fraction with the checkbox off, and below 1% in absolute terms.
4. Hand-over: with the streaming simulation on at the 5G preset, dithered transitions on, camera
   close so level 0 is selected, evict residency, then pause streaming at the first frame in which
   a level 0 chunk begins its Gaussian fade-in and again when that fade has completed. The mean
   absolute pixel difference between the two frames over the model's pixels, with the checkbox on,
   is at least 30% lower than with it off.
5. Level 0 fully resident view (streaming simulation off) is pixel-identical with the checkbox on
   and off, and identical to the current build.
6. Criteria 3 and 4 hold on `--render-path mesh`, `vs6` and `vs5`, and the level 1 view matches
   numerically across the three paths.
7. Main splat dispatch time at the forced level 1 view with the checkbox on is at most 1.5 times
   the current build's at the same view.
8. The trace logs one line at package load with the derived softness ratio and radius factor.
9. Arrival glow: streaming simulation on, Show Streaming Arrivals on, evict residency; a screenshot
   during the refill shows orange-tinted chunks on the splat model, and the Hue and Intensity
   sliders visibly change them. With Show Streaming Arrivals off the same view matches the current
   build.

## Open questions
- None. The opacity floor value, the clamp range and the exact footprint statistic are recorded in
  the design document.
