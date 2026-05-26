"""Import every .obj under a root dir into the current Blender scene.

Usage (from the project root):
  blender --python scripts/blender_import_objs.py -- out/static_models
  blender -b --python scripts/blender_import_objs.py -- out/static_models out/scene.blend
    (-b headless + extra path: save to .blend instead of opening the UI)

Optional flags (after the root dir / output path):
  --high-only       skip multi-LOD low/med/ulow variants
  --limit N         pick N random files from the filtered set

Each OBJ is imported and offset on the X axis so they don't pile up at the
origin. Layout: a square-ish grid, spacing 6 m. The active scene's default
cube/camera/light are cleared first.
"""

from __future__ import annotations

import math
import random
import re
import sys
from pathlib import Path

import bpy


SKIP_RE = re.compile(r"(low|med|ulow|ultralow|_ul)$", re.IGNORECASE)


def parse_args() -> tuple[Path, Path | None, bool, int | None]:
    argv = sys.argv
    if "--" not in argv:
        print("error: missing '--' followed by <root-dir> [output.blend] [--high-only] [--limit N]")
        sys.exit(2)
    after = argv[argv.index("--") + 1:]
    high_only = "--high-only" in after
    limit: int | None = None
    if "--limit" in after:
        i = after.index("--limit")
        if i + 1 >= len(after):
            print("error: --limit needs a number")
            sys.exit(2)
        try:
            limit = int(after[i + 1])
        except ValueError:
            print(f"error: --limit needs an integer, got {after[i + 1]!r}")
            sys.exit(2)
        after = after[:i] + after[i + 2:]
    after = [a for a in after if a != "--high-only"]
    if not after:
        print("error: need at least <root-dir> after '--'")
        sys.exit(2)
    root = Path(after[0]).resolve()
    save_to = Path(after[1]).resolve() if len(after) > 1 else None
    if not root.is_dir():
        print(f"error: {root} is not a directory")
        sys.exit(2)
    return root, save_to, high_only, limit


def clear_default_scene() -> None:
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)


def import_obj(path: Path) -> list[bpy.types.Object]:
    """Import a .obj and return the new objects (Blender 4.x+ API)."""
    before = set(bpy.data.objects)
    # Blender 4.x renamed the operator
    if hasattr(bpy.ops.wm, "obj_import"):
        bpy.ops.wm.obj_import(filepath=str(path))
    else:
        bpy.ops.import_scene.obj(filepath=str(path))
    return [o for o in bpy.data.objects if o not in before]


def main() -> None:
    root, save_to, high_only, limit = parse_args()
    obj_paths = sorted(root.rglob("*.obj"))
    if high_only:
        before = len(obj_paths)
        obj_paths = [p for p in obj_paths if SKIP_RE.search(p.stem) is None]
        print(f"--high-only: kept {len(obj_paths)}/{before}")
    if limit is not None and limit < len(obj_paths):
        obj_paths = random.sample(obj_paths, limit)
        obj_paths.sort()  # keep the import order deterministic-ish for logs
        print(f"--limit {limit}: randomly picked {len(obj_paths)}")
    print(f"importing {len(obj_paths)} .obj files from {root}")
    clear_default_scene()

    cols = max(1, int(math.sqrt(len(obj_paths))))
    spacing = 6.0
    ok = 0
    fail = 0
    for i, p in enumerate(obj_paths):
        try:
            new = import_obj(p)
        except Exception as e:
            print(f"  skip {p.name}: {e}")
            fail += 1
            continue
        col = i % cols
        row = i // cols
        for o in new:
            o.location.x += col * spacing
            o.location.y += row * spacing
        ok += 1
        if (i + 1) % 200 == 0:
            print(f"  {i + 1}/{len(obj_paths)}...")

    print(f"done: {ok} imported, {fail} skipped")

    # Frame everything in the viewport
    if save_to:
        save_to.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(save_to))
        print(f"saved scene: {save_to}")
    else:
        # When run with GUI, frame all objects so they're visible
        for area in bpy.context.screen.areas:
            if area.type == "VIEW_3D":
                with bpy.context.temp_override(area=area):
                    bpy.ops.object.select_all(action="SELECT")
                    bpy.ops.view3d.view_selected()
                break


if __name__ == "__main__":
    main()
