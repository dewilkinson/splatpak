# Design: environment scenes

Requirements: the user, 2026-10-26: "SplatPak must be able to navigate and process these environmental scenes"; "the
goal is to recreate the view that is shown in the screenshot with similar color fidelity and detail" (the bicycle's
reference render from the 3DGS project page, `assets/bicycle/bicycle.png`, 1237 x 822).
Status: proposed (user, 2026-10-26: "yes, design doc please"); the training-camera view (part of step 1) is built
Date: 2026-10-26
Related: [compact-splat-planes.md](compact-splat-planes.md) (the 63-bit Morton order), [docs/perf/ply-loader.md](../perf/ply-loader.md)

## Summary

SplatPak was built around objects: a model in empty space, framed by its bounding box, every scale-dependent default a
fraction of that box. A 3DGS capture of a place (Mip-NeRF 360's bicycle, garden, stump; Tanks and Temples) is the
opposite: a few metres of subject at the centre of the training cameras, then ground, trees and background thinning out
for 100 m or more. On the bicycle that one assumption breaks the bake defaults, the occlusion volume, the camera and the
coarse levels. This doc proposes treating such scans as **environments**: detected at import, scaled from their
content rather than their box, opened from a training camera, and organised in distance shells around the core.

## What happens today (bicycle, 5.28 M Gaussians, bounds 120 x 97 x 169 m)

- **Nothing recognisable on open.** The camera frames the box from 229 m: the outside of the background shell, with
  the 2 m bicycle inside it at a few pixels.
- **The occlusion volume hides the splats.** Voxelised over the whole box, the sparse background becomes solid blobs;
  the splats are depth-tested against them (the screenshot of 2026-10-26: grey surfaces, almost no splats).
- **Defaults from the box:** chunk 42 m (21 chunks; the subject in one), deadband 252 mm (coarse levels drop all detail
  under 25 cm), detail grid 3.5 m cells, the decomposition's Morton cells 16 cm (pairs inside a cell come in file
  order: 50% of the points re-partnered by a 0.03% change of codes, see the perf note).
- **The bake stalled** in the SH parent search (a uniform grid over a scene with a dense centre); fixed 2026-10-26 with a
  k-d tree, identical output.
- **The far background rendered as specks** and the sky as black where the reference shows bright sky through the
  trees. Found and fixed 2026-10-26: at level 0 the sky was right; the coarse levels' Gaussian cap (1.5 x the level's
  spacing, measured once for the whole model and so from the core) shrank the sparse background's Gaussians to specks.
  Each block now carries its spacing over its level's median block (StreamChunk::spacingRatio, at least 1, to the
  shader in MeshletChunk.coneCutoff as -1 - ratio) and the cap scales by it. Camera 1 with automatic levels: 16.9 to
  18.8 dB against the reference (level 0: 19.4). Without any cap: 12.7 dB, the merged Gaussians as blobs.
- **The bicycle's frame showed large soft blurs** close up, at 100% residency. Found and fixed 2026-10-26: blocks whose
  first points shared a 10-bit Morton cell (16 cm on this scene) got no children, and a block with no children is drawn
  at its own level for good, so 225k level 2 to 5 Gaussians stayed on screen over level 0. The runtime now shares a
  cell's finer points across such blocks (StreamRuntime.cpp); the render list settles to level 0 alone and the frame is
  as crisp as the reference. Not a package change. Measured on the way: 4x finer positions (a 20 m crop), TAA off and
  Match Level 0 Softness off each changed the bicycle region by under 0.3 dB.

How the bicycle's Gaussians spread from the origin (the training cameras' focus):

| Radius | Share of Gaussians | Median largest axis |
|---|---|---|
| 0-2 m | 11.0% | 11 mm |
| 2-5 m | 18.7% | 6 mm |
| 5-10 m | 28.0% | 18 mm |
| 10-20 m | 25.5% | 26 mm |
| 20-50 m | 15.6% | 35 mm |
| > 50 m | 1.1% | 69 mm |

Half the Gaussians are within 8.8 m, 99% within 52 m, the farthest at 107 m; the Gaussians grow with distance, as the
trainer's own scene contraction makes them. Cthulhu, for contrast: everything within 0.9 m.

## Proposal

### 1. Training cameras and the measurement loop (built)

3DGS releases ship `cameras.json` beside the .ply: every photo's position, camera-to-world rotation and focal lengths.
`src/SplatCore/TrainingCameras.h` reads it (beside the file, the folder above, or `--cameras`) and converts it to model
space (the import's half turn about X). The renderer gained a **pose camera** (eye, direction, up with roll, vertical
field of view from fy) that replaces the orbit; the stream's level choice and culling use the same view
(`StreamCamera::explicitView`). The Renderer tab picks a camera; any orbit input carries on from it.

