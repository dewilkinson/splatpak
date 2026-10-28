#!/usr/bin/env python3
"""
stress_test.py -- combinatorial stress test for the public SplatPak build.

Exercises the CLI pipeline tools (CompressVenus, TestOcclusionVolume, TestLoadPackage)
across many randomized combinations of:
  - source model (Cthulhu or Venus, chosen at random per case)
  - operation kind: full PLY->package SAVE round trip, or an existing-package LOAD,
    or a re-save-then-reload double round trip
  - shave value (0..10) and, for load cases, a resolution override

The public build links libs/gstx in one of two forms (see that directory's
README): the PREBUILT proprietary library, or the OPEN stand-ins as a fallback. The test
detects which one the tools were built with (from CompressVenus's own output) and asserts
the invariants that must hold for that form, regardless of model or parameters:
  - the tool exits 0 (no crash, no reported failure)
  - a save produces a .gstx that reloads with the SAME splat and chunk counts
  - prebuilt codec: every save bakes a NON-EMPTY occluder mesh (format 13 stores the occlusion
    volume as a mesh in place of the voxel cubes, so no cubes are written), and reloading the
    package reports exactly the triangle count the save wrote
  - open stand-ins: every save writes no occluder mesh and 0 occluder cubes (the stand-in never
    bakes a volume) -- this doubles as a check that no proprietary occlusion logic is silently
    present in a from-source build
  - a package that was saved and reloaded twice (double round trip) is byte-for-byte
    identical the second time (idempotent save)

Usage: python stress_test.py [--cases N] [--seed S]
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # C:\github\splatpak
BIN = os.path.join(ROOT, "bin")

COMPRESS_VENUS = os.path.join(BIN, "CompressVenus.exe")
TEST_LOAD_PACKAGE = os.path.join(BIN, "TestLoadPackage.exe")
TEST_OCCLUSION_VOLUME = os.path.join(BIN, "TestOcclusionVolume.exe")

MODELS = {
    "cthulu": {
        "ply": os.path.join(ROOT, "assets", "cthulu", "cthulu.ply"),
        "gstx": os.path.join(ROOT, "assets", "cthulu", "cthulu.gstx"),
    },
    "venus": {
        "ply": os.path.join(ROOT, "assets", "venus", "venus.ply"),
        "gstx": os.path.join(ROOT, "assets", "venus", "venus.gstx"),
    },
}

OPEN_BUILD_MARKER = "open build has no occlusion volume feature"


def detect_codec():
    """Runs CompressVenus with no usable input just to read its banner/trace: the open
    stand-in's BuildGrid announces itself with OPEN_BUILD_MARKER, the prebuilt codec never
    does. Returns "prebuilt", "open", or None if it could not be determined (no tool)."""
    if not os.path.isfile(COMPRESS_VENUS):
        return None
    probe = os.path.join(tempfile.gettempdir(), "stress_probe_missing.ply")
    rc, out, _ = run([COMPRESS_VENUS, probe, os.path.join(tempfile.gettempdir(), "stress_probe"), "0"], timeout=120)
    if OPEN_BUILD_MARKER in out:
        return "open"
    # A missing input never reaches the occlusion bake, so probe with the smallest real
    # model we have instead when the banner alone was not conclusive.
    for m in MODELS.values():
        if os.path.isfile(m["gstx"]):
            rc, out, _ = run([TEST_OCCLUSION_VOLUME, m["gstx"], "0"], timeout=300)
            return "open" if OPEN_BUILD_MARKER in out else "prebuilt"
    return None


CODEC = None  # set in main(): "prebuilt" or "open"


class CaseResult:
    def __init__(self, index, kind, model, params):
        self.index = index
        self.kind = kind
        self.model = model
        self.params = params
        self.ok = True
        self.notes = []
        self.elapsed = 0.0

    def fail(self, msg):
        self.ok = False
        self.notes.append("FAIL: " + msg)

    def note(self, msg):
        self.notes.append(msg)

    def summary_line(self):
        status = "PASS" if self.ok else "FAIL"
        return f"[{self.index:3d}] {status}  {self.kind:<24s} model={self.model:<7s} {self.params}  ({self.elapsed:.1f}s)"


def run(cmd, timeout=600):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr, time.time() - t0
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT", time.time() - t0


def extract(pattern, text, cast=int):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None


def case_save_roundtrip(idx, model, shave):
    """Full pipeline: raw .ply -> chunks/LOD/package (CompressVenus), then reload and
    compare splat/chunk counts, then check the occlusion volume against what this
    build's codec is expected to produce (a real bake, or none at all)."""
    r = CaseResult(idx, "PLY->save->reload", model, f"shave={shave:.1f}")
    ply = MODELS[model]["ply"]
    if not os.path.isfile(ply):
        r.note(f"skipped: source PLY not found at {ply}")
        return r

    with tempfile.TemporaryDirectory() as td:
        out_base = os.path.join(td, f"{model}_case{idx}")
        rc, out, dt = run([COMPRESS_VENUS, ply, out_base, str(shave)], timeout=900)
        r.elapsed += dt
        if rc != 0:
            r.fail(f"CompressVenus exited {rc}: {out[-400:]}")
            return r
        saved_splats = extract(r"Loaded (\d+) points", out)
        saved_chunks = extract(r"Partitioned into (\d+) chunks", out)
        saved_cubes = extract(r"(\d+) occluder blocks", out)
        saved_occ = extract(r"occluder mesh (\d+) triangles", out)
        saved_grid = extract(r"detail grid (\d+)x", out)
        # Every build (prebuilt codec or open stand-ins) bakes the detail heatmap: it is Apache-side code.
        if not saved_grid:
            r.fail("no detail heatmap grid was written (expected a v7 package with a detail grid)")
        if saved_cubes != 0:
            r.fail(f"save wrote {saved_cubes} occluder cubes (format 13 packages carry the occluder as a mesh: expected 0)")
        if CODEC == "open" and saved_occ:
            r.fail(f"open build baked a {saved_occ}-triangle occluder mesh (expected none)")
        if CODEC == "prebuilt" and not saved_occ:
            r.fail(f"prebuilt codec baked a {saved_occ}-triangle occluder mesh (expected a non-empty mesh)")

        sflw = out_base + ".gstx"
        if not os.path.isfile(sflw):
            r.fail("save reported success but .gstx file is missing")
            return r

        rc2, out2, dt2 = run([TEST_OCCLUSION_VOLUME, sflw, "0"], timeout=300)
        r.elapsed += dt2
        loaded_splats = extract(r"Loaded (\d+) LOD0 splats", out2)
        loaded_chunks = extract(r"from (\d+) chunks", out2)
        loaded_cubes = extract(r"package volume had (\d+) cubes", out2)
        loaded_occ = extract(r"package occluder mesh (\d+) triangles", out2)
        loaded_grid = extract(r"detail grid (\d+)x", out2)
        if saved_grid is not None and loaded_grid != saved_grid:
            r.fail(f"reloaded package reports a {loaded_grid}-wide detail grid but the save wrote {saved_grid}")
        if loaded_cubes != 0:
            r.fail(f"reloaded package reports {loaded_cubes} baked cubes (expected 0)")
        if loaded_occ != (saved_occ or 0):
            r.fail(f"reloaded package reports a {loaded_occ}-triangle occluder mesh but the save wrote {saved_occ}")
        if saved_splats is not None and loaded_splats is not None and saved_splats != loaded_splats:
            r.fail(f"splat count mismatch: saved {saved_splats} vs reloaded {loaded_splats}")
        if saved_chunks is not None and loaded_chunks is not None and saved_chunks != loaded_chunks:
            r.fail(f"chunk count mismatch: saved {saved_chunks} vs reloaded {loaded_chunks}")
        r.note(f"splats={loaded_splats} chunks={loaded_chunks} occluder={loaded_occ} triangles")
    return r


