# Streaming runtime in the codec library

Status: agreed
Date: 2026-10-25
Owner: Dave Wilkinson

This is the public summary; implementation detail is kept in the private repository.

## Goal
Move the streaming runtime (the part that decides which parts of a package are loaded, at which level of detail, and how levels hand over) out of the app and into `libs/bluesec-codec`, behind a small API that does not depend on the host. SplatPak and the UE5Splats plugin then run the same streaming, and the runtime's methods ship only as the prebuilt library.

## User-visible behaviour
- SplatPak looks and behaves exactly as before on the Streaming and Renderer tabs: Evict, the network profiles and bandwidth throttle, Conservative / Greedy, decay, Demote Chunks on Zoom Out, the residency graph, the loupe, Detach Camera and the Refinement Visualizer.
- The trace lines that measure streaming keep their form.
- No new UI, keys or config keys. No `.sflw` format change.
- A host (SplatPak, later the UE5 plugin) drives the runtime in five steps: open a package, update it with the camera each frame, perform the reads it asks for, take the frame's render list, and read stats.

## Non-goals
- The UE5 plugin adopting the runtime. That is a follow-up; this change only makes it possible.
- Real asynchronous file reads in SplatPak. SplatPak keeps its simulated network (all data in memory, delivery paced by the throttle). The API must allow a real reader later without change.
- Rendering. Sorting, rasterising, blending, TAA, the occlusion volume draw and the silhouette prepass stay in the host.
- Disabled experimental code in the old streaming path is not carried over.

## Constraints
- Render paths: mesh, VS SM6 and VS SM5 keep working unchanged. The render list the host draws has the same content as before.
- Modes: Studio and Viewer both use the runtime.
- Package compatibility: none affected (no format change).
- Performance: the runtime's per-frame CPU cost stays within 10% of the old in-app streaming cost on the Cthulhu package.
- The library still builds as one static library with the same toolchain rules (MSVC 14.51+ for the prebuilt; open stand-ins otherwise).

## Public / private placement
- Private (`libs/bluesec-codec`, shipped prebuilt in the public tree): the runtime's implementation, that is, how it decides residency, order, level changes and eviction.
- Public:
  - the runtime's header, `libs/bluesec-codec/StreamRuntime.h` (the API and its plain data types);
  - an open stand-in implementation that streams coarse-to-fine in chunk order with one model-wide level and instant level changes;
  - payload decoding (`PayloadDecode`, using Brotli, which moves into the library with its MIT notice).
- App code keeps the UI, the renderer and the host adapter.

## Acceptance criteria
1. `SplatPakApp.cpp` no longer contains the streaming runtime; it calls the runtime's API. Its line count drops by at least 1,500.
2. The library builds with the runtime, both as the real implementation and as the stand-in (`-DBLUESEC_CODEC_USE_PREBUILT=OFF`), and SplatPak links and runs against each.
3. On the Cthulhu package, the `--lod-calibration` report matches the pre-change report: every row's splat count is identical and PSNR is within 0.2 dB.
4. An Evict at 5G with the loupe on the face, recorded before and after the change, shows the same refill: no blocks vanish or pop in during refinement, and the per-second delivery totals by source match within 5%.
5. The Streaming tab's controls all still work: Conservative stops at the displayed level (no deliveries beyond it), and Evict, Re-Stream, Instant Full Load, Pause, decay and ring buffer size behave as before.
6. `TestLoadPackage`, `TestSplatCodec` and `CompressVenus` still pass. A new `TestStreamRuntime` console test drives the runtime without a GPU: it opens Cthulhu, runs a scripted camera for N frames and prints residency per level and the render list size, with identical output between runs.
7. With the stand-in linked, SplatPak opens Cthulhu and streams it to full resolution after Evict.
8. The UE5Splats staging script (`Scripts/BuildCodec.ps1`) points at the library's source checkout and stages the new header.

## Open questions
- None.