`--camera-capture <folder> [--camera-list ...] --size WxH` renders each training camera at its own field of view once
the model is resident and writes PNGs. Camera 0 at 1237 x 822 already shows the bicycle, the bench and the path in the
reference's framing and colours. This is the loop every later step is measured with: **PSNR and SSIM against the
reference render** (and against the dataset's photos where available), per camera, before and after each change.

### 2. Detect environments at import

An environment is a scan whose bounds are many times its dense core: the radius holding 99% of the Gaussians over the
radius holding 50% (bicycle: 6.0; an object: about 1.7), or the bounds over the 90% radius. Detected once at import,
shown on the Package/Bake tab, overridable. Everything below switches on it.

### 3. Scale from content, not the box

- **Chunks by count, not size:** split the octree until a node holds at most N Gaussians (and a minimum size), so the
  dense core gets small chunks and the background large ones.
- **Deadband and the coarse-level cap from local spacing** (per block), not from the box or a model-wide spacing.
- **The decomposition's order:** the 63-bit Morton code (compact planes), so ties no longer pair in file order.
- **Detail grid and occlusion voxels** sized from the core.

### 4. Distance shells (macro clusters)

Organise the scene in shells around the core (the trainer's contraction: linear inside a radius, then compressed with
distance). Chunks, levels and the streaming order work in the contracted space: the far background gets coarse cells
and coarse levels by construction, the core keeps full detail, and the stream fills the core first. In practice three
macro clusters (core, mid, far) with their own chunk sizes and level counts, in one package.

### 5. Occlusion volume and shadow mesh for the core only

Built inside the core shell, off by default for environments (the splats are not depth-tested against a volume that
is mostly air). The shadow mesh likewise, for Unreal's shadows and collision near the subject.

### 5a. Culling and streaming order for scenes

The octahedral "seen faces" test and the occlusion volume both assume a viewer outside an object: which sides of the
model face the camera, and a solid interior to hide the far side behind. From inside a scene neither means anything
(user, 2026-10-26: "the octahedral visibility test is useless here too"). For environments:

- **Hierarchical frustum culling with a guard band** on the block hierarchy (the traversal already tests block bounds
  against the frustum): the primary test, with a margin so turning the head does not expose unloaded blocks.
- **Screen-space error** per block (built: per-block levels): the far background costs little by construction.
- **Visibility feedback (preferred; the user, 2026-10-26: "would a visibility buffer help?").** Not a visibility
  buffer for shading (a pixel of blended Gaussians has no single winner to shade from), but for culling and streaming.
  The silhouette pass already renders an item buffer (R32_UINT ids, depth-tested, every splat a ~1 px disc, at reduced
  resolution). It also counts, per block, the pixels the block wins (an InterlockedAdd into a per-block counter
  array), and that small array is read back a frame or two late, not the image. Blocks with visible pixels get
  their children requested first; blocks unseen for a while are eviction candidates; blocks with no pixels last frame
  are skipped outside a guard band, and a newly revealed area shows through its resident coarse parent until its
  children follow. Exact per block rather than a bounds test, and the pass is already paid for. One fix first: the
  item pass draws every splat opaque, so only splats above about half opacity may write it, or a fringe of
  near-transparent Gaussians would hide what is behind.
- **Hierarchical-Z occlusion (fallback):** after each frame, a depth pyramid from the splats (depth written where the
  accumulated opacity passes about a half); the next frame tests each block's bounds against it on the GPU, two-pass to
  avoid popping. No precomputed geometry; adapts to any scene.
- **A residency ring around the viewer** (the user, 2026-10-26: "keep ground tiles around the user fully loaded,
  even the ones behind, so that a quick turn means the data is already there"). What is resident and what is drawn
  become separate decisions. Every block within a radius of the eye stays resident at the level its distance alone
  asks for (as if looking every way at once: full detail near, coarser out), pinned against eviction, Release Unused
  included. For ground scenes the ring is a flattened disc rather than a sphere, since turns are mostly horizontal.
  Drawing stays view-dependent (frustum, visibility feedback), so the ring costs memory, not frame time. Streaming:
  the view first, the ring with a fixed share of the bandwidth (about a third), nearest first, so it fills behind the
  viewer while they look ahead. The ring stretches forward along the fly velocity, and its radius scales with fly
  speed: roughly the reach of a second or two. A memory budget sets the distance-to-level rule (the bicycle: level 0
  within 5 m is 30% of its Gaussians, level 1 to 10 m 28% more; together about 2.3 M Gaussians, about 150 MB at 65 bytes each with degree-3 harmonics).
  Conservative policy counts the ring as needed; Greedy goes on loading beyond it when idle.
- **Built 2026-10-26: screen-space order for every model, and the ring for environments** (the user: "it doesnt seem
  like the bike is getting priority for loading"; "we dont have octahedron faces for scenes"; "can we abandon the
  octahedron view even for simple models now?"). Requests rank by a block's level spacing over its distance, in view
  counting in full, and are delivered strictly in rank order with the whole frame's bandwidth
  (StreamSettings::screenSpaceOrder, on by default). At 5G after an evict: the bicycle at camera 1 shows 2.4-2.6x as
  much level 0 at every second (943k against 384k at 20 s); Cthulhu delivers about 2.6x the view's requests per second.
  Environments also drop the face test and keep a residency ring (StreamSettings::environmentScene,
  residencyRingRadius: the core's half radius, 3 to 30 m). Objects keep the face test as their hidden-side filter until
  visibility feedback lands; then the octahedron goes.
- **Built 2026-10-26: cross-fades admitted by screen-space error, and the subject** (the user: "the refinement
  visualizer sweeps the whole scene and not just the bike"; "detect just the high detailed objects close to the user
  and bring just those up to the max current LOD before moving on"). Cross-fade starts were admitted in delivery order,
  and a level's children are requested only once it shows, so each level swept the whole scene before the next; they
  are now admitted by the same screen-space error as deliveries, and the scheduler's background load list waits until
  the view's requests are all in. The subject of a capture (a scan has no objects, only Gaussians) is found where the
  training cameras' view axes converge (least squares), out to where level 0's density falls to a tenth: on the
  bicycle, 2.26 m about the bike and bench. Its blocks rank 8x (StreamSettings::subjectCentre, subjectRadius,
  subjectBoost); it is saved in the view settings (`subject`). Measured at 5G from camera 1: the same progress with and
  without the boost (878 subject blocks at their level at 18 s either way), because from there nearly everything in view
  is the subject; the boost matters where scenery nearer the camera competes. At 5G the subject's cascade is limited by
  bandwidth (about 78 MB within 2.26 m).
- **Scenes stream Conservative** (the user: "for scenes we should keep to conservative mode, we dont want to stream
  everything in"): the view and the residency ring, nothing in the background. Objects keep the policy last chosen
  for an object.
- **Training-camera visibility for streaming order:** the bake records which training cameras see each block; at run
  time the blocks seen by the cameras nearest the eye stream first.
- Later, if HZB is not enough: precomputed visibility per view cell (PVS), expensive to bake.

Objects keep the octahedron and the occlusion volume, where they work.

### 6. Navigation and depth range

- **Detection and defaults on load** (built 2026-10-26, the user: "can the show occlusion volume/shadow volume
  requirement be determined by the model - can this and any other settings be set on load?"). The radius about the core
  holding 99% of level 0 over the one holding half: bicycle 6.4, Cthulhu 1.5, Venus 1.9, dwarves 2.0; above 3 is an
  environment. An environment loads with the occlusion volume (and so the shadow-mesh gap fill) off for the session,
  no auto-rotate, at its start camera or inside its core; the Renderer tab says so. Objects load with the saved choice.
- **View settings in the package** (built 2026-10-26, the user: "the gstx should have a section with preferred/default
  settings for when it is opened in a viewing app"): the trailing section GSTXVEW1 (format spec clause 14.2) carries the
  scene type and core, the occlusion volume's state on open, the training cameras in model space and the start camera.
  A .gstx then opens at the right view with no cameras.json beside it, and a package that states its scene is taken
  at its word. The UE5 plugin should read it too (to do).
- **Open at a training camera** when there is one (the package's start camera, else camera 0), else inside the core
  looking at its centre. The Renderer tab lists the cameras by number and photo name in a dropdown (built).
- **A fly camera** (built 2026-10-26, the user: "you may use wasd for navigation around a scene, but you'll need to
  reassign the loupe zoom keys"): W/S/A/D/Q/E fly, right-drag looks about the eye, speed from the orbit distance; the
  loupe magnifies on [ and ]. The orbit (left-drag, arrows, wheel) is unchanged.
- **Depth range:** near and far from the scene (today 0.1 to 500 m, hard-coded in six places), reversed-Z, so a
  subject at 2 m and a background at 100 m both resolve.

### 7. Up axis

COLMAP scenes are not gravity-aligned: estimate up at import from the training cameras (their image up vectors
average to it) or a ground-plane fit, and rotate the scene so the grid lies on the ground.

## Order and measurements

1. Training cameras and captures (built). Baseline: PSNR/SSIM of the current bake against the reference, per camera.
2. Diagnose the sky specks and the halo with the captures (level 0 forced, cap off, glow off): a fix or a step below.
3. Detection (2) and the occlusion default (5): the visible failures go.
4. Scale from content (3), then shells (4): measured by package size, PSNR against the reference and time to first
   view at 4G.
5. Fly camera, depth range and up axis (6, 7).

Each step changes bake output, so each is agreed before it lands and measured with the same captures.

## Risks

- **Package format:** shells and count-based chunks fit the current chunk table; per-shell parameters may need a header
  field (a format bump, and the UE5 plugin updated with it).
- **Objects must not regress:** detection keeps today's behaviour for objects; the captures run on Cthulhu and Venus too.
- **Reference fidelity:** the reference is the trainer's render at full precision; the package quantises (positions
  16-bit in the bounds: 2.6 mm over 169 m, coarser than the core needs, which shells also fix).
