# Splat transition shimmer

Status: agreed
Date: 2026-09-07
Owner: Dave Wilkinson

This is the public summary; implementation detail is kept in the private repository.

## Status, 2026-09-07 late: Gaussian level 0 hand-over switched off
The special hand-over into a Gaussian level 0 (see the revision below) is set aside for now. Every
level of a splat package refines with the ordinary dithered transitions. Refinement pacing
(criteria 2, 3, 5, 9 and 10) is unaffected; criteria 4, 6 and 7 do not apply while the hand-over
is off.

## Revision, 2026-09-07 evening
The level 0 hand-over was changed from a per-block dither between representations to an opacity
cross-fade: the level 1 blocks fade out while their level 0 children fade in, over the Dither
Duration, with no pixel losing coverage. Level 0 is only handed over once the part of it in view
has finished streaming, so it grows in as one rather than sweeping across the model. Criterion 4 is
read accordingly: no block switches representation in a single frame.

## Goal
When an evicted model refills at high bandwidth (5G preset, camera close enough that the finest
level is selected), levels 1 and 0 must refine with the same clean, gradual dither that the coarse
levels show, and the finest level must ease in rather than snap. Two causes are addressed:

- a safety valve that completes every cross-fade at once when too many blocks are mid-fade, which
  fired during normal fast streaming and made whole regions snap;
- a single-frame change of representation when a level 0 block finished its fade.

## User-visible behaviour
- **Refinement pacing (all render modes).** Refinement is paced so that the number of blocks
  mid-fade stays bounded and the safety valve does not fire in normal streaming. A refinement that
  has to wait keeps showing the parent, which is already on screen, and starts a little later. Data
  keeps arriving meanwhile; only the visual hand-over is paced. The safety valve stays as a last
  resort.
- **Streaming tab.** The transition statistics gain one line: the number of refinement starts
  deferred by the pacing this frame.
- **Refinement Visualizer.** The arrival glow starts when a block is first drawn, not when it is
  delivered, so blocks whose start was deferred still glow.
- **Finest level ease-in (splat mode only).** Level 0 eases in over the Dither Duration instead of
  snapping. Blocks resident at load (streaming simulation off) show the finest level at once, with
  no fade.
- No new controls, no config keys, no package format change. The Dither Duration slider on the
  Streaming tab governs every fade.

## Non-goals
- Changing the streaming order, priorities, bandwidth model or eviction.
- Removing the safety valve.
- Changing how a fully resident finest level looks.

## Constraints
- Render paths: mesh shaders, vertex shaders (SM6) and vertex shaders (SM5) must produce the same
  image.
- Studio and Viewer behave identically; the pacing also applies to disc rendering modes.
- No effect on package compatibility.
- Performance: a block mid-fade keeps both parent and children on the GPU, so the pacing must keep
  that population bounded. A sustained refill must run without a device removal.
- The reference Gaussian maths is unchanged once a fade completes.

## Public / private placement
The pacing and the shader changes live in the viewer. The streaming order and delivery model are
not touched.

## Acceptance criteria
1. The app and the test executables build in Release for all three render paths.
2. Repro: Viewer, bundled Cthulhu package, streaming simulation on with the 5G preset, dithered
   transitions on, camera close enough that level 0 is selected, then evict residency. During the
   refill the safety valve never fires, and the deferred-starts line on the Streaming tab is
   non-zero at some point during the two finest levels.
3. In a frame sequence of that refill, levels 1 and 0 refine by the same gradual dithered
   cross-fade as the coarse levels; no region snaps from one level to the next, and there is no
   flashing.
4. In the same sequence, the finest level eases in over the Dither Duration; no block switches
   representation in a single frame.
5. The refill does not take noticeably longer than before the change at the same settings.
6. After the refill settles, the level 0 view is pixel-identical to the same camera with the
   streaming simulation off.
7. With the streaming simulation off, level 0 appears immediately at start-up, pixel-identical to
   the previous build.
8. Criteria 3 and 4 hold on the mesh, VS SM6 and VS SM5 paths, and a level 1 view and a settled
   level 0 view match across the three paths.
9. A sustained refill repro runs on the mesh path with the D3D12 debug layer clean and no device
   removal.
10. Disc rendering modes (streaming simulation on, 5G, evict) also refill without snapped
    transitions.

## Open questions
- None.
