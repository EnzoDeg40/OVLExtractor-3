"""Render an extracted model (.obj or .glb) to a PNG contact sheet for quick checks.

Usage:
    blender -b --python scripts/blender_render_obj.py -- model.obj out.png [--views 4] [--frame N]

Imports the model, frames it with an orthographic-ish camera from N azimuths
(default 4: front/right/back/left at 30° elevation) and renders each view with
EEVEE (at animation frame N for an animated .glb), then tiles them horizontally into `out.png`. Material alpha (map_d /
glTF alphaMode) is honoured so cut-out textures show correctly.
"""
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2:
        raise SystemExit("usage: -- <model.obj|glb> <out.png> [--views N] [--size PX]")
    model, out = Path(argv[0]), Path(argv[1])
    views, size, frame = 4, 512, None
    if "--views" in argv:
        views = int(argv[argv.index("--views") + 1])
    if "--size" in argv:
        size = int(argv[argv.index("--size") + 1])
    if "--frame" in argv:
        frame = int(argv[argv.index("--frame") + 1])
    return model, out, views, size, frame


def main():
    model, out, views, size, frame = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if model.suffix.lower() in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=str(model))
    else:
        bpy.ops.wm.obj_import(filepath=str(model))
    if frame is not None:
        bpy.context.scene.frame_set(frame)
        bpy.context.view_layer.update()
    objs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not objs:
        raise SystemExit("no mesh imported")

    # Bounding box in world space.
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
    center = (lo + hi) / 2
    radius = max((hi - lo).length / 2, 0.01)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in {
        e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items
    } else "BLENDER_EEVEE"
    scene.render.resolution_x = size
    scene.render.resolution_y = size
    scene.render.film_transparent = False
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.65, 0.8, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 1.0
    scene.world = world

    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    scene.collection.objects.link(sun)

    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.lens = 50
    scene.collection.objects.link(cam)
    scene.camera = cam
    dist = radius / math.tan(cam.data.angle / 2) * 1.1

    tiles = []
    tmp = out.with_suffix("")
    for i in range(views):
        az = math.radians(-90 + 360 * i / views + 35)
        el = math.radians(25)
        # Blender OBJ import is Z-up; the game is Y-up which the importer converts.
        offset = Vector((math.cos(az) * math.cos(el), math.sin(az) * math.cos(el), math.sin(el))) * dist
        cam.location = center + offset
        cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
        path = f"{tmp}_v{i}.png"
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        tiles.append(path)

    # Stitch tiles horizontally.
    imgs = [bpy.data.images.load(p) for p in tiles]
    w, h = imgs[0].size
    sheet = bpy.data.images.new("sheet", w * len(imgs), h, alpha=True)
    px = [0.0] * (w * len(imgs) * h * 4)
    for k, im in enumerate(imgs):
        src = list(im.pixels)
        for y in range(h):
            row = y * w * 4
            dst = (y * w * len(imgs) + k * w) * 4
            px[dst:dst + w * 4] = src[row:row + w * 4]
    sheet.pixels = px
    sheet.filepath_raw = str(out)
    sheet.file_format = "PNG"
    sheet.save()
    for p in tiles:
        Path(p).unlink(missing_ok=True)
    print(f"rendered {out}")


main()
