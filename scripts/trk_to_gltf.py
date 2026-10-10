"""Reconstruct an RCT3 saved coaster (.trk) as a glTF/GLB scene, placing the
track-piece meshes extracted by `ovlextract --types model`.

Usage (from the project root, after extracting the coaster's track-piece set
into out/models/tracks/coasters/<set>/):

  python3 scripts/trk_to_gltf.py '[WoodenRC]MR_compact.trk'
  python3 scripts/trk_to_gltf.py ride.trk -o ride.glb --models out/models
  python3 scripts/trk_to_gltf.py ride.trk --set Track20 --lod ME --report
  python3 scripts/trk_to_gltf.py ride.trk --no-textures        # flat material

Flags:
  -o, --output PATH   output .glb (default: <trk basename>.glb next to the .trk)
  --models DIR        extracted-models root (default: out/models)
  --set NAME          coaster piece set, e.g. Track20 (default: auto-detected
                      from the Tracks\\Coasters\\<set>\\ references in the .trk)
  --lod HI|ME|LO      level of detail to place (default: HI)
  --tile-size M       metres per grid tile = size of one straight piece
                      (default: 1.0); the scene is also recentred on the origin
  --textures DIR      folder holding the piece textures (default: auto — the
                      set's <set>_Textures folder under any out/ subtree)
  --no-textures       skip textures, use flat per-material colours (stdlib only)
  --report            print the piece-count histogram

How it works
------------
A .trk stores its placed pieces as packed records in the file tail: an
`ef ef ef ef` marker, a UTF-16LE `name:tks` symbol, then five int32 —
CORNER, DIRECTION, HEIGHT, POSX, POSZ — at +8/+12/+16/+20/+24 from the symbol
end (same field order as the part-1 `SceneryItemDataField` struct). The visual
mesh of each piece is <models>/tracks/coasters/<set>/<name>_data/<name>_<lod>.obj.

World convention, calibrated from the meshes: 1 grid tile = 4 mesh units,
1 HEIGHT level = 1 unit, DIRECTION = 90-degree yaw steps, Y is up. RCT3 is
left-handed and the OBJ meshes come out of ovlextract mirrored on Z into
glTF's right-handed frame, so placements are mirrored the same way (POSZ → -Z,
yaw → -yaw); every piece becomes a node with a translation and a
90-degree-step rotation quaternion. POSX/POSZ name the SW corner of the
footprint, so even-tile-span pieces are shifted half a tile to re-centre. The
ride hangs under one root node scaled to `--tile-size` metres per tile and
recentred on the origin.

Textures (the OBJ carries UVs + per-sub-mesh `usemtl` groups) are embedded as
PNG when Pillow is available; otherwise the script falls back to flat colours
and stays dependency-free. See docs/TRK_FORMAT.md for the format.
"""

from __future__ import annotations

import argparse
import io
import json
import math
import re
import struct
import sys
from collections import Counter
from pathlib import Path

TILE = 4.0          # mesh units per grid tile (POSX/POSZ -> world X/Z)
HUNIT = 1.0         # mesh units per HEIGHT level   (HEIGHT  -> world Y)
EF = b"\xef\xef\xef\xef"


# --------------------------------------------------------------------------- #
# .trk parsing
# --------------------------------------------------------------------------- #
def _utf16(buf: bytes, off: int) -> tuple[str, int]:
    out = []
    j = off
    while j < len(buf) - 1 and 32 <= buf[j] < 127 and buf[j + 1] == 0:
        out.append(chr(buf[j]))
        j += 2
    return "".join(out), j


def parse_pieces(buf: bytes) -> list[dict]:
    """Return the placed track pieces in file order."""
    pieces = []
    pos = 0
    while True:
        m = buf.find(EF, pos)
        if m < 0:
            break
        pos = m + 4
        sym, end = _utf16(buf, m + 4)
        if not sym.endswith(":tks"):
            continue

        def i32(k: int) -> int:
            o = end + k
            return struct.unpack_from("<i", buf, o)[0] if 0 <= o <= len(buf) - 4 else 0

        pieces.append({
            "name": sym[:-4],
            "corner": i32(8), "direction": i32(12),
            "height": i32(16), "posx": i32(20), "posz": i32(24),
        })
    return pieces


