"""Render every .glb/.obj under a directory into one labelled contact sheet.

Usage:
    blender -b --python scripts/blender_contact_sheet.py -- <dir> <out.png> [--cols 8] [--size 256] [--frame N] [--pattern GLOB]

One three-quarter view per model, each framed on its own bounding box, tiled
in a grid with the file stem written under each tile (needs Pillow in the
interpreter Blender runs; without it the sheet is written unlabelled).
Handy to eyeball a whole batch extraction (e.g. every stall) at once.
"""
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def args():
    a = sys.argv[sys.argv.index("--") + 1:]
    d, out = Path(a[0]), Path(a[1])
    opt = lambda k, dflt: type(dflt)(a[a.index(k) + 1]) if k in a else dflt
    return d, out, opt("--cols", 8), opt("--size", 256), opt("--frame", 1), opt("--pattern", "**/*.glb")


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.armatures, bpy.data.actions):
        for x in list(coll):
            coll.remove(x)


def render_one(path, png, size, frame):
    clear()
    if path.suffix.lower() == ".glb":
        bpy.ops.import_scene.gltf(filepath=str(path))
    else:
        bpy.ops.wm.obj_import(filepath=str(path))
    scene = bpy.context.scene
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    objs = [o for o in scene.objects if o.type == "MESH"]
    lo = Vector((1e9,) * 3)
    hi = Vector((-1e9,) * 3)
    deps = bpy.context.evaluated_depsgraph_get()
    for o in objs:
        ev = o.evaluated_get(deps)
        for v in ev.to_mesh().vertices:
            w = o.matrix_world @ v.co
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
        ev.to_mesh_clear()
    center, radius = (lo + hi) / 2, max((hi - lo).length / 2, 0.01)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    scene.collection.objects.link(sun)
    dist = radius / math.tan(cam.data.angle / 2) * 1.05
    az, el = math.radians(-55), math.radians(25)
    cam.location = center + Vector((math.cos(az) * math.cos(el), math.sin(az) * math.cos(el), math.sin(el))) * dist
    cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.render.resolution_x = scene.render.resolution_y = size
    scene.render.filepath = str(png)
    bpy.ops.render.render(write_still=True)


def main():
    d, out, cols, size, frame, pattern = args()
    files = sorted(p for p in d.glob(pattern) if "_lod" not in p.stem.lower())
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    engines = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items}
    scene.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in engines else "BLENDER_EEVEE"
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.65, 0.8, 1)
    scene.world = world
    tiles = []
    for i, f in enumerate(files):
        png = out.parent / f"_tile_{i:03d}.png"
        try:
            render_one(f, png, size, frame)
            tiles.append((f.stem, png))
        except Exception as e:  # keep going: one broken file shouldn't kill the sheet
            print(f"skip {f}: {e}")
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow missing: tiles left as", out.parent / "_tile_*.png")
        return
    rows = math.ceil(len(tiles) / cols)
    sheet = Image.new("RGB", (cols * size, rows * (size + 16)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, (name, png) in enumerate(tiles):
        x, y = (i % cols) * size, (i // cols) * (size + 16)
        sheet.paste(Image.open(png).convert("RGB"), (x, y))
        draw.text((x + 3, y + size + 2), name[:40], fill=(230, 230, 230))
        png.unlink()
    sheet.save(out)
    print(f"contact sheet {out} ({len(tiles)} models)")


main()