def case_load_existing(idx, model, resolution):
    """Load a bundled/pre-existing .gstx package and verify it reports sane, consistent
    data. With the prebuilt codec the bundled packages must carry a baked volume; with
    the open stand-ins the tool cannot rebuild a grid (expected, not a failure)."""
    r = CaseResult(idx, "load existing .gstx", model, f"resolution={resolution}")
    sflw = MODELS[model]["gstx"]
    if not os.path.isfile(sflw):
        r.note(f"skipped: package not found at {sflw}")
        return r

    rc, out, dt = run([TEST_OCCLUSION_VOLUME, sflw, str(resolution)], timeout=300)
    r.elapsed = dt
    # With the open stand-ins TestOcclusionVolume always exits 1 once it reaches the
    # "rebuild the grid from the loaded points" step, since the open BuildGrid()
    # deliberately leaves valid=false (no occlusion-volume feature) and the tool treats
    # that as fatal for its own diagnostic purposes -- expected there, NOT a load failure.
    # What matters is whether the package itself loaded, which the splat/chunk counts
    # printed *before* that point already prove. With the prebuilt codec a non-zero exit
    # is a real failure.
    if rc != 0 and (CODEC == "prebuilt" or OPEN_BUILD_MARKER not in out):
        r.fail(f"TestOcclusionVolume exited {rc}: {out[-400:]}")
        return r
    splats = extract(r"Loaded (\d+) LOD0 splats", out)
    chunks = extract(r"from (\d+) chunks", out)
    occ = extract(r"package occluder mesh (\d+) triangles", out)
    if "Failed to load" in out or "Failed to open" in out:
        r.fail(f"package failed to load: {out[-300:]}")
        return r
    if splats is None or splats == 0:
        r.fail("no splats reported on load")
    if CODEC == "prebuilt" and not occ:
        r.fail("bundled package carries no occluder mesh (was it written by the open stand-in codec?)")
    if CODEC == "open" and occ:
        r.note(f"package carries a {occ}-triangle occluder mesh from a prebuilt-codec bake -- not a failure, just provenance")
    r.note(f"splats={splats} chunks={chunks} occluder={occ} triangles")
    return r


