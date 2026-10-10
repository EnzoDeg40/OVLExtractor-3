"""Compare the support pieces rct3_track predicts with the ones the RUNNING game built.

Every scene node the game draws has a world matrix in memory (16 floats, last
column 0,0,0,1). The probe's `mats` command (rct3_saves rct3probe) lists all of
them inside a box; each predicted support piece (column part or girder) must
match one by translation. The game only builds supports for the part of the
park in view: aim the camera there first (rct3_saves Save.look_at).

    RCT3_PROBE_DIR=/tmp/rct3probe python3 scripts/verify/live_supports.py JSON_DIR park.dat X0 X1 Z0 Z1 [--where]

JSON_DIR: `ovlextract -t track` output of the whole game. X0..Z1: tile window.
--where only prints where the predicted girders are (no game needed).
"""
import collections
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parents[3] / "rct3_saves" / "src"))
sys.path.insert(0, str(HERE.parents[3] / "rct3_saves" / "tools" / "mac"))
import rct3_track as r  # noqa: E402

NODE_CLASS = 0x100D28F50   # RCT3_Exe 3.3.4 (Mac): first class pointer of a drawn scene node


def predicted(json_dir, save):
    cat = r.Catalog(Path(json_dir))
    d = r.read_design(save)
    ride = cat.ride(d["ride"])
    lay = r.ride_layout(cat, ride, [], design=d, supports=True)
    out = []
    for it in lay["items"]:
        if it["kind"] != "support":
            continue
        m = it["matrix"]
        # glTF column-major, Z mirrored: back to the game frame (layout origin)
        out.append({"asset": it["asset"], "piece": it["piece"], "t": (m[12], m[13], -m[14]),
                    "x_axis": (m[0], m[1], -m[2])})
    return out, d


def live(box):
    import probe
    (ans,) = probe.send("mats %g %g %g %g 400000" % box, timeout=300)
    out = []
    for rec in ans.split(";"):
        f = rec.split()
        if len(f) != 13:
            continue
        v = list(map(float, f[1:]))
        out.append({"addr": f[0], "rows": (v[0:3], v[3:6], v[6:9]), "t": tuple(v[9:12])})
    # keep real scene nodes: the Mac build's node class pointers sit 0x70 before
    # the world matrix, the model 0x58 before; the model's +0x2c float is the
    # mesh height (1, 2, 4, 8 for support pieces)
    heads = probe.read_many([int(m["addr"], 16) - 0x80 for m in out], 0x30)
    nodes = []
    for m, h in zip(out, heads):
        if not h:
            continue
        q = struct.unpack("<6Q", h)
        if q[2] != NODE_CLASS:
            continue
        m["model"] = q[5]
        nodes.append(m)
    models = sorted({m["model"] for m in nodes})
    info = dict(zip(models, probe.read_many(models, 0x30)))
    for m in nodes:
        b = info.get(m["model"])
        m["height"] = struct.unpack_from("<f", b, 0x2C)[0] if b else None
    return nodes


def main(argv):
    json_dir, save = argv[0], argv[1]
    x0, x1, z0, z1 = map(int, argv[2:6])
    from rct3.save import Save
    ox, oz = Save.open(save).t.origin[:2]
    pred, d = predicted(json_dir, save)
    T = r.TILE
    inbox = lambda x, z: x0 * T <= x <= (x1 + 1) * T and z0 * T <= z <= (z1 + 1) * T
    pred = [p for p in pred if inbox(p["t"][0], p["t"][2])]
    if "--where" in argv:
        for p in pred:
            if "girder" in p["asset"].lower():
                print(p["piece"], "girder at tile", (int(p["t"][0] // T), int(p["t"][2] // T)), "h", round(p["t"][1], 2))
        return 0
    mats = live((ox + x0 * T - 1, ox + (x1 + 1) * T + 1, oz + z0 * T - 1, oz + (z1 + 1) * T + 1))
    for m in mats:
        m["l"] = (m["t"][0] - ox, m["t"][1], m["t"][2] - oz)
    print(f"{len(pred)} predicted support pieces in the window, {len(mats)} live matrices")
    used = set()
    ok = collections.Counter()
    for p in sorted(pred, key=lambda p: (p["t"][0], p["t"][2], p["t"][1])):
        best = min(mats, key=lambda m: sum((a - b) ** 2 for a, b in zip(m["l"], p["t"])), default=None)
        dist = math.dist(best["l"], p["t"]) if best else 1e9
        hit = dist < 0.06
        kind = "girder" if "girder" in p["asset"].lower() else "column"
        ok[(kind, hit)] += 1
        if hit:
            used.add(best["addr"])
        if not hit or kind == "girder":
            print(f"  {'OK ' if hit else 'MISS'} {kind:6s} {p['asset']:28s} {p['piece']:26s} at "
                  f"({p['t'][0]:.2f},{p['t'][1]:.2f},{p['t'][2]:.2f})  nearest live {dist:.2f} "
                  f"{tuple(round(c, 2) for c in best['l']) if best else ''}")
    print(dict(ok))
    # live column-like stacks nobody predicted: >= 3 axis-aligned unit matrices on one (x, z)
    stacks = collections.defaultdict(list)
    for m in mats:
        if m["addr"] in used:
            continue
        R = m["rows"]
        if all(abs(R[i][j] - (i == j)) < 1e-3 for i in range(3) for j in range(3)):
            stacks[(round(m["l"][0], 2), round(m["l"][2], 2))].append(round(m["l"][1], 2))
    extra = {k: sorted(set(v)) for k, v in stacks.items() if len(set(v)) >= 3}
    print("live stacks not predicted:")
    for k, v in sorted(extra.items()):
        print("  ", k, "tile", (int(k[0] // T), int(k[1] // T)), v[:12])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
