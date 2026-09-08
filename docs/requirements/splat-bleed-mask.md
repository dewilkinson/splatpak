# Splat bleed mask

Status: withdrawn (superseded by splat-coarse-level-billboards on 2026-09-07; implementation parked in git stash "splat bleed mask (withdrawn)")
Date: 2026-09-07
Owner: Dave Wilkinson

## Goal
In splat mode, coarser levels draw larger Gaussians whose soft footprint bleeds past the model's
outer silhouette as a halo against the background. A screen-space mask derived from the packaged
interior occlusion volume attenuates coarse-level splats outside the model boundary, so a model
viewed above level 0 keeps a clean rim.

## User-visible behaviour
- A new **Bleed Mask** group in the Renderer tab, shown only in splat mode and only when the loaded
  package carries an occlusion volume. In splat mode without an occlusion volume the group is
  replaced by a warning: "No occlusion volume: cannot create bleed mask. Rebake with the occlusion
  volume enabled to remove glow artefacts." The same line appears in the on-screen status overlay.
  Two controls only:
  - **Bleed Mask** checkbox. On by default when a splat package with an occlusion volume loads.
  - **Visualize Masked** checkbox: a utility overlay that tints, at 10% opacity, every pixel that
    fails the mask test (pixels outside the dilated silhouette, fading through the feather).
  The mask geometry (dilation as a multiple of the projected occlusion cell size, an extra pixel
  margin, and the feather width) has fixed defaults with no UI. They can be adjusted only through
  `config.json` keys, for datasets whose rim needs a different fit. Masked coverage is removed
  entirely; there is no strength slider.
- Only splats whose chunk is above level 0 are attenuated. Level 0 splats are never touched, so a
  view where every visible chunk is at level 0 renders exactly as before.
- The mask works whether or not **Show Occlusion Volume** is on. It is built from the occlusion
  volume's silhouette, not from the visible occluder cubes.
- The mask is rebuilt every frame the view changes and held when nothing changed. A change is any
  of: camera view or projection (ignoring the temporal-filter sub-pixel jitter), window resize,
  a change of the occlusion volume level in use, a rebuilt occlusion volume, or any Bleed Mask
  control. Streaming chunks in or out does not rebuild the mask.
- The timing panel gains a **Bleed Mask** line showing GPU time for the most recent rebuild and
  whether the current frame rebuilt or held the mask.
- `config.json` gains keys `bleed_mask_enabled`, `bleed_mask_dilation_cells`, `bleed_mask_margin_px`
  and `bleed_mask_feather_px`, saved and loaded alongside `occlusion_shave_bias`. Only the first has
  a UI control.

## Non-goals
- Bleed across interior depth steps (one part of the model in front of another). Only the outer
  silhouette against the background is masked.
- Surfel disc modes (tangent discs and billboards). The controls are hidden outside splat mode.
- Packages without an occlusion volume: rendering is unchanged; only the warning is shown.
- Any change to the `.sflw` format or to the occlusion volume bake.

## Constraints
- Render paths: mesh shaders, vertex-shader SM6 and vertex-shader SM5 must all render the mask with
  the same result. Compute stages must compile for the SM5 fallback.
- Modes (Studio / Viewer): identical behaviour in both. The Renderer tab exists in both.
- Package compatibility: reads the occlusion volume already present in v6+ packages. No format change.
- Performance: a mask rebuild (occluder silhouette draw plus mask construction) must take at most
  0.5 ms of GPU time at 1920x1080 on the development machine. A held frame must cost no more than
  the per-splat mask sample in the splat pixel shader.
- The temporal filter must keep working with the mask on. The mask is sampled at the un-jittered
  pixel position so it does not swim under jitter.
- Occlusion depth culling, display-space and linear-space blending, and the debug Show Occlusion
  Volume Only view keep working with the mask on.

## Public / private placement
Entirely public app code in `src/SurfelsCore` (renderer passes, shaders, UI, config keys). The mask
consumes the occlusion volume as packaged data through the existing renderer interface. Nothing in
`libs/bluesec-codec` changes, and the bake of the occlusion volume is not involved.

## Acceptance criteria
1. `Surfels_DX12`, `SplatLab` and the test executables build in Release for all three render paths.
2. Open the bundled Cthulhu splat package in Viewer mode with the temporal filter off, force the
   preview level to 2 or higher, and take a screenshot with Bleed Mask on and off. With the mask on,
   background pixels farther from the model rim than the dilation plus margin plus feather are the
   clear colour; with it off they show the halo.
3. Same package, every visible chunk at level 0, temporal filter off: screenshots with the mask on
   and off are identical.
4. With Show Occlusion Volume off, the level-2 screenshot with the mask on still shows the halo
   removed.
5. With the camera still for 60 frames, the timing panel reports the mask as held on every frame
   after the first. Orbiting the camera reports a rebuild on every frame.
6. The timing panel's Bleed Mask GPU time for a rebuild is at most 0.5 ms at 1920x1080 on the
   development machine.
7. Criteria 2 and 4 pass with `--render-path mesh`, `--render-path vs6` and `--render-path vs5`,
   and the three screenshots match each other.
8. Visualize Masked tints the pixels outside the mask at 10% opacity; turning it off restores the
   normal frame.
9. Turning Bleed Mask off, closing the app, and reopening it restores it off from `config.json`;
   editing `bleed_mask_margin_px` in the file changes the tinted region in the overlay.
10. In a surfel disc mode the Bleed Mask group is not shown and screenshots are unchanged from
    the previous build.
11. Enabling the temporal filter with the mask on shows no swimming or flicker along the rim in a
    10 second orbit recording or screenshot sequence.
12. Opening a splat package baked without an occlusion volume shows the warning in the Renderer
    tab and the status overlay, and neither Bleed Mask control.

## Open questions
- None at requirements level. Distance-field construction, mask resolution and the dilation
  formula are design decisions for `/architect`.