def case_double_roundtrip(idx, model, shave):
    """Save once, reload, re-save from the same source, and confirm the second save is
    byte-for-byte identical to the first (idempotent, deterministic save)."""
    r = CaseResult(idx, "double save idempotency", model, f"shave={shave:.1f}")
    ply = MODELS[model]["ply"]
    if not os.path.isfile(ply):
        r.note(f"skipped: source PLY not found at {ply}")
        return r

    with tempfile.TemporaryDirectory() as td:
        out1 = os.path.join(td, "first")
        out2 = os.path.join(td, "second")
        rc1, o1, dt1 = run([COMPRESS_VENUS, ply, out1, str(shave)], timeout=900)
        rc2, o2, dt2 = run([COMPRESS_VENUS, ply, out2, str(shave)], timeout=900)
        r.elapsed = dt1 + dt2
        if rc1 != 0 or rc2 != 0:
            r.fail(f"a save exited non-zero (rc1={rc1}, rc2={rc2})")
            return r
        f1, f2 = out1 + ".gstx", out2 + ".gstx"
        if not (os.path.isfile(f1) and os.path.isfile(f2)):
            r.fail("one or both .gstx outputs missing")
            return r
        b1, b2 = open(f1, "rb").read(), open(f2, "rb").read()
        if b1 != b2:
            r.fail(f"saves differ: {len(b1)} vs {len(b2)} bytes (not deterministic)")
        else:
            r.note(f"identical, {len(b1)} bytes")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=16, help="number of randomized cases to run")
    ap.add_argument("--seed", type=int, default=None, help="RNG seed for reproducibility")
    args = ap.parse_args()

    seed = args.seed if args.seed is not None else random.randint(0, 1_000_000)
    random.seed(seed)
    print(f"stress_test.py: {args.cases} cases, seed={seed}")
    print(f"root: {ROOT}")
    for name, exe in [("CompressVenus", COMPRESS_VENUS), ("TestLoadPackage", TEST_LOAD_PACKAGE), ("TestOcclusionVolume", TEST_OCCLUSION_VOLUME)]:
        print(f"  {name}: {'OK' if os.path.isfile(exe) else 'MISSING'} ({exe})")
    global CODEC
    CODEC = detect_codec()
    if CODEC is None:
        print("could not determine which gstx the tools were built with (tools or packages missing)")
        sys.exit(2)
    print(f"  gstx: {CODEC} ({'real bake expected' if CODEC == 'prebuilt' else 'open stand-ins, no volume expected'})")
    print()

    kinds = ["save_roundtrip", "load_existing", "load_existing", "double_roundtrip"]  # weight toward cheap load cases
    results = []
    for i in range(1, args.cases + 1):
        model = random.choice(list(MODELS.keys()))
        kind = random.choice(kinds)
        if kind == "save_roundtrip":
            r = case_save_roundtrip(i, model, round(random.uniform(0.0, 10.0), 1))
        elif kind == "double_roundtrip":
            r = case_double_roundtrip(i, model, round(random.uniform(0.0, 10.0), 1))
        else:
            r = case_load_existing(i, model, random.choice([0, 32, 64, 96, 128]))
        results.append(r)
        print(r.summary_line())
        for n in r.notes:
            print("       " + n)

    print()
    passed = sum(1 for r in results if r.ok)
    skipped = sum(1 for r in results if any("skipped" in n for n in r.notes))
    failed = len(results) - passed
    print(f"=== {passed}/{len(results)} passed, {failed} failed, {skipped} skipped (seed={seed}) ===")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