def detect_set(buf: bytes) -> str | None:
    """Most common <set> in the Tracks\\Coasters\\<set>\\ references (UTF-16)."""
    sets: Counter[str] = Counter()
    pos = 0
    while pos < len(buf) - 1:
        if 32 <= buf[pos] < 127 and buf[pos + 1] == 0:
            s, end = _utf16(buf, pos)
            pos = end
            mt = re.search(r"Tracks\\Coasters\\([^\\]+)\\", s)
            if mt:
                sets[mt.group(1)] += 1
        else:
            pos += 1
    return sets.most_common(1)[0][0] if sets else None


# --------------------------------------------------------------------------- #
# OBJ loading — per-material sub-meshes with positions, normals, UVs
# --------------------------------------------------------------------------- #
def find_obj(models: Path, name: str, sets: list[str], lod: str) -> Path | None:
    coasters = models / "tracks" / "coasters"
    search = [coasters / s for s in sets] + sorted(coasters.glob("*"))
    for setdir in search:
        if not setdir.is_dir():
            continue
        for datadir in setdir.glob("*_data"):
            if datadir.name[:-5].lower() == name.lower():
                hits = (sorted(datadir.glob(f"*_{lod}.obj"))
                        or sorted(datadir.glob("*.obj")))
                if hits:
                    return hits[0]
    return None


def load_obj(path: Path) -> list[dict]:
    """Return a list of sub-meshes, one per `usemtl` group, de-indexed:
    {material, pos[float*3], nrm[float*3], uv[float*2], idx[int]}."""
    verts: list[tuple] = []
    uvs: list[tuple] = []
    norms: list[tuple] = []
    groups: list[tuple[str, list]] = []
    cur_mat = "default"
    cur_faces: list = []

    def flush():
        if cur_faces:
            groups.append((cur_mat, cur_faces))

    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("v "):
            p = line.split(); verts.append((float(p[1]), float(p[2]), float(p[3])))
        elif line.startswith("vt "):
            p = line.split(); uvs.append((float(p[1]), float(p[2])))
        elif line.startswith("vn "):
            p = line.split(); norms.append((float(p[1]), float(p[2]), float(p[3])))
        elif line.startswith("usemtl "):
            flush(); cur_mat = line.split(None, 1)[1].strip(); cur_faces = []
        elif line.startswith("f "):
            ref = []
            for w in line.split()[1:]:
                a = (w.split("/") + ["", "", ""])[:3]
                ref.append((int(a[0]) - 1,
                            int(a[1]) - 1 if a[1] else -1,
                            int(a[2]) - 1 if a[2] else -1))
            for k in range(1, len(ref) - 1):
                cur_faces.append((ref[0], ref[k], ref[k + 1]))
    flush()

    out = []
    for mat, faces in groups:
        uniq: dict[tuple, int] = {}
        pos: list[float] = []; nrm: list[float] = []; uv: list[float] = []; idx: list[int] = []
        for tri in faces:
            for vi, ti, ni in tri:
                j = uniq.get((vi, ti, ni))
                if j is None:
                    j = uniq[(vi, ti, ni)] = len(pos) // 3
                    pos.extend(verts[vi])
                    nrm.extend(norms[ni] if 0 <= ni < len(norms) else (0.0, 1.0, 0.0))
                    u, v = uvs[ti] if 0 <= ti < len(uvs) else (0.0, 0.0)
                    uv.extend((u, 1.0 - v))          # OBJ V-up -> glTF V-down
                idx.append(j)
        out.append({"material": mat, "pos": pos, "nrm": nrm, "uv": uv, "idx": idx})
    return out


# --------------------------------------------------------------------------- #
# materials / textures
# --------------------------------------------------------------------------- #
def tex_basename(material: str) -> str:
    """`WoodenCoaster_Flexi__SIOpaque` -> `WoodenCoaster_Flexi`."""
    return material.split("__", 1)[0]


