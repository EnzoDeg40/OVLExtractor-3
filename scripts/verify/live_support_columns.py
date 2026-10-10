"""Ground truth for supports: list the support columns the RUNNING game built.

Each support piece is a scene node whose world matrix ends in (x, y, z, 1.0):
scan the game memory (rct3_saves rct3probe) for (z, 1.0) at every tile row and
sub-position, read x, y, and group by (x, z). The game only builds supports for
the part of the park in view, so aim the camera first (rct3_saves look_at).

    RCT3_PROBE_DIR=/tmp/rct3probe python3 scripts/verify/live_support_columns.py ROW0 ROW1 COL0 COL1 out.json
"""
import sys, struct, collections, json
sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'rct3_saves' / 'tools' / 'mac')); import probe
# columns = several matrices sharing (x, z) whose y include ground-level stacking
z0, z1, x0, x1 = map(int, sys.argv[1:5]); out = sys.argv[5]
OX = OZ = -180.0
found = collections.defaultdict(set)
for row in range(z0, z1 + 1):
    for frac in (1/6, 0.5, 5/6):
        Z = OZ + (row + frac) * 4
        word = struct.unpack('<Q', struct.pack('<ff', Z, 1.0))[0]
        (ans,) = probe.send(f'scan {word:#x} 3000', timeout=180)
        hits = [int(a, 16) for a in ans.split()]
        heads = probe.read_many([a - 8 for a in hits], 8)
        for a, b in zip(hits, heads):
            if not b: continue
            x, y = struct.unpack('<ff', b)
            if OX + x0 * 4 <= x <= OX + (x1 + 1) * 4:
                found[(round(x, 3), round(Z, 3))].add(round(y, 3))
cols = {f'{k[0]},{k[1]}': sorted(v) for k, v in found.items()}
json.dump(cols, open(out, 'w'), indent=0)
for k, v in sorted(cols.items()): print(k, v[:16])
