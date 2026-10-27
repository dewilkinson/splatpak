# Splat coarse-level billboards

Status: agreed
Date: 2026-09-07
Owner: Dave Wilkinson

This is the public summary; implementation detail is kept in the private repository.

## Status, 2026-09-07 late
Level 0 draws as discs too for now (`kSplatDiscMinLevel = 0`), since the Gaussian level 0 is
switched off. The disc rule applies to every level.

## Revision, 2026-09-07 evening (final)
The clause "a level 0 chunk mid cross-fade is drawn as discs" is withdrawn, and so are the
intermediate forms tried the same evening (a per-pixel dither, an opacity ramp with the parent
discs solid, a per-record disc-to-Gaussian blend). The code now does this: level 0 never draws
discs; the disc condition is `lod >= kSplatDiscMinLevel` only. The level 1 to 0 hand-over is an
opacity cross-fade with no dither: the level 1 parent's discs fade out while the level 0
children's Gaussians fade in, and the Gaussian halos grow in as the fade progresses. The level 0
in view fades in together rather than chunk by chunk. See
[splat-transition-shimmer](../design/splat-transition-shimmer.md), revision note. Everything about
levels 1 and up stands.

## Goal
In splat mode, chunks at level 1 and above draw as camera-facing billboard discs sized from the
level's point spacing instead of as full 3D Gaussians, so coarse levels have no Gaussian bloom past
the model's rim. Only level 0 stays Gaussian. This supersedes the splat bleed mask, which is
withdrawn.

## User-visible behaviour
- In splat mode, every splat whose chunk is at level 1 or higher is drawn as a camera-facing disc:
  - Radius: the level's measured median point spacing times 1.5, so neighbouring discs overlap
    and blend into a surface. The record's own scale and rotation are not used.
  - Falloff: the splat mode's bounded Gaussian falloff (the reference viewer's normalised
    falloff, reaching exactly zero at the disc edge) times the record's opacity, so there is no
    rim ring and nothing is drawn outside the radius. (Revised 2026-09-07 from the disc-mode
    exp(-2.5 d^2) x 0.9 rule and a 1.0 radius factor, which read as speckle.)
  - Colour: unchanged from the Gaussian path (DC plus spherical harmonics for the view direction,
    display or linear blending as selected).
  - Sub-pixel discs follow the existing solid-dot rule and become opaque one-pixel dots.
- Level 0 renders exactly as before, full 3D Gaussians with the reference viewer's math. (A clause
  added and withdrawn on 2026-09-07 drew level 0 chunks mid cross-fade as discs; see the revision
  above.)
- The level threshold is fixed at 1. There is no checkbox, slider or config key. The on-screen
  status banner's SPLAT line notes "levels 1+ as discs". (Revised 2026-09-07 from 2: level 1
  Gaussians flickered with glow at the level 1 to 2 boundary.)
- Cross-fades between level 0 (Gaussians) and level 1 (discs) use the existing LOD transition;
  nothing new is shown during a transition.
- Sorting, occlusion depth culling, blending space, temporal filter, Detach Camera and Show Only
  Locked Chunks behave as they do today; a disc is simply a different quad for the same slot.
- The splat bleed mask is withdrawn: no mask passes, no Bleed Mask or Visualize Masked
  checkboxes, no bleed_mask_* config keys, no BleedMaskCS.hlsl.

## Non-goals
- Any change to how level 0 is rendered.
- A user-adjustable threshold, radius multiplier or falloff.
- Normal-oriented discs for splats: splat records carry no normal, so only camera-facing billboards
  apply.
- Splat disc modes (render modes other than splat mode).
- Any change to the `.gstx` format or to the bake.

## Constraints
- Render paths: mesh shaders, vertex-shader SM6 and vertex-shader SM5 must produce the same image.
- Modes (Studio / Viewer): identical behaviour in both.
- Package compatibility: existing splat packages (v8) render with the new rule; no format change.
  The per-level point spacing must be available for splat packages at load, and the existing
  "ComputeLodSpacing" trace line must show a non-zero spacing for every level.
- Performance: at a forced level 1 or 2 view, the main splat dispatch must not be slower than the
  Gaussian rendering of the same view in the previous build; discs are expected to be cheaper.
- The render path for Gaussians copies the reference viewer's math exactly and must not change.

## Public / private placement
Entirely public app code in `src/SplatCore` (shader and renderer). The private codec library does
not change. The opacity already baked into coarse-level records is used as is by the disc alpha.

## Acceptance criteria
1. `SplatPakViewer`, `SplatPak` and the test executables build in Release for all three render paths.
2. The tree contains no bleed mask code: no `BleedMaskCS.hlsl`, no `bleed_mask` string in
   `src/`, `docs/USER_GUIDE.md` or `bin/config.json` after a run, and no Bleed Mask controls in
   the Renderer tab.
3. Bundled Cthulhu splat package, Viewer mode, temporal filter off, preview level forced to 1:
   a screenshot shows the model as discs with no halo past the rim, and the same view at levels 2
   and 3 likewise.
4. Same package, preview level forced to 0 with the streaming simulation off (all of level 0
   loaded): the screenshot is pixel-identical to the previous build at the same view.
5. Criterion 3 passes with `--render-path mesh`, `vs6` and `vs5`, and the three screenshots match
   each other numerically.
6. The trace shows a non-zero ComputeLodSpacing value for every level of the Cthulhu package.
7. With the streaming simulation at a low bandwidth, zooming in from level 1 to level 0 shows a
   smooth cross-fade with no glow shimmer in a screenshot sequence, and zooming out restores the
   discs.
8. At the forced level-1 view, the frame breakdown's main splat dispatch time is not higher than the
   previous build's at the same view.
9. Status banner SPLAT line includes "levels 1+ as discs" in splat mode.
10. At a forced level 1 view zoomed in so discs are tens of pixels across, no disc shows a rim
    ring: the alpha fades smoothly to the background at every disc edge.

## Open questions
- None. The radius rule and falloff were decided by the owner on 2026-09-07; the threshold was
  lowered from 2 to 1 the same day after level 1 Gaussians flickered with glow.
