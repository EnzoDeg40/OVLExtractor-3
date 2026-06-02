"""Reconstruct an RCT3 saved coaster (.trk) as a glTF/GLB scene, placing the
track-piece meshes extracted by `ovlextract --types model`.

Usage (from the project root, after extracting the coaster's track-piece set
into out/models/tracks/coasters/<set>/):

  python3 scripts/trk_to_gltf.py '[WoodenRC]MR_compact.trk'
  python3 scripts/trk_to_gltf.py ride.trk -o ride.glb --models out/models
  python3 scripts/trk_to_gltf.py ride.trk --set Track20 --lod ME --report

Flags:
  -o, --output PATH   output .glb (default: <trk basename>.glb next to the .trk)
  --models DIR        extracted-models root (default: out/models)
  --set NAME          coaster piece set, e.g. Track20 (default: auto-detected
                      from the Tracks\\Coasters\\<set>\\ references in the .trk)
  --lod HI|ME|LO      level of detail to place (default: HI)
  --report            print the piece-count histogram

How it works
------------
A .trk stores its placed pieces as packed records in the file tail: an
`ef ef ef ef` marker, a UTF-16LE `name:tks` symbol, then five int32 —
CORNER, DIRECTION, HEIGHT, POSX, POSZ — at +8/+12/+16/+20/+24 from the symbol
end (same field order as the part-1 `SceneryItemDataField` struct). The visual
mesh of each piece is <models>/tracks/coasters/<set>/<name>_data/<name>_<lod>.obj.

World convention, calibrated from the meshes: 1 grid tile = 4 mesh units,
1 HEIGHT level = 1 unit, DIRECTION = 90-degree yaw steps, Y is up — already
glTF's axis convention, so every piece becomes a node with a translation and a
90-degree-step rotation quaternion. Pure standard library, no dependencies.

See docs/TRK_FORMAT.md for the full format notes.
"""

from __future__ import annotations

import argparse
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
    """Read a printable UTF-16LE run at off; return (text, end_offset)."""
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
# OBJ loading (positions + normals, triangulated, sub-meshes merged)
# --------------------------------------------------------------------------- #
def find_obj(models: Path, name: str, sets: list[str], lod: str) -> Path | None:
    """Locate <name>_data/<name>_<lod>.obj, trying the given sets then any."""
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


def load_obj(path: Path):
    """Return (positions[float*3], normals[float*3], indices[int]) de-indexed."""
    verts: list[tuple[float, float, float]] = []
    norms: list[tuple[float, float, float]] = []
    faces: list[list[tuple[int, int]]] = []
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("v "):
            p = line.split()
            verts.append((float(p[1]), float(p[2]), float(p[3])))
        elif line.startswith("vn "):
            p = line.split()
            norms.append((float(p[1]), float(p[2]), float(p[3])))
        elif line.startswith("f "):
            ref = []
            for w in line.split()[1:]:
                a = (w.split("/") + ["", "", ""])[:3]
                vi = int(a[0]) - 1
                ni = int(a[2]) - 1 if a[2] else -1
                ref.append((vi, ni))
            for k in range(1, len(ref) - 1):          # triangulate fan
                faces.append([ref[0], ref[k], ref[k + 1]])

    uniq: dict[tuple[int, int], int] = {}
    pos: list[float] = []
    nrm: list[float] = []
    idx: list[int] = []
    for tri in faces:
        for vi, ni in tri:
            key = (vi, ni)
            j = uniq.get(key)
            if j is None:
                j = uniq[key] = len(pos) // 3
                pos.extend(verts[vi])
                nrm.extend(norms[ni] if 0 <= ni < len(norms) else (0.0, 1.0, 0.0))
            idx.append(j)
    return pos, nrm, idx


# --------------------------------------------------------------------------- #
# GLB assembly (hand-written, stdlib only)
# --------------------------------------------------------------------------- #
def quat_y(direction: int) -> list[float]:
    a = math.radians(90.0 * direction) / 2.0
    return [0.0, math.sin(a), 0.0, math.cos(a)]