def resolve_texture(models: Path, setname: str | None, base: str,
                    override: Path | None) -> Path | None:
    # Search, in order: an explicit --textures dir, the set's own _Textures
    # folder, the shared-texture pool, then any other set's texture folder.
    # `*[Tt]extures` matches both `Track20_Textures` and `Track31_textures`.
    cands: list[Path] = []
    if override:
        cands.append(override / f"{base}.tga")
    roots = [models] + [s for s in sorted(models.parent.glob("*")) if s.is_dir()]
    for root in roots:
        if setname:
            cands.append(root / "tracks" / "coasters" / setname
                         / f"{setname}_Textures" / f"{base}.tga")
        cands.append(root / "tracks" / "SharedTextures" / base / f"{base}.tga")
        cands += sorted(root.glob(f"tracks/coasters/*/*[Tt]extures/{base}.tga"))
        cands += sorted(root.glob(f"tracks/SharedTextures/*/{base}.tga"))
    for c in cands:
        if c.is_file():
            return c
    return None


def flat_color(base: str) -> list[float]:
    b = base.lower()
    if "rail" in b:
        return [0.22, 0.23, 0.26, 1.0]                # dark metal
    if "wood" in b or "flexi" in b:
        return [0.55, 0.40, 0.25, 1.0]                # timber
    return [0.55, 0.55, 0.58, 1.0]


# --------------------------------------------------------------------------- #
# GLB assembly
# --------------------------------------------------------------------------- #
def quat_y(direction: int) -> list[float]:
    # The game turns a placed piece by a = -(direction + 2)·90° about Y
    # (SceneryItem_InitRotationMatrix; x' = x·cos a − z·sin a,
    # z' = x·sin a + z·cos a in its left-handed frame). ovlextract's OBJ is
    # already mirrored on Z into glTF's right-handed frame, and conjugating
    # that rotation by the mirror gives glTF's own R_y(a) — same angle.
    a = -math.radians(90.0 * (direction + 2)) / 2.0
    return [0.0, math.sin(a), 0.0, math.cos(a)]


