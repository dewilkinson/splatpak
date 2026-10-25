# Shadow mesh baked into GSTx packages

Status: agreed
Date: 2026-10-25
Owner: Dave Wilkinson

## Goal
A splat model placed in Unreal casts shadows that match its visible shape, contributes to Lumen and has
collision, all from one smooth, low-polygon mesh that SplatLab bakes into the package. This replaces
UE5Splats' planned shadow proxy built from the interior occlusion volume (UE5Splats D20).

## User-visible behaviour
- **SplatLab, GSTx Bake tab.** Saving a package also bakes a shadow mesh. There are no new controls: the
  mesh lies on the visible surface of the level 0 splats (not inside it) and has three levels of detail.
  The finest level aims at about 10,000 triangles and spends more where the model is thin, so thin parts
  such as tentacle tips and fingers keep their shadow. The save's status line reports the triangle count
  of each level.
- **Package format.** A new format version (11) adds an optional mesh section: the three levels, each an
  indexed triangle list in model space. Its layout is documented in `docs/GSTX_FORMAT_SPECIFICATION.md`.
  Packages without the section (version 10 and older, `.sflw` included) still load everywhere.
- **SplatLab, Renderer tab.** Packages with a mesh show its level count and triangle counts in Input
  Model Metrics. A **Show Shadow Mesh** checkbox (off by default, remembered like the other debug views)
  draws the finest level as a wireframe over the splats, with a banner row naming it, as the other debug
  toggles do. It is disabled, with a tooltip saying why, for a package without a mesh.
- **UE5Splats.** Importing a package that carries a mesh also creates a hidden mesh for it. A placed splat
  model then:
  - casts shadows from every light type, virtual shadow maps included;
  - contributes to Lumen through a mesh distance field;
  - has collision for line traces and physics.
  The splat component gains three checkboxes, all on by default: Cast Shadow, Affect Lumen and Collision.
  A package without a mesh imports as today, casts no shadow, and logs one line suggesting a rebake.

## Non-goals
- Soft or partly transparent shadows (hair, foliage, fuzzy edges cast solid shadows).
- Splats receiving shadows or Unreal lighting (UE5Splats step 1b).
- Using the mesh for SplatLab's occlusion culling; the interior occlusion volume keeps that role.
- Animated or deforming models, and editing the mesh.
- A triangle-budget control (the budget is automatic).
- Shadows exactly as soft as the splats' own edges; the mesh is a solid surface.

## Constraints
- Render paths: splat drawing on the mesh, VS SM6 and VS SM5 paths is unchanged.
- Modes: Studio bakes the mesh; Viewer and the console tools read it.
- Package compatibility: version 11 readers read versions 1 to 10 unchanged. Versions 10 and older are
  never rewritten on load.
- Size: the mesh section is at most 1 MB for any bundled package.
- Bake time: at most 20% longer than today for the bundled Cthulhu package.
- Fidelity: shadows follow the visible surface, not the occlusion shell inside it, and thin parts of the
  model keep their shadow.
- Self-shadowing: the mesh lies on the surface, so once splats receive shadows (step 1b, not here) a depth
  bias in that work keeps the model from shadowing its own splats.

## Public / private placement
- Private (`libs/gstx`): building the mesh from the splats, simplifying it and deriving its levels of
  detail. Public documents describe it only as "a smooth mesh following the visible surface".
- Public: the format section and its specification, the reader in SplatLab, the console tools and
  UE5Splats, the SplatLab readouts, and UE5Splats' shadow, Lumen and collision use.
- The open stand-in library bakes no mesh (its packages have no mesh section), and reads packages that
  have one.

## Acceptance criteria
1. Baking each bundled package writes format version 11 with a mesh section of three levels. The finest
   level has 8,000 to 20,000 triangles and each coarser level at most half the triangles of the previous
   one. `TestLoadPackage` prints each level's triangle count.
2. The finest level is closed (every edge shared by exactly two triangles) and has no degenerate
   triangles, checked by a console test.
3. The finest level follows the visible surface: the median distance from the level 0 splat centres to
   the mesh is at most 0.5% of the model's bounding-box diagonal, the 95th percentile at most 1%, and the
   99.9th percentile at most 2% (a lost thin part shows up as splats far from any triangle), reported by
   the same console test for every bundled package. The mesh's own vertices lie on the surface: their
   median distance to the nearest level 0 splat centre is at most 0.5% of the diagonal.
4. The mesh section of every bundled package is at most 1 MB; the save's status line and Input Model
   Metrics report the triangle counts.
5. Cthulhu's bake takes at most 20% longer than at the start of this work (same machine, Release).
6. Version 10 `.gstx` and version 9 `.sflw` packages still load in SplatLab, the Viewer, `TestLoadPackage`
   and UE5Splats, and SplatLab's splat rendering of a version 11 package is pixel-identical to the same
   package without its mesh.
7. UE5Splats' automation tests read the mesh section of every bundled package and check its level and
   triangle counts against the package.
8. In UE5Splats' test scene, the Cthulhu package casts a shadow under a directional light with virtual
   shadow maps; a screenshot shows the shadow's outline matching the model's silhouette.
9. In UE5Splats, a line trace aimed at the model hits within 2% of the bounding-box diagonal of the
   visible surface, checked by an automation test; with Collision off it passes through.
10. In UE5Splats, the model's mesh distance field appears in Unreal's distance field visualisation, and
    turning Affect Lumen off removes it.
11. The public stand-in build (`-DGSTX_USE_PREBUILT=OFF`) bakes packages without a mesh section and reads
    the bundled packages that have one.
12. No public file (public docs and the generalised design summary) describes how the mesh is extracted
    or simplified.
13. In SplatLab, Show Shadow Mesh draws the finest level over the Cthulhu package on every render path (mesh,
    VS SM6, VS SM5) with its banner row; a screenshot shows the wireframe hugging the model, tentacle tips
    included. The checkbox is disabled for a version 10 package.

## Decisions (user, 2026-10-25)
- Built at bake time and stored in the package, not at import.
- Follows the visible splat surface, on it rather than inside it.
- Three levels of detail with an automatic budget of about 10,000 triangles at the finest; thin parts must
  survive, so the budget is a target, not a cap.
- In Unreal it serves shadows, Lumen and collision; the extraction lives in the private library.
- SplatLab gets a Show Shadow Mesh debug view.
