# Bake button and modal progress

Status: agreed
Date: 2026-10-25
Owner: Dave Wilkinson

## Goal
On the Package Bake tab, parameter changes take effect only when the user presses a dedicated **Bake** button, so the
displayed model always matches what was last baked. Every long operation (baking, opening a scan or package,
saving) shows a modal progress dialog with a Cancel button instead of freezing the window.

## User-visible behaviour
- **Bake parameters wait for Bake.** Octree Chunk, Max Wavelet LODs, SH Quality, Deadband Zero, Generate
  Interior Occlusion Volume and Shave change only the settings. The displayed model, its metrics and the
  occlusion volume stay as last baked.
- **The colour grade stays live.** Hue Shift, Saturation and Brightness recolour the existing occlusion volume
  immediately, as today, because a recolour is instant.
- **Bake button.** A full-width **Bake** button on the tab, enabled only while there are unbaked changes, and
  highlighted (with a short line naming the changed parameters) while it is. Pressing it rebuilds the model with
  the current parameters, including the occlusion volume and, once it exists, the shadow mesh.
- **Save waits for Bake.** Save Compressed Package is disabled while there are unbaked changes, with the tooltip
  "Bake your changes first". A package therefore always matches the displayed model.
- **Modal progress dialog.** Baking, opening a `.ply` or `.splat`, opening a `.gstx` package and saving a package
  run without freezing the window. When one runs longer than one second, a modal dialog appears with the
  operation's name, the current stage, a progress bar and **Cancel**. The rest of the UI is blocked while it is
  open; the viewport keeps drawing.
- **Cancel.** Cancelling stops the operation at its next step (within about a second):
  - Bake: the displayed model is unchanged and the parameter changes stay pending.
  - Open: the previously displayed model stays loaded.
  - Save: no package file is left behind (a partial file is removed).
- Operations that finish within a second show no dialog, so quick actions don't flash one.

## Non-goals
- Background work while the user keeps editing: the dialog is modal on purpose.
- Undo or history for parameters.
- Progress for the Streaming tab's actions (Evict, decay), which are already incremental.
- Changing what a bake produces: the same parameters give the same model as today.

## Constraints
- Render paths: mesh, VS SM6 and VS SM5 keep drawing the current model while an operation runs.
- Modes: Studio has the Bake button; Viewer has no bake tab but gets the progress dialog when opening a package.
- Scripted runs: `--lod-calibration`, `--stream-test`, a file passed on the command line, and the console tools
  (CompressVenus and the tests) behave as today and never wait on a dialog.
- Package compatibility: no format change.
- Performance: a bake takes no longer than today's rebuild for the same parameters, within 10%.

## Public / private placement
Public app code only (`src/SplatCore`). The library calls the operations make (wavelet, occlusion volume, codec)
are unchanged; progress is reported between stages and, where a stage already loops over chunks or levels,
between those.

## Acceptance criteria
1. Moving any of the six bake parameters leaves the displayed model unchanged: the render list, Input Model
   Metrics and the occlusion volume stay as they were (checked by a trace line per rebuild, which must not appear).
2. The Bake button is disabled with no pending changes, enabled and highlighted with a line naming the changed
   parameters when there are, and disabled again after a successful bake.
3. After Bake, the model matches today's result for the same parameters: per-level splat counts, chunk count and
   occlusion block count are identical to a rebuild before this change.
4. Hue Shift, Saturation and Brightness update the occlusion volume's colour immediately without Bake.
5. Save is disabled, with the tooltip "Bake your changes first", whenever changes are pending.
6. Baking Cthulhu, opening `cthulu.ply`, opening `cthulu.gstx` and saving a package each show the modal dialog
   with the operation name, stage and a moving progress bar, and the window keeps redrawing (no "Not responding").
7. An operation under one second (for example opening `dwarves.gstx`) shows no dialog.
8. Cancel during a Cthulhu bake returns within about a second with the previous model displayed and the changes
   still pending; Cancel during an open keeps the previous model; Cancel during a save leaves no file.
9. `--lod-calibration`, `--stream-test`, a command-line file and the console tools run unattended exactly as before.
10. SplatPakViewer shows the dialog when opening a large package and has no Bake button.
11. A bake takes no more than 10% longer than today's rebuild for Cthulhu.

## Open questions
- None.