def build_glb(pieces, meshes: dict, out: Path, tile_size: float,
              models: Path, setname: str | None,
              tex_dir: Path | None, want_textures: bool,
              squares: dict[str, tuple[int, int]] | None = None) -> dict:
    squares = squares or {}
    blob = bytearray()
    views: list[dict] = []
    accessors: list[dict] = []

    def add_view(data: bytes, target: int | None) -> int:
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data),
                      **({"target": target} if target else {})})
        blob.extend(data)
        return len(views) - 1

    def add_accessor(view, ctype, count, comp, mn=None, mx=None) -> int:
        a = {"bufferView": view, "componentType": comp, "count": count, "type": ctype}
        if mn is not None:
            a["min"], a["max"] = mn, mx
        accessors.append(a)
        return len(accessors) - 1

    # ---- materials (resolve a texture per usemtl group, else flat colour) ----
    Image = None
    if want_textures:
        try:
            from PIL import Image  # noqa: lazy, optional
        except ImportError:
            print("  (Pillow not installed — falling back to flat colours; "
                  "use --no-textures to silence)", file=sys.stderr)

    images: list[dict] = []
    textures: list[dict] = []
    samplers = [{"wrapS": 10497, "wrapT": 10497, "magFilter": 9729, "minFilter": 9987}]
    materials: list[dict] = []
    mat_index: dict[str, int] = {}
    tex_cache: dict[str, int] = {}
    n_textured = 0

    used_mats = {sm["material"] for subs in meshes.values() for sm in subs}
    for mat in sorted(used_mats):
        base = tex_basename(mat)
        tex_id = None
        if Image is not None:
            path = resolve_texture(models, setname, base, tex_dir)
            if path is not None:
                if base not in tex_cache:
                    buf = io.BytesIO()
                    Image.open(path).convert("RGBA").save(buf, "PNG")
                    iv = add_view(buf.getvalue(), None)
                    images.append({"mimeType": "image/png", "bufferView": iv, "name": base})
                    textures.append({"sampler": 0, "source": len(images) - 1})
                    tex_cache[base] = len(textures) - 1
                tex_id = tex_cache[base]
        pbr = {"metallicFactor": 0.0, "roughnessFactor": 0.85}
        if tex_id is not None:
            pbr["baseColorTexture"] = {"index": tex_id}
            n_textured += 1
        else:
            pbr["baseColorFactor"] = flat_color(base)
        mat_index[mat] = len(materials)
        materials.append({"name": mat, "pbrMetallicRoughness": pbr, "doubleSided": True})

    # ---- one glTF mesh per piece type (sub-meshes -> primitives) ----
    def footprint(name, subs) -> tuple[int, int]:
        # SceneryItem squares (exact, from `ovlextract -t track` JSON), else
        # the mesh's extent in whole tiles.
        if name.lower() in squares:
            return squares[name.lower()]
        xs = [v for sm in subs for v in sm["pos"][0::3]]
        zs = [v for sm in subs for v in sm["pos"][2::3]]
        if not xs:
            return 1, 1
        return (max(1, round((max(xs) - min(xs)) / TILE)),
                max(1, round((max(zs) - min(zs)) / TILE)))

    gltf_meshes: dict[str, int] = {}
    anchor: dict[str, tuple[int, int]] = {}
    mesh_list: list[dict] = []
    for name, subs in meshes.items():
        anchor[name] = footprint(name, subs)
        prims = []
        for sm in subs:
            npos = len(sm["pos"]) // 3
            mn = [min(sm["pos"][i::3]) for i in range(3)]
            mx = [max(sm["pos"][i::3]) for i in range(3)]
            ap = add_accessor(add_view(struct.pack(f"<{len(sm['pos'])}f", *sm["pos"]), 34962),
                              "VEC3", npos, 5126, mn, mx)
            an = add_accessor(add_view(struct.pack(f"<{len(sm['nrm'])}f", *sm["nrm"]), 34962),
                              "VEC3", npos, 5126)
            attrs = {"POSITION": ap, "NORMAL": an}
            if sm["uv"]:
                au = add_accessor(add_view(struct.pack(f"<{len(sm['uv'])}f", *sm["uv"]), 34962),
                                  "VEC2", npos, 5126)
                attrs["TEXCOORD_0"] = au
            ai = add_accessor(add_view(struct.pack(f"<{len(sm['idx'])}I", *sm["idx"]), 34963),
                              "SCALAR", len(sm["idx"]), 5125)
            prims.append({"attributes": attrs, "indices": ai,
                          "material": mat_index[sm["material"]]})
        gltf_meshes[name] = len(mesh_list)
        mesh_list.append({"name": name, "primitives": prims})

    # ---- nodes + centring/scaling root ----
    nodes = []
    for p in pieces:
        if p["name"] not in gltf_meshes:
            continue
        # The mesh is modelled centred on its footprint; POSX/POSZ is the
        # footprint's min corner, and the footprint turns with the piece.
        sx, sz = anchor[p["name"]]
        if p["direction"] % 2:
            sx, sz = sz, sx
        nodes.append({
            "name": f"{p['name']}@{p['posx']},{p['height']},{p['posz']}",
            "mesh": gltf_meshes[p["name"]],
            "translation": [(p["posx"] + sx / 2) * TILE, p["height"] * HUNIT,
                            -(p["posz"] + sz / 2) * TILE],
            "rotation": quat_y(p["direction"]),
        })
    sf = tile_size / TILE
    txs = [n["translation"][0] for n in nodes]
    tys = [n["translation"][1] for n in nodes]
    tzs = [n["translation"][2] for n in nodes]
    cx = (min(txs) + max(txs)) / 2 if txs else 0.0
    cy = min(tys) if tys else 0.0
    cz = (min(tzs) + max(tzs)) / 2 if tzs else 0.0
    nodes.append({"name": "ride", "scale": [sf, sf, sf],
                  "translation": [-cx * sf, -cy * sf, -cz * sf],
                  "children": list(range(len(nodes)))})

    gltf = {
        "asset": {"version": "2.0", "generator": "OVLExtractor trk_to_gltf"},
        "scene": 0,
        "scenes": [{"nodes": [len(nodes) - 1]}],
        "nodes": nodes,
        "meshes": mesh_list,
        "materials": materials,
        "bufferViews": views,
        "accessors": accessors,
        "buffers": [{"byteLength": len(blob)}],
    }
    if images:
        gltf["images"] = images
        gltf["textures"] = textures
        gltf["samplers"] = samplers

    jchunk = json.dumps(gltf, separators=(",", ":")).encode()
    jchunk += b" " * (-len(jchunk) % 4)
    bchunk = bytes(blob) + b"\x00" * (-len(blob) % 4)
    total = 12 + 8 + len(jchunk) + 8 + len(bchunk)
    glb = bytearray()
    glb += struct.pack("<III", 0x46546C67, 2, total)
    glb += struct.pack("<II", len(jchunk), 0x4E4F534A) + jchunk
    glb += struct.pack("<II", len(bchunk), 0x004E4942) + bchunk
    out.write_bytes(glb)
    return {"nodes": len(nodes) - 1, "meshes": len(mesh_list),
            "textures": len(images), "bytes": len(glb)}


