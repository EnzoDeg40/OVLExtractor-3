"""Assemble a whole ride in Blender from a layout + the per-piece .glb files.

Usage:
    blender -b --python scripts/blender_ride.py -- ride.layout.json GLB_DIR \
        [--blend out.blend] [--glb out.glb] [--render out.png] [--size PX] [--views N]
        [--focus car|platform|track] [--elev DEG] [--zoom F]

`ride.layout.json` comes from `python3 scripts/rct3_track.py layout …`
(track pieces, station platforms, a parked train — glTF-space matrices);
GLB_DIR holds the `ovlextract -t scenery` output (<svd>.glb). Each asset is
imported once into its own hidden collection and placed as collection
instances, so a 150-piece coaster stays light. --focus frames the camera on
one kind of item (e.g. the train in the station) instead of the whole ride.
"""
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

# glTF (Y up) -> Blender (Z up), what the glTF importer applies to the data.
C = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))


def arg(argv, name, default=None, cast=str):
    return cast(argv[argv.index(name) + 1]) if name in argv else default


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2:
        raise SystemExit("usage: -- ride.layout.json GLB_DIR [--blend f] [--glb f] [--render f.png]")
    layout = json.loads(Path(argv[0]).read_text())
    glb_dir = Path(argv[1])
    blend, glb, render = arg(argv, "--blend"), arg(argv, "--glb"), arg(argv, "--render")
    size, views, focus = arg(argv, "--size", 1400, int), arg(argv, "--views", 1, int), arg(argv, "--focus")
    elev, zoom = arg(argv, "--elev", 35.0, float), arg(argv, "--zoom", 1.0, float)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    files = {p.stem.lower(): p for p in glb_dir.glob("*.glb")}
    assets = {}
    missing = set()
    for it in layout["items"]:
        key = it["asset"].lower()
        if key in assets or key in missing:
            continue
        f = files.get(key)
        if not f:
            missing.add(key)
            continue
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=str(f))
        new = [o for o in bpy.data.objects if o not in before]
        # the glTF importer adds an icosphere as the bones' display shape;
        # it must not be instanced as geometry
        shapes = {pb.custom_shape for o in new if o.type == "ARMATURE"
                  for pb in o.pose.bones if pb.custom_shape}
        new = [o for o in new if o not in shapes]
        col = bpy.data.collections.new("asset_" + it["asset"])
        for o in new:
            for c in o.users_collection:
                c.objects.unlink(o)
            col.objects.link(o)
        assets[key] = col
    if missing:
        print("missing assets:", sorted(missing))

    ride = bpy.data.collections.new(layout.get("ride", "ride"))
    scene.collection.children.link(ride)
    placed = []
    for it in layout["items"]:
        col = assets.get(it["asset"].lower())
        if not col:
            continue
        m = it["matrix"]
        M = Matrix([[m[c * 4 + r] for c in range(4)] for r in range(4)])
        e = bpy.data.objects.new(f'{it["kind"]}:{it["piece"]}', None)
        e.instance_type = "COLLECTION"
        e.instance_collection = col
        e.matrix_world = C @ M @ C.inverted()
        ride.objects.link(e)
        placed.append((it["kind"], e))
    print(f"placed {len(placed)} items from {len(assets)} assets")

    if blend:
        bpy.ops.wm.save_as_mainfile(filepath=str(Path(blend).resolve()))
    if glb:
        bpy.ops.export_scene.gltf(filepath=str(Path(glb).resolve()), export_format="GLB")
    if render:
        render_views(scene, placed, assets, Path(render), size, views, focus, elev, zoom)


def render_views(scene, placed, assets, out, size, views, focus, elev=35.0, zoom=1.0):
    lo = Vector((1e9, 1e9, 1e9))
    hi = -lo
    for kind, e in placed:
        if focus and kind != focus:
            continue
        for o in e.instance_collection.all_objects:
            if o.type != "MESH":
                continue
            for c in o.bound_box:
                w = e.matrix_world @ o.matrix_world @ Vector(c)
                lo = Vector(map(min, lo, w))
                hi = Vector(map(max, hi, w))
    center = (lo + hi) / 2
    radius = max((hi - lo).length / 2, 0.5)
    engines = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items}
    scene.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in engines else "BLENDER_EEVEE"
    scene.render.resolution_x = size
    scene.render.resolution_y = int(size * 0.66)
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.65, 0.8, 1)
    scene.world = world
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    scene.collection.objects.link(sun)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.lens = 40
    cam.data.clip_end = 5000
    scene.collection.objects.link(cam)
    scene.camera = cam
    dist = radius / math.tan(cam.data.angle / 2) * 0.95 / zoom
    for i in range(views):
        az = math.radians(-60 + 360 * i / views)
        el = math.radians(elev)
        cam.location = center + Vector((math.cos(az) * math.cos(el), math.sin(az) * math.cos(el),
                                        math.sin(el))) * dist
        cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
        path = out if views == 1 else out.with_name(f"{out.stem}_v{i}{out.suffix}")
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        print("rendered", path)


main()
