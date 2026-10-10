"""Compare the support points rct3_track predicts with columns read from the game
(live_support_columns.py output), piece by piece, in a tile window.

    python3 scripts/verify/compare_support_columns.py JSON_DIR park.dat cols.json X0 X1 Z0 Z1

JSON_DIR must hold the whole game ovlextract -t track output (under JSON_DIR/full/
by default layout: pass the parent).
"""
import sys, json, math
sys.path.insert(0, 'scripts'); import rct3_track as r
S, savep, colsf = sys.argv[1:4]
x0, x1, z0, z1 = map(int, sys.argv[4:8])
cat = r.Catalog(S + '/full'); d = r.read_design(savep); ride = cat.ride(d['ride'])
OX = OZ = -180.0
cols = {tuple(map(float, k.split(','))): v for k, v in json.load(open(colsf)).items() if len(v) >= 3}
def gt(lx, lz, ox, oz, w, h, dd):
    return [(lx+ox, lz+oz), (lz+ox, (w-lx-1)+oz), ((w-lx-1)+ox, (h-lz-1)+oz), ((h-lz-1)+ox, lx+oz)][dd]
def subt(x, z, dd):
    return [(x, z), (z, 1-x), (1-x, 1-z), (1-z, x)][dd]
used = set()
for p in d['pieces']:
    if not (x0 <= p['x'] <= x1 and z0 <= p['z'] <= z1): continue
    sec = ride.section(p['name'])
    if not sec: continue
    w, h = sec.squares
    print(f"{p['name']:28s} tile {(p['x'], p['z'])} h {p['height']:3d} dir {p['direction']} sq {(w, h)} rev {p['reversed']}")
    for c in sec.data['sid']['connections']:
        if c['start'] != c['end']: continue
        gx, gz = gt(c['start'][0], c['start'][1], p['x'], p['z'], w, h, p['direction'])
        ux, uz = subt(c['start_sub'][2], c['start_sub'][3], p['direction'])
        X, Z = OX + (gx + ux) * 4, OZ + (gz + uz) * 4
        near = [k for k in cols if abs(k[0] - X) < 0.2 and abs(k[1] - Z) < 0.2]
        used.update(near)
        top = p['height'] + c['height']
        print(f"    dt{c['direction_type']} flags {c['flags']} -> ({X:.2f},{Z:.2f}) top {top:.2f}  game column: {[(k, cols[k][-1]) for k in near] or '-'}")
print('game columns not predicted:', [(k, v[-1]) for k, v in cols.items() if k not in used])