def build_glb(pieces, meshes: dict, out: Path) -> dict:
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

    gltf_meshes: dict[str, int] = {}
    mesh_list: list[dict] = []
    for name, (pos, nrm, idx) in meshes.items():
        n = len(pos) // 3
        mn = [min(pos[i::3]) for i in range(3)]
        mx = [max(pos[i::3]) for i in range(3)]
        ap = add_accessor(add_view(struct.pack(f"<{len(pos)}f", *pos), 34962),
                          "VEC3", n, 5126, mn, mx)
        an = add_accessor(add_view(struct.pack(f"<{len(nrm)}f", *nrm), 34962),
                          "VEC3", n, 5126)
        ai = add_accessor(add_view(struct.pack(f"<{len(idx)}I", *idx), 34963),
                          "SCALAR", len(idx), 5125)
        gltf_meshes[name] = len(mesh_list)
        mesh_list.append({"name": name, "primitives": [
            {"attributes": {"POSITION": ap, "NORMAL": an}, "indices": ai, "material": 0}]})

    nodes = []
    for p in pieces:
        if p["name"] not in gltf_meshes:
            continue
        nodes.append({
            "name": f"{p['name']}@{p['posx']},{p['height']},{p['posz']}",
            "mesh": gltf_meshes[p["name"]],
            "translation": [p["posx"] * TILE, p["height"] * HUNIT, p["posz"] * TILE],
            "rotation": quat_y(p["direction"]),
        })

    gltf = {
        "asset": {"version": "2.0", "generator": "OVLExtractor trk_to_gltf"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": mesh_list,
        "materials": [{
            "name": "Track",
            "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.40, 0.25, 1.0],
                                     "metallicFactor": 0.0, "roughnessFactor": 0.85},
            "doubleSided": True,
        }],
        "bufferViews": views,
        "accessors": accessors,
        "buffers": [{"byteLength": len(blob)}],
    }

    jchunk = json.dumps(gltf, separators=(",", ":")).encode()
    jchunk += b" " * (-len(jchunk) % 4)
    bchunk = bytes(blob) + b"\x00" * (-len(blob) % 4)
    total = 12 + 8 + len(jchunk) + 8 + len(bchunk)
    glb = bytearray()
    glb += struct.pack("<III", 0x46546C67, 2, total)
    glb += struct.pack("<II", len(jchunk), 0x4E4F534A) + jchunk
    glb += struct.pack("<II", len(bchunk), 0x004E4942) + bchunk
    out.write_bytes(glb)
    return {"nodes": len(nodes), "meshes": len(mesh_list), "bytes": len(glb)}


# --------------------------------------------------------------------------- #
def main() -> None:
    ap = argparse.ArgumentParser(description="Reconstruct an RCT3 .trk coaster as glTF/GLB.")
    ap.add_argument("trk", type=Path, help="input .trk file")
    ap.add_argument("-o", "--output", type=Path, help="output .glb (default: <trk>.glb)")
    ap.add_argument("--models", type=Path, default=Path("out/models"),
                    help="extracted-models root (default: out/models)")
    ap.add_argument("--set", dest="setname", help="coaster piece set (default: auto)")
    ap.add_argument("--lod", default="HI", choices=["HI", "ME", "LO"], help="LOD (default: HI)")
    ap.add_argument("--report", action="store_true", help="print piece histogram")
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

    out = args.output or args.trk.with_suffix(".glb")
    info = build_glb(pieces, meshes, out)
    placed = sum(1 for p in pieces if p["name"] in meshes)
    print(f"wrote {out}  ({info['bytes']:,} bytes, {placed} pieces placed, "
          f"{info['meshes']} unique meshes"
          + (f", {len(missing)} piece type(s) missing" if missing else "") + ")")


if __name__ == "__main__":
    main()