# --------------------------------------------------------------------------- #
def main() -> None:
    ap = argparse.ArgumentParser(description="Reconstruct an RCT3 .trk coaster as glTF/GLB.")
    ap.add_argument("trk", type=Path, help="input .trk file")
    ap.add_argument("-o", "--output", type=Path, help="output .glb (default: <trk>.glb)")
    ap.add_argument("--models", type=Path, default=Path("out/models"),
                    help="extracted-models root (default: out/models)")
    ap.add_argument("--set", dest="setname", help="coaster piece set (default: auto)")
    ap.add_argument("--lod", default="HI", choices=["HI", "ME", "LO"], help="LOD (default: HI)")
    ap.add_argument("--tile-size", type=float, default=1.0,
                    help="metres per grid tile = size of one straight piece (default: 1.0)")
    ap.add_argument("--textures", type=Path, default=None,
                    help="texture folder (default: auto-locate <set>_Textures)")
    ap.add_argument("--no-textures", action="store_true", help="flat colours, stdlib only")
    ap.add_argument("--report", action="store_true", help="print piece histogram")
    ap.add_argument("--track-json", type=Path, default=None,
                    help="directory of `ovlextract -t track` output; gives the exact "
                         "piece footprints (default: estimated from the meshes)")
    args = ap.parse_args()

    if not args.trk.is_file():
        ap.error(f"no such file: {args.trk}")
    if not (args.models / "tracks" / "coasters").is_dir():
        ap.error(f"{args.models}/tracks/coasters not found — run "
                 f"`ovlextract --types model` on the coaster's OVLs first")

    buf = args.trk.read_bytes()
    pieces = parse_pieces(buf)
    if not pieces:
        ap.error("no :tks track pieces found — is this a coaster .trk?")
    setname = args.setname or detect_set(buf)
    sets = [setname] if setname else []
    print(f"{len(pieces)} pieces, coaster set = {setname or 'auto/any'}")

    if args.report:
        for name, c in Counter(p["name"] for p in pieces).most_common():
            print(f"  {c:3d} x {name}")

    meshes: dict = {}
    missing: set[str] = set()
    for p in pieces:
        name = p["name"]
        if name in meshes or name in missing:
            continue
        obj = find_obj(args.models, name, sets, args.lod)
        if obj is None:
            missing.add(name)
            print(f"  ! no mesh for {name}", file=sys.stderr)
        else:
            meshes[name] = load_obj(obj)
    if not meshes:
        ap.error("none of the pieces matched a mesh under "
                 f"{args.models}/tracks/coasters/ — check --set / --models")

    squares: dict[str, tuple[int, int]] = {}
    if args.track_json:
        for f in sorted(args.track_json.rglob("*.tracks.json")):
            t = json.loads(f.read_text())
            if setname and t["track_set"].lower() != setname.lower():
                continue
            for sec in t["sections"]:
                sq = (sec.get("sid") or {}).get("squares")
                if sq:
                    squares.setdefault(sec["name"].lower(), (int(sq[0]), int(sq[1])))

    out = args.output or args.trk.with_suffix(".glb")
    info = build_glb(pieces, meshes, out, args.tile_size,
                     args.models, setname, args.textures, not args.no_textures, squares)
    placed = sum(1 for p in pieces if p["name"] in meshes)
    extra = (f", {len(missing)} piece type(s) missing" if missing else "")
    tex = f", {info['textures']} textures" if info["textures"] else ", flat colours"
    print(f"wrote {out}  ({info['bytes']:,} bytes, {placed} pieces placed, "
          f"{info['meshes']} unique meshes{tex}{extra})")


if __name__ == "__main__":
    main()
