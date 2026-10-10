"""RCT3 tracked-ride logic on top of `ovlextract --types track` output.

Reads the JSON the extractor writes (`<set>.tracks.json` for every track set,
`<ride>.ride.json` for every tracked ride) and answers the two questions a
ride builder needs (docs/RCT3_OVL_FORMAT.md §15):

  * where does a piece go — the exact game transform of a placed piece
    (grid tile, height, 90° direction) and the world pose of its entry /
    exit joints;
  * what may follow it — which pieces of the ride (forward or driven
    backwards) can be attached to a given joint.

Usage:

  # one-off: export every ride / track set to JSON
  ovlextract -t track -o out/track <Assets>/Tracks    (or one OVL at a time)

  python3 scripts/rct3_track.py next  out/track WoodenRC straight
  python3 scripts/rct3_track.py next  out/track WoodenRC medslope --reversed
  python3 scripts/rct3_track.py check out/track '<Assets>/Coaster Designs'/*.trk
  python3 scripts/rct3_track.py show  out/track my_ride.trk

Conventions (all game space = Direct3D left-handed, Y up, metres):
  one grid tile = 4 units, one HEIGHT step = 1 unit; a piece's mesh and
  splines are modelled centred on its footprint and travel along local -X.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

TILE = 4.0
EF = b"\xef\xef\xef\xef"

SLOPES = ["Flat", "MediumUp", "MediumDown", "SteepUp", "SteepDown",
          "VerticalUp", "VerticalDown", "AutoEnd"]
# bank k = roll of k*45 degrees, positive = banked to the right (right rail
# lower). rct3-importer's names for these values are mirrored left/right.
BANKS = ["Flat", "Right45", "Right90", "Right135", "Inverted",
         "Left135", "Left90", "Left45"]
CURVES = ["Straight", "GentleLeft", "MediumLeft", "SharpLeft",
          "GentleRight", "MediumRight", "SharpRight", "Special"]

# Driving a piece backwards swaps up/down and mirrors the bank.
REVERSE_SLOPE = {0: 0, 1: 2, 2: 1, 3: 4, 4: 3, 5: 6, 6: 5, 7: 7}


def reverse_bank(b: int) -> int:
    return (8 - b) % 8


FLAG_START = 0x1
FLAG_FORWARD_ONLY = 0x4
FLAG_STATION = 0x40


# --------------------------------------------------------------------------- #
# catalogue
# --------------------------------------------------------------------------- #
@dataclass
class Section:
    name: str
    data: dict
    source: str = ""

    @property
    def flags(self) -> int:
        return self.data["entry_flags"] | self.data["exit_flags"]

    @property
    def squares(self) -> tuple[int, int]:
        sid = self.data.get("sid") or {}
        sq = sid.get("squares") or [1, 1]
        return int(sq[0]), int(sq[1])

    def joint(self, which: str) -> dict:
        return self.data["geometry"][which]

    def state(self, which: str) -> tuple[int, int]:
        e = self.data[which]
        return e["slope"], e["bank"]

    def groups(self, which: str) -> str:
        return self.data[which].get("group", "")


@dataclass
class Ride:
    name: str
    data: dict
    sections: dict[str, Section] = field(default_factory=dict)   # lower name -> Section

    def section(self, name: str) -> Section | None:
        return self.sections.get(name.lower())


class Catalog:
    """All rides + track sets found in a directory of extractor JSON."""

    def __init__(self, root: Path):
        self.root = Path(root)
        self.sets: dict[str, dict[str, Section]] = {}
        for f in self.root.rglob("*.tracks.json"):
            t = json.loads(f.read_text())
            secs = {s["name"].lower(): Section(s["name"], s) for s in t["sections"]}
            for sec in secs.values():
                sec.source = t.get("source", "")     # the set's OVL; pieces' meshes are <piece>_data next to it
            self.sets[t["track_set"].lower()] = secs
        self.ride_files = {f.name[:-len(".ride.json")].lower(): f
                           for f in self.root.rglob("*.ride.json")}
        self._rides: dict[str, Ride] = {}
        self._splines: dict[str, dict] = {}
        self._set_files = {}
        for f in self.root.rglob("*.tracks.json"):
            self._set_files[f.name[:-len(".tracks.json")].lower()] = f
        self._sid_files = {f.name[:-len(".sids.json")].lower(): f
                           for f in self.root.rglob("*.sids.json")}
        self._train_files = {f.name[:-len(".train.json")].lower(): f
                             for f in self.root.rglob("*.train.json")}

    def splines(self, setname: str) -> dict:
        key = setname.lower()
        if key not in self._splines:
            d = json.loads(self._set_files[key].read_text())
            self._splines[key] = {k.lower(): v for k, v in d.get("splines", {}).items()}
        return self._splines[key]

    def any_spline(self, name: str) -> dict | None:
        """A spline by symbol from anywhere in the catalogue (track sets,
        rides, flat rides, stand-alone <ovl>.splines.json)."""
        if not hasattr(self, "_all_splines"):
            self._all_splines = {}
            for f in self.root.rglob("*.json"):
                if f.name.endswith(".layout.json"):
                    continue
                try:
                    d = json.loads(f.read_text())
                except ValueError:
                    continue
                pool = d.get("splines", d) if isinstance(d, dict) else {}
                for k, v in pool.items():
                    if isinstance(v, dict) and "nodes" in v:
                        self._all_splines.setdefault(k.lower(), v)
        return self._all_splines.get(name.split(":")[0].lower())

    def sid(self, name: str) -> dict | None:
        """A scenery item by symbol from any <ovl>.sids.json / flat ride /
        track set of the catalogue."""
        if not hasattr(self, "_all_sids"):
            self._all_sids = {}
            for f in self._sid_files.values():
                for i in json.loads(f.read_text())["items"]:
                    self._all_sids.setdefault(i["symbol"].lower(), i)
            for secs in self.sets.values():
                for sec in secs.values():
                    if sec.data.get("sid", {}).get("squares"):
                        self._all_sids.setdefault(sec.data["sid"]["symbol"].lower(), sec.data["sid"])
        return self._all_sids.get(name.split(":")[0].lower())

    def platforms(self, style: str) -> dict:
        f = self._sid_files.get(style.lower())
        if not f:
            return {}
        return {i["symbol"].lower(): i for i in json.loads(f.read_text())["items"]}

    def train(self, name: str) -> dict | None:
        f = self._train_files.get(name.lower())
        return json.loads(f.read_text()) if f else None

    def ride(self, name: str) -> Ride:
        key = name.lower()
        if key in self._rides:
            return self._rides[key]
        f = self.ride_files.get(key)
        if not f:
            raise KeyError(f"no {name}.ride.json under {self.root}")
        d = json.loads(f.read_text())
        r = Ride(d["ride"], d)
        pool: dict[str, Section] = {}
        for p in d.get("track_paths", []):
            pool.update(self.sets.get(Path(p.replace("\\", "/")).name.lower(), {}))
        if not pool:                       # fall back to every set
            for s in self.sets.values():
                pool.update(s)
        for n in d["sections"]:
            s = pool.get(n.lower())
            if s:
                r.sections[n.lower()] = s
        self._rides[key] = r
        return r


# --------------------------------------------------------------------------- #
# placement
# --------------------------------------------------------------------------- #
@dataclass
class Placed:
    """A piece as stored in a .trk / park save: SW-corner tile, height step,
    quarter-turn direction."""
    name: str
    x: int
    z: int
    height: int
    direction: int
    distance: float = 0.0


def piece_transform(sec: Section, p: Placed):
    """Return f(local xyz) -> world xyz (game frame) for a placed piece.

    The footprint (sid squares, swapped on odd directions) is centred on the
    anchor tile block, then the piece is turned by -(direction+2)·90° about Y
    (SceneryItem_InitRotationMatrix: angle = (dir+2)·π·-0.5).
    """
    sx, sz = sec.squares
    if p.direction % 2:
        sx, sz = sz, sx
    cx = (p.x + sx / 2) * TILE
    cz = (p.z + sz / 2) * TILE
    a = -(p.direction + 2) * math.pi / 2
    c, s = math.cos(a), math.sin(a)

    def f(v, translate=True):
        x, y, z = v
        w = (x * c - z * s, y, x * s + z * c)
        if translate:
            w = (w[0] + cx, w[1] + p.height, w[2] + cz)
        return w
    return f


def joints(sec: Section, p: Placed, reversed_: bool):
    """World (in_pos, in_dir, out_pos, out_dir) in travel order."""
    f = piece_transform(sec, p)
    a, b = sec.joint("entry"), sec.joint("exit")
    if reversed_:
        a, b = b, a
        sign = -1
    else:
        sign = 1
    return (f(a["pos"]), f([sign * d for d in a["dir"]], False),
            f(b["pos"]), f([sign * d for d in b["dir"]], False))


def travel_state(sec: Section, reversed_: bool, end: str) -> tuple[int, int]:
    """(slope, bank) at the 'in' or 'out' end, in the direction of travel."""
    if not reversed_:
        return sec.state("entry" if end == "in" else "exit")
    slope, bank = sec.state("exit" if end == "in" else "entry")
    return REVERSE_SLOPE[slope], reverse_bank(bank)


# --------------------------------------------------------------------------- #
# connection rules
# --------------------------------------------------------------------------- #
def can_follow(a: Section, a_rev: bool, b: Section, b_rev: bool) -> tuple[bool, str]:
    """May `b` (driven backwards if b_rev) directly follow `a`?

    Joint rules, all of which hold on every one of the shipped designs:
      1. same (slope, bank) on both sides of the joint (travel direction);
      2. a ForwardOnly piece cannot be driven backwards;
      3. the joint headings agree modulo 90° (diagonal 45* pieces only meet
         diagonal pieces) — rotation by whole quarter turns does the rest;
      4. trackgroup tags: a non-empty entry/exit group must match the other
         side's group (stations, lifts, special sequences).
    """
    if b_rev and b.flags & FLAG_FORWARD_ONLY:
        return False, "ForwardOnly"
    if travel_state(a, a_rev, "out") != travel_state(b, b_rev, "in"):
        return False, "slope/bank mismatch"
    ya = _heading(a, a_rev, "out")
    yb = _heading(b, b_rev, "in")
    if (ya is not None and yb is not None and _snapped(ya) and _snapped(yb)
            and _diagonal(ya) != _diagonal(yb)):
        return False, "diagonal mismatch"
    ga = a.groups("entry" if a_rev else "exit")
    gb = b.groups("exit" if b_rev else "entry")
    if (ga or gb) and ga.lower() != gb.lower():
        return False, f"group {ga!r} vs {gb!r}"
    return True, ""


def _snapped(yaw_deg: float) -> bool:
    """Heading within 10° of a multiple of 45° (water-ride splines meander
    and their end tangents say nothing about the grid)."""
    d = yaw_deg % 45
    return min(d, 45 - d) <= 10


def _diagonal(yaw_deg: float) -> bool:
    """True when a joint heading is on a 45° diagonal rather than a grid axis
    (the heading is snapped to the nearest 45°: spline end tangents of curved
    pieces are a few degrees off)."""
    return round(yaw_deg / 45) % 2 == 1


def _heading(sec: Section, rev: bool, end: str):
    which = ("exit" if end == "out" else "entry") if not rev else ("entry" if end == "out" else "exit")
    d = sec.joint(which)["dir"]
    if math.hypot(d[0], d[2]) < 1e-3:       # vertical joint: heading undefined
        return None
    return math.degrees(math.atan2(d[0], d[2]))


def successors(ride: Ride, a: Section, a_rev: bool = False):
    """Every (section, reversed) of the ride that may follow `a`."""
    out = []
    for b in ride.sections.values():
        for b_rev in (False, True):
            ok, _ = can_follow(a, a_rev, b, b_rev)
            if ok:
                out.append((b, b_rev))
    return out


def next_placement(a: Section, pa: Placed, a_rev: bool, b: Section, b_rev: bool) -> Placed | None:
    """Grid placement of `b` so that it attaches to the out-joint of `a`."""
    _, _, out_p, out_d = joints(a, pa, a_rev)
    for direction in range(4):
        probe = Placed(b.name, 0, 0, 0, direction)
        in_p, in_d, _, _ = joints(b, probe, b_rev)
        if math.dist(in_d, out_d) > 0.05:
            continue
        dx, dy, dz = (out_p[i] - in_p[i] for i in range(3))
        x, z, h = dx / TILE, dz / TILE, dy
        if all(abs(v - round(v)) < 0.02 for v in (x, z, h)):
            return Placed(b.name, round(x), round(z), round(h), direction)
    return None


# --------------------------------------------------------------------------- #
# .trk designs
# --------------------------------------------------------------------------- #
def _utf16(buf: bytes, off: int) -> tuple[str, int]:
    out, j = [], off
    while j < len(buf) - 1 and 32 <= buf[j] < 127 and buf[j + 1] == 0:
        out.append(chr(buf[j]))
        j += 2
    return "".join(out), j


def parse_trk(path: Path) -> tuple[str | None, list[Placed]]:
    """(ride name, pieces sorted by StartDistance) of a saved design."""
    buf = Path(path).read_bytes()
    pieces, pos = [], 0
    while True:
        m = buf.find(EF, pos)
        if m < 0:
            break
        pos = m + 4
        sym, end = _utf16(buf, m + 4)
        if not sym.endswith(":tks"):
            continue
        i32 = lambda k: struct.unpack_from("<i", buf, end + k)[0]
        f32 = lambda k: struct.unpack_from("<f", buf, end + k)[0]
        pieces.append(Placed(sym[:-4], i32(20), i32(24), i32(16), i32(12) & 3, f32(48)))
    pieces.sort(key=lambda p: p.distance)
    # The ride type is stored as a UTF-16 "<Ride>:trr" symbol; the "[Ride]"
    # file-name prefix is only a fallback (it is a display name for some
    # rides, e.g. [CoasterBall] = BallCoaster:trr).
    ride = None
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){3,}", buf):
        t = m.group().decode("utf-16le")
        if t.endswith(":trr"):
            ride = t[:-4]
            break
    if ride is None:
        mt = re.match(r"\[([^\]]+)\]", Path(path).name)
        if mt:
            ride = mt.group(1)
    return ride, pieces


def _saves_walk():
    """rct3_saves' object-stream reader (`rct3.stream.Walk`), when the
    sibling repository is around (or on RCT3_SAVES_SRC); else None."""
    import os
    for cand in (os.environ.get("RCT3_SAVES_SRC"),
                 str(Path(__file__).resolve().parent.parent.parent / "rct3_saves" / "src")):
        if cand and Path(cand, "rct3", "stream.py").is_file():
            if cand not in sys.path:
                sys.path.insert(0, cand)
            from rct3.stream import Walk
            return Walk
    return None


def read_design(path: Path) -> dict | None:
    """Everything a .trk / park save says about its (first) tracked ride,
    decoded as objects: pieces in travel order with the stored `Reversed`
    flag, station platforms, entrance / exit placements, the chosen train
    and every FlexiColour triple. None when rct3_saves is not available.

    A .trk is the same object stream as a park save (TrackedRideInstance,
    Track, TrackSegment, TrackPiece, TrackPlatformPiece, RideStations,
    SceneryItemPlacementSingle, SIDDatabaseEntry)."""
    Walk = _saves_walk()
    if Walk is None:
        return None
    w = Walk(Path(path).read_bytes()).run()

    def objs(name):
        try:
            cls = w.cls(name)
        except KeyError:
            return {}
        return {oid: w.values(start, cls) for start, oid, _ in w.find(name)}

    sids = objs("SIDDatabaseEntry")
    tri = next(iter(objs("TrackedRideInstance").values()), {})
    track = next(iter(objs("Track").values()), {})
    pieces = objs("TrackPiece")
    col = lambda d: tuple(d.get(k, 32) for k in ("COL0", "COL1", "COL2")) if d else None
    # a park holds several rides: keep the pieces of this ride's track segments
    tri_id = next(iter(objs("TrackedRideInstance")), None)
    tracks = objs("Track")
    if len(objs("TrackedRideInstance")) > 1 and tracks:
        segs_all = objs("TrackSegment")
        mine = next((t for t in tracks.values()
                     if t.get("TrackedRideInstance") and t["TrackedRideInstance"].id == tri_id), None)
        if mine:
            track = mine
            segs, cur = set(), mine.get("FirstSegment")
            while cur and cur.id in segs_all and cur.id not in segs:
                segs.add(cur.id)
                cur = segs_all[cur.id].get("NextSegment")
            pieces = {k: v for k, v in pieces.items() if v.get("Segment") and v["Segment"].id in segs}
    # travel order: follow Next from the lowest StartDistance
    order, seen = [], set()
    if pieces:
        cur = min(pieces, key=lambda k: pieces[k].get("StartDistance", 0.0))
        while cur in pieces and cur not in seen:
            seen.add(cur)
            order.append(cur)
            nxt = pieces[cur].get("Next")
            cur = nxt.id if nxt else None
        order += [k for k in pieces if k not in seen]
    placed = []
    for oid in order:
        v = pieces[oid]
        d = v["SceneryItemDataField"]
        placed.append({"id": oid, "name": v["SYMBOLNAME"].split(":")[0], "x": d["POSX"], "z": d["POSZ"],
                       "height": d["HEIGHT"], "direction": d["DIRECTION"] & 3,
                       "reversed": bool(v.get("Reversed")), "distance": v.get("StartDistance", 0.0),
                       "colours": col(v.get("FlexiColourField"))})
    def sid_name(ref):
        e = sids.get(ref.id) if ref else None
        return (e or {}).get("SYMBOLNAME", ""), (e or {}).get("OVERLAYFILENAME", "")
    platforms = []
    for v in objs("TrackPlatformPiece").values():
        d = v["SceneryItemDataField"]
        name, ovl = sid_name(v.get("SIDDatabaseEntry"))
        platforms.append({"sid": name, "ovl": ovl, "flags": v.get("Flags", 0), "x": d["POSX"], "z": d["POSZ"],
                          "height": d["HEIGHT"], "direction": d["DIRECTION"] & 3,
                          "piece": v["TrackPiece"].id if v.get("TrackPiece") else None,
                          "colours": col(v.get("FlexiColourField"))})
    singles = []
    # in a park save every ride's entrance / exit is a SceneryItemPlacementSingle:
    # keep the ones owned by this ride's objects
    tri_all = objs("TrackedRideInstance")
    tri_id = next(iter(tri_all), None)
    own = {tri_id} | {r.id for r in (tri.get("RideStationsObj"),) if r}
    for v in objs("SceneryItemPlacementSingle").values():
        if len(tri_all) > 1 and v.get("Owner") and v["Owner"].id not in own:
            continue
        d = v["SceneryItemDataField"]
        name, ovl = sid_name(v.get("SIDDatabaseEntry"))
        singles.append({"sid": name, "ovl": ovl, "x": d["POSX"], "z": d["POSZ"], "height": d["HEIGHT"],
                        "direction": d["DIRECTION"] & 3, "corner": d.get("CORNER", 0),
                        "colours": col(v.get("FlexiColourField"))})
    # every other scenery item, by tile: a support column cannot stand on a
    # tile that holds one (live game, docs §15.8). Track pieces are scenery
    # items too and are left out here.
    scenery = {}
    own_names = {p["name"].lower() for p in placed}
    for v in objs("SceneryItem").values():
        d = v.get("SceneryItemDataField") or {}
        name, _ = sid_name(v.get("DATABASEENTRY"))
        if "POSX" not in d or name.split(":")[0].lower() in own_names:
            continue
        scenery.setdefault((d["POSX"], d["POSZ"]), []).append((name.split(":")[0], d["HEIGHT"]))
    # terrain heights (min, max of the 4 corners) under the ride, for supports
    terrain = {}
    try:
        from rct3.terrain import Terrain
        t = Terrain(Path(path).read_bytes())
        for p in placed:
            for dx in range(-3, 6):
                for dz in range(-3, 6):
                    c, r_ = p["x"] + dx, p["z"] + dz
                    if 0 <= c < t.w and 0 <= r_ < t.h and (c, r_) not in terrain:
                        cs = t.corners(c, r_)
                        terrain[(c, r_)] = (min(cs), max(cs))
    except Exception:
        terrain = {}                       # a .trk has no terrain: ground at 0
    return {"ride": (tri.get("TrackedRideSymbolName") or "").split(":")[0],
            "terrain": terrain, "scenery": scenery,
            "name": tri.get("Name", ""), "train_selection": tri.get("TrainSelection", 0),
            "cars_per_train": tri.get("NCarsPerTrain", 0), "trains": tri.get("NTrains", 0),
            "platform": (tri.get("PlatformStyle"), tri.get("PlatformType")),
            "colours": {"track": col(track.get("TrackFlexiColours")),
                        "car": col(tri.get("CarFlexiColours")),
                        "car_light": col(tri.get("CarLightFlexiColours")),
                        "fence": col(tri.get("FenceFlexiColours")),
                        "queue": col(tri.get("QueueFlexiColours"))},
            "pieces": placed, "platforms": platforms, "items": singles}


def order_by_links(ride: Ride, pieces: list[Placed], tol: float = 0.2) -> list[Placed]:
    """Re-order pieces by following joints, for designs whose StartDistance
    is missing (an untested design stores 0 for every piece). Starts at the
    first station piece (else the first piece) and walks out-joint ->
    in-joint; pieces that cannot be reached are appended in file order."""
    def key(v):
        return tuple(round(c / tol) for c in v)
    ends = []
    by_pos: dict = {}
    for i, p in enumerate(pieces):
        sec = ride.section(p.name)
        if not sec:
            ends.append(None)
            continue
        e = joints(sec, p, False)
        ends.append((e[0], e[2]))
        for k in (key(e[0]), key(e[2])):
            by_pos.setdefault(k, []).append(i)
    start = next((i for i, p in enumerate(pieces)
                  if ride.section(p.name) and ride.section(p.name).flags & FLAG_STATION), 0)
    order, seen = [start], {start}
    cur = ends[start][1] if ends[start] else None
    while cur is not None:
        nxt = [j for j in by_pos.get(key(cur), []) if j not in seen]
        if not nxt:
            break
        j = nxt[0]
        order.append(j)
        seen.add(j)
        a, b = ends[j]
        cur = b if math.dist(a, cur) < math.dist(b, cur) else a
    order += [i for i in range(len(pieces)) if i not in seen]
    return [pieces[i] for i in order]


def solve_orientation(ride: Ride, pieces: list[Placed], tol: float = 0.2):
    """Decide which pieces are driven backwards (the file does not say):
    DP over pieces in distance order maximising matching joints. Returns
    (reversed flags, joint ok flags)."""
    n = len(pieces)
    secs = [ride.section(p.name) for p in pieces]
    J = [[joints(s, p, r) if s else None for r in (False, True)] for s, p in zip(secs, pieces)]

    def link(i, ri, j, rj):
        a, b = J[i][ri], J[j][rj]
        return a is not None and b is not None and math.dist(a[2], b[0]) < tol

    def own(i, r):
        # tie-breakers for pieces whose two orientations put the joints in
        # the same places (stations, straights): never drive a ForwardOnly
        # piece backwards, otherwise prefer forward.
        s = secs[i]
        if not r or s is None:
            return 0.0
        return -5.0 if s.flags & FLAG_FORWARD_ONLY else -0.01

    def edge(i, ri, j, rj):
        if not link(i, ri, j, rj):
            return 0.0
        ok, _ = can_follow(secs[i], bool(ri), secs[j], bool(rj))
        return 10.0 + (2.0 if ok else 0.0)

    # Viterbi over the reversed/forward state of each piece
    score = [[own(0, 0), own(0, 1)]]
    back = [[0, 0]]
    for i in range(1, n):
        row, brow = [], []
        for rj in (0, 1):
            s_, ri = max((score[-1][ri] + edge(i - 1, ri, i, rj), ri) for ri in (0, 1))
            row.append(s_ + own(i, rj))
            brow.append(ri)
        score.append(row)
        back.append(brow)
    r = max((0, 1), key=lambda k: score[-1][k])
    rev = [0] * n
    for i in range(n - 1, -1, -1):
        rev[i] = r
        r = back[i][r]
    ok = [link(i, rev[i], i + 1, rev[i + 1]) for i in range(n - 1)]
    if n > 2:
        ok.append(link(n - 1, rev[n - 1], 0, rev[0]))
    return [bool(x) for x in rev], ok


# --------------------------------------------------------------------------- #
# rail centre line, layout of a whole ride
# --------------------------------------------------------------------------- #
def _bezier(nodes, seg, t):
    a, b = nodes[seg], nodes[(seg + 1) % len(nodes)]
    u = 1 - t
    p0 = a[0]
    p1 = [a[0][k] + a[2][k] for k in range(3)]
    p2 = [b[0][k] + b[1][k] for k in range(3)]
    p3 = b[0]
    return [u * u * u * p0[k] + 3 * u * u * t * p1[k] + 3 * u * t * t * p2[k] + t * t * t * p3[k]
            for k in range(3)]


def sample_spline(sp: dict, n_per_seg: int = 16) -> list[list[float]]:
    nodes = sp["nodes"]
    segs = len(nodes) if sp.get("cyclic") else len(nodes) - 1
    pts = []
    for s_ in range(segs):
        for i in range(n_per_seg + (1 if s_ == segs - 1 else 0)):
            pts.append(_bezier(nodes, s_, i / n_per_seg))
    return pts


class CentreLine:
    """Polyline of a piece's rail mid-line (local frame, entry → exit)."""

    def __init__(self, cat: "Catalog", sec: Section):
        self.left = self.right = None
        for setname, secs in cat.sets.items():
            if secs.get(sec.name.lower()) is sec:
                spl = cat.splines(setname)
                l = spl.get(sec.data["splines"].get("left", "").split(":")[0].lower())
                r = spl.get(sec.data["splines"].get("right", "").split(":")[0].lower())
                self.left = sample_spline(l) if l else None
                self.right = sample_spline(r) if r else None
                break
        if self.left and self.right and len(self.left) == len(self.right):
            self.pts = [[(a[k] + b[k]) / 2 for k in range(3)] for a, b in zip(self.left, self.right)]
            self.rgt = [[b[k] - a[k] for k in range(3)] for a, b in zip(self.left, self.right)]
        else:
            self.pts = self.left or self.right or []
            self.rgt = [[0, 0, 1]] * len(self.pts)
        self.cum = [0.0]
        for a, b in zip(self.pts, self.pts[1:]):
            self.cum.append(self.cum[-1] + math.dist(a, b))

    @property
    def length(self) -> float:
        return self.cum[-1] if self.cum else 0.0

    def at(self, d: float):
        """(pos, forward, right) at arc length d from the entry."""
        d = max(0.0, min(d, self.length))
        i = 0
        while i < len(self.cum) - 2 and self.cum[i + 1] < d:
            i += 1
        span = self.cum[i + 1] - self.cum[i] or 1.0
        t = (d - self.cum[i]) / span
        a, b = self.pts[i], self.pts[i + 1]
        pos = [a[k] + (b[k] - a[k]) * t for k in range(3)]
        fwd = [b[k] - a[k] for k in range(3)]
        rgt = self.rgt[i]
        return pos, _unit(fwd), _unit(rgt)


def _unit(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / n for c in v]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def game_to_gltf_matrix(rot_cols, t):
    """Column-major 4x4 glTF matrix for a game-frame (left-handed) rigid
    transform applied to a mesh that ovlextract already mirrored on Z:
    M = S·[R|t]·S with S = diag(1, 1, -1)."""
    S = [1, 1, -1]
    m = [0.0] * 16
    for c in range(3):
        for r in range(3):
            m[c * 4 + r] = S[r] * rot_cols[c][r] * S[c]
    m[12], m[13], m[14], m[15] = t[0], t[1], -t[2], 1.0
    return m


def piece_matrix(sec: Section, p: Placed):
    f = piece_transform(sec, p)
    o = f((0, 0, 0))
    cols = [[f(e)[k] - o[k] for k in range(3)] for e in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
    return game_to_gltf_matrix(cols, o)


def glb_extent(path: Path) -> tuple[list[float], list[float]] | None:
    """(min, max) of every POSITION accessor in a .glb (glTF frame)."""
    buf = Path(path).read_bytes()
    if buf[:4] != b"glTF":
        return None
    n = struct.unpack_from("<I", buf, 12)[0]
    g = json.loads(buf[20:20 + n])
    lo, hi = [1e9] * 3, [-1e9] * 3
    for m in g.get("meshes", []):
        for pr in m["primitives"]:
            a = g["accessors"][pr["attributes"]["POSITION"]]
            if "min" in a:
                lo = [min(x, y) for x, y in zip(lo, a["min"])]
                hi = [max(x, y) for x, y in zip(hi, a["max"])]
    return (lo, hi) if lo[0] < 1e9 else None


class _Footprint:
    """Duck-typed Section for piece_transform: just the squares."""
    def __init__(self, sx: int, sz: int):
        self.squares = (sx, sz)


# Wall / edge inset (grid units) by sid position type, for the non-centred
# placement modes (rerct3 grid-placement.ts, SceneryItem_ComputeWorldTransform).
_EDGE_INSET = {1: 0.15, 2: 0.05, 3: 0.0, 5: 0.25, 7: 0.0, 8: 0.05}
_QUARTER = [(0.25, 0.75), (0.75, 0.75), (0.25, 0.25), (0.75, 0.25)]


def sid_matrix(sid: dict | None, x: int, z: int, height: int, direction: int, corner: int = 0):
    """glTF matrix of a grid-placed scenery item (platform, entrance, …)."""
    sx, sz = (sid or {}).get("squares") or (1, 1)
    mode = (sid or {}).get("position_type", 0)
    if mode in (0, 6):
        return piece_matrix(_Footprint(sx, sz), Placed("", x, z, height, direction))
    if mode == 4:
        dx, dz = _QUARTER[corner & 3]
    else:
        e = _EDGE_INSET.get(mode, 0.5)
        dx, dz = [(e, 0.5), (0.5, 1 - e), (1 - e, 0.5), (0.5, e)][direction & 3]
    a = -(direction + 2) * math.pi / 2
    c, s_ = math.cos(a), math.sin(a)
    cols = [[c, 0, s_], [0, 1, 0], [-s_, 0, c]]
    return game_to_gltf_matrix(cols, ((x + dx) * TILE, height, (z + dz) * TILE))


def set_support_style(cat: "Catalog", sec: Section) -> str:
    """Fallback support style for a piece whose sid names none: the most
    common style of its track set."""
    for setname, secs in cat.sets.items():
        if secs.get(sec.name.lower()) is sec:
            if not hasattr(cat, "_set_style"):
                cat._set_style = {}
            if setname not in cat._set_style:
                from collections import Counter
                c = Counter((x.data.get("sid") or {}).get("supports", "") for x in secs.values())
                c.pop("", None)
                cat._set_style[setname] = c.most_common(1)[0][0] if c else ""
            return cat._set_style[setname]
    return ""


def _rail_height(cat: "Catalog", sec: Section) -> float:
    """Rail height above a piece's base in its set: the entry height of the
    set's flat straight (1.25 on Track20)."""
    for secs in cat.sets.values():
        if secs.get(sec.name.lower()) is sec:
            for name in ("straight", "stationmiddle", "invstraight"):
                st = secs.get(name)
                if st:
                    return st.joint("entry")["pos"][1]
    return sec.joint("entry")["pos"][1]


def _grid_transform(lx, lz, ox, oz, w, h, d):
    """SceneryItem_GridTransform @0x00961910 (rerct3 RE): local footprint cell
    -> world tile for a placed item."""
    return [(lx + ox, lz + oz), (lz + ox, (w - lx - 1) + oz),
            ((w - lx - 1) + ox, (h - lz - 1) + oz), ((h - lz - 1) + ox, lx + oz)][d & 3]


def _sub_transform(x, z, d):
    """SupportGrid_TransformSubGridFloat @0x009b2210 (rerct3 RE)."""
    return [(x, z), (z, 1 - x), (1 - x, 1 - z), (1 - z, x)][d & 3]


def _style_pieces(cat: "Catalog", style: str) -> dict:
    """The support style's pieces from Supports/Supports.ovl sids:
    middles (type 12, height = sid size y), top (13), bottom (14), girders (16)."""
    if not hasattr(cat, "_style_cache"):
        cat._style_cache = {}
    if style in cat._style_cache:
        return cat._style_cache[style]
    out = {"middle": [], "top": None, "bottom": None, "girder": []}
    for name in ("support_1H", "support_2H", "support_4H", "support_8H", "support_16H",
                 "supporttop_1H", "supportbot_1H", "girder_1L", "girder_2L", "girder_3L"):
        sid = cat.sid(f"{style}_{name}")
        if not sid:
            continue
        asset = _svd(sid, f"{style}_{name}")
        size = sid.get("size") or [0, 1, 0]
        if sid.get("type") == 12:
            out["middle"].append((float(size[1]), asset))
        elif sid.get("type") == 13:
            out["top"] = (float(size[1]), asset)
        elif sid.get("type") == 14:
            out["bottom"] = (float(size[1]), asset)
        elif sid.get("type") == 16:
            out["girder"].append((int(name[-2]), asset))     # length in thirds of a tile? 1L/2L/3L
    out["middle"].sort(reverse=True)
    out["girder"].sort()
    cat._style_cache[style] = out
    return out


def stack_column(bottom: float, attach: float, pieces: dict) -> list[tuple[str, float, float]]:
    """Fill [bottom, attach] with support pieces the way the game does
    (ScenerySupportManager_RebuildCellVisuals, rerct3 RE; measured on the live
    game, Giga lift: footing@0, 1H@1, 2H@2, 4H@4, 8H@8, 8H@16, 4H@24, 2H@28,
    1H@30, top@31 for attach 32). The footing stands on the ground rounded up;
    the top piece starts at round(attach) - 1 and is stretched vertically to
    end exactly at the attach height (scale 0.76 for attach 17.76, 1.37 for
    22.37); the middles fill the gap at multiples of their own height,
    largest first. Returns [(asset, y, y_scale)]."""
    out = []
    lo = math.ceil(bottom - 1e-3)
    hi = math.floor(attach + 0.5)
    if pieces.get("bottom") and hi - lo >= pieces["bottom"][0]:
        out.append((pieces["bottom"][1], lo, 1.0))
        lo += pieces["bottom"][0]
    if pieces.get("top") and hi - lo >= pieces["top"][0]:
        hi -= pieces["top"][0]
        out.append((pieces["top"][1], hi, (attach - hi) / pieces["top"][0]))
    gaps = [(lo, hi)]
    for h, asset in pieces["middle"]:
        nxt = []
        for a, b in gaps:
            i0, i1 = math.ceil(a / h - 1e-6), math.floor(b / h + 1e-6)
            if i0 < i1:
                for i in range(i0, i1):
                    out.append((asset, i * h, 1.0))
                if a < i0 * h - 1e-6:
                    nxt.append((a, i0 * h))
                if i1 * h < b - 1e-6:
                    nxt.append((i1 * h, b))
            else:
                nxt.append((a, b))
        gaps = nxt
    return out


class SupportContext:
    """What a support column must avoid: the tiles covered by other track
    pieces (with the height they start at), and the terrain."""

    def __init__(self, ride: "Ride", pieces: list[Placed], terrain=None, scenery=None):
        self.terrain = terrain or {}
        self.scenery = scenery or {}
        self.occupied: dict[tuple[int, int], list[tuple[int, float]]] = {}
        for i, p in enumerate(pieces):
            sec = ride.section(p.name)
            if not sec:
                continue
            w, h = sec.squares
            for cz in range(h):
                for cx in range(w):
                    t = _grid_transform(cx, cz, p.x, p.z, w, h, p.direction)
                    self.occupied.setdefault(t, []).append((i, p.height))

    def ground(self, tx: int, tz: int) -> float:
        g = self.terrain.get((tx, tz))
        return g[0] if g else 0.0

    def ground_max(self, tx: int, tz: int) -> float:
        g = self.terrain.get((tx, tz))
        return g[1] if g else 0.0

    def blocked(self, tx: int, tz: int, below: float, own: int) -> bool:
        """Another piece covers this tile lower than `below`, or a scenery
        item stands on it (a bush at height 0 is enough in the live game)."""
        if any(h < below for _, h in self.scenery.get((tx, tz), [])):
            return True
        return any(i != own and hb < below - 1.1 for i, hb in self.occupied.get((tx, tz), []))


def support_columns(cat: "Catalog", sec: Section, p: Placed, index: int,
                    ctx: "SupportContext") -> list[dict]:
    """Support structure of one placed piece, from its sid connection entries
    (SceneryItemTrack, 88 B — the game's own support points; docs §15.8).

    Entries come in groups (direction_type). In each group the entry whose end
    cell is its start cell is the point the track rests on: a column from the
    ground straight up to round(attach height). The other entries name
    neighbouring cells: when the tile under the point is taken by another
    piece, the column moves there and a horizontal girder at the attach height
    joins its top to the point. A group whose column cannot stand anywhere, or
    that sits less than 1.1 above the ground, gets no support.
    Returns [{"asset", "matrix"}]."""
    style = (sec.data.get("sid") or {}).get("supports") or set_support_style(cat, sec)
    if not style:
        return []
    pieces = _style_pieces(cat, style)
    if not pieces["middle"]:
        return []
    w, h = sec.squares
    groups: dict[int, list[dict]] = {}
    for c in (sec.data.get("sid") or {}).get("connections") or []:
        groups.setdefault(c["direction_type"], []).append(c)
    ident = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    out = []
    for dt, entries in groups.items():
        main = next((c for c in entries if c["start"] == c["end"]), None)
        if main is None:
            continue
        attach = p.height + main["height"]
        # the game ends the column at round(attach): measured in the live game,
        # attach 30.37 -> 30, 24.37 -> 24, 8.99 -> 9, 32.0 -> 32
        top = math.floor(attach + 0.5)
        sx, sz = _grid_transform(main["start"][0], main["start"][1], p.x, p.z, w, h, p.direction)
        ux, uz = _sub_transform(main["start_sub"][2], main["start_sub"][3], p.direction)
        point = _support_point(cat, sec, p, sx, sz, attach) or ((sx + ux) * TILE, (sz + uz) * TILE)
        candidates = [(sx, sz, point)]
        for c in entries:
            if c is main:
                continue
            ex, ez = _grid_transform(c["end"][0], c["end"][1], p.x, p.z, w, h, p.direction)
            # the column takes the neighbour's sub-cell nearest the support point
            near = lambda base, v: (base + (2 * min(2, max(0, int((v / TILE - base) * 3))) + 1) / 6) * TILE
            candidates.append((ex, ez, (near(ex, point[0]), near(ez, point[1]))))
        for k, (tx, tz, (cx, cz)) in enumerate(candidates):
            g = ctx.ground(tx, tz)
            if top - ctx.ground_max(tx, tz) < 1.1 or ctx.blocked(tx, tz, top, index):
                continue
            # under a girder the column stops GIRDER_DEPTH lower (live game)
            end = attach - (GIRDER_DEPTH if k > 0 and pieces["girder"] else 0.0)
            for asset, y, sy in stack_column(g, end, pieces):
                cols = [[1, 0, 0], [0, sy, 0], [0, 0, 1]]
                out.append({"asset": asset, "matrix": game_to_gltf_matrix(cols, (cx, y, cz))})
            if k > 0 and pieces["girder"]:
                out.append(_girder(pieces, (cx, attach - GIRDER_DEPTH / 2, cz),
                                   (point[0], attach - GIRDER_DEPTH / 2, point[1])))
            break
    return out


def _underside(cat: "Catalog", sec: Section, n: int = 600) -> list:
    """Lowest point of the track's cross-section along the piece (local frame):
    the rail mid-line moved down by the ride's track depth along the banked
    normal, then by 0.25 towards the lower side. The depth is the gap between
    the rails and the support height on the ride's flat Straight (1.18 on the
    Giga). Fitted on the live game (docs §15.8)."""
    if not hasattr(cat, "_under_cache"):
        cat._under_cache = {}
    key = id(sec)
    if key in cat._under_cache:
        return cat._under_cache[key]
    depth = _track_depth(cat, sec)
    cl = CentreLine(cat, sec)
    out = []
    if cl.pts and cl.length > 0:
        for k in range(n + 1):
            pos, fwd, rgt = cl.at(cl.length * k / n)
            up = _unit(_cross(rgt, fwd))
            if up[1] < 0:
                up = [-u for u in up]
            side = _unit(rgt)
            c = [[pos[i] - depth * up[i] + sgn * 0.25 * side[i] for i in range(3)] for sgn in (-1, 1)]
            out.append(min(c, key=lambda v: v[1]))
    cat._under_cache[key] = out
    return out


def _track_depth(cat: "Catalog", sec: Section) -> float:
    """Rail mid-line height above the support attach point on a flat piece."""
    ride = next((r for r in (cat.ride(n) for n in cat.ride_files) if r and r.section(sec.name) is sec), None)
    for name in ("Straight", "Stationmiddle", "Straightchain"):
        st = ride.section(name) if ride else None
        if not st:
            continue
        mains = [c for c in (st.data.get("sid") or {}).get("connections") or [] if c["start"] == c["end"]]
        cl = CentreLine(cat, st)
        if mains and cl.pts:
            return cl.pts[len(cl.pts) // 2][1] - mains[0]["height"]
    return 1.18


def _support_point(cat: "Catalog", sec: Section, p: Placed, sx: int, sz: int, attach: float):
    """Where in tile (sx, sz) the game stands the column (live game, §15.8):
    on the 3x3 sub-grid (centres 1/6, 1/2, 5/6), in the sub-cell where the
    track's underside is at the attach height. When it is there over several
    sub-cells (flat track), the one the track crosses the longest; then the
    one nearest the tile centre."""
    f = piece_transform(sec, p)
    hits = []
    for b in _underside(cat, sec):
        q = f(b)
        fx, fz = q[0] / TILE - sx, q[2] / TILE - sz
        if 0 <= fx < 1 and 0 <= fz < 1:
            hits.append(((int(fx * 3), int(fz * 3)), abs(q[1] - attach)))
    if not hits:
        return None
    best = min(e for _, e in hits)
    count: dict = {}
    for k, e in hits:
        if e <= best + 0.03:
            count[k] = count.get(k, 0) + 1
    i, j = max(count, key=lambda k: (count[k], -(abs(k[0] - 1) + abs(k[1] - 1))))
    return ((sx + (2 * i + 1) / 6) * TILE, (sz + (2 * j + 1) / 6) * TILE)


GIRDER_DEPTH = 0.443      # attach height - column end under a girder (live game)


def _girder(pieces: dict, a, b) -> dict:
    """A horizontal girder joining a column top (a) to the support point (b),
    game frame, a/b at the girder's centre height. Measured on the live game
    (The Money Pit, ts1): the 3L girder (3.66 long along its local X) centred
    between the two, stretched to the gap plus 0.73 (it overhangs both ends):
    gap 4.0 -> X scale 1.292, gap 2.667 -> 0.927."""
    dx, dz = b[0] - a[0], b[2] - a[2]
    dist = math.hypot(dx, dz)
    n, asset = pieces["girder"][-1]
    sc = (dist + 0.73) / 3.66
    ux, uz = (dx / dist, dz / dist) if dist > 1e-6 else (1.0, 0.0)
    # columns: local X -> along the gap (scaled), Y up, Z = X × Y in the game frame
    cols = [[ux * sc, 0, uz * sc], [0, 1, 0], [-uz, 0, ux]]
    mid = ((a[0] + b[0]) / 2, a[1], (a[2] + b[2]) / 2)
    return {"asset": asset, "matrix": game_to_gltf_matrix(cols, mid)}


def _svd(sid: dict | None, fallback: str) -> str:
    svds = (sid or {}).get("svds") or []
    return svds[0].split(":")[0] if svds else fallback


def ride_layout(cat: "Catalog", ride: Ride, pieces: list[Placed], cars: int = 0,
                train: dict | None = None, car_length: float | None = None,
                platform_style: str | None = None, design: dict | None = None,
                supports: bool = False) -> dict:
    """Everything needed to draw the ride: track pieces, station platforms,
    entrance / exit and one train parked in the station, as glTF-space
    matrices, each with the FlexiColour triple it is drawn with.
    Asset names are svd symbols (`ovlextract -t scenery` writes <svd>.glb).

    With `design` (read_design: the .trk / save decoded as objects) the stored
    travel direction, platform pieces, entrance / exit and colours are used;
    without it they are inferred (§15.4, §15.8)."""
    items = []
    if design:
        pieces = [Placed(p["name"], p["x"], p["z"], p["height"], p["direction"], p["distance"])
                  for p in design["pieces"]]
        rev = [p["reversed"] for p in design["pieces"]]
        piece_colours = [p["colours"] for p in design["pieces"]]
    else:
        pieces = ordered_pieces(ride, pieces)
        rev, _ = solve_orientation(ride, pieces)
        piece_colours = [None] * len(pieces)
    for p, col in zip(pieces, piece_colours):
        sec = ride.section(p.name)
        if not sec:
            continue
        svds = (sec.data.get("sid") or {}).get("svds") or []
        if svds:
            items.append({"kind": "track", "piece": sec.name, "asset": svds[0].split(":")[0],
                          "matrix": piece_matrix(sec, p), "colours": col,
                          "ovl_dir": str(Path(sec.source).parent) if sec.source else ""})

    if supports:
        ctx = SupportContext(ride, pieces, (design or {}).get("terrain"), (design or {}).get("scenery"))
        for i, (p, col) in enumerate(zip(pieces, piece_colours)):
            sec = ride.section(p.name)
            if not sec:
                continue
            for it in support_columns(cat, sec, p, i, ctx):
                items.append({"kind": "support", "piece": sec.name, "asset": it["asset"],
                              "matrix": it["matrix"], "colours": col})

    # station runs in travel order
    runs, cur = [], []
    for i, p in enumerate(pieces):
        sec = ride.section(p.name)
        if sec and sec.flags & FLAG_STATION:
            cur.append(i)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        if runs and runs[0][0] == 0 and len(pieces) - 1 == cur[-1]:
            runs[0] = cur + runs[0]          # run wraps around the circuit start
        else:
            runs.append(cur)

    if design:
        for pl in design["platforms"]:
            sid = cat.sid(pl["sid"])
            items.append({"kind": "platform", "piece": pl["sid"], "asset": _svd(sid, pl["sid"]),
                          "matrix": sid_matrix(sid, pl["x"], pl["z"], pl["height"], pl["direction"]),
                          "colours": pl["colours"]})
        for it in design["items"]:
            sid = cat.sid(it["sid"])
            kind = ("entrance" if "entrance" in it["sid"].lower() else
                    "exit" if "exit" in it["sid"].lower() else "scenery")
            items.append({"kind": kind, "piece": it["sid"], "asset": _svd(sid, it["sid"]),
                          "matrix": sid_matrix(sid, it["x"], it["z"], it["height"], it["direction"],
                                               it.get("corner", 0)),
                          "colours": it["colours"]})
    else:
        # one platform piece per station piece, by its place in the run
        style = platform_style or ride.data.get("station_name") or ""
        plat = cat.platforms(style) if style else {}
        for run in runs:
            for k, i in enumerate(run):
                role = ("startEnd" if len(run) == 1 else "start" if k == 0
                        else "end" if k == len(run) - 1 else "middle")
                item = plat.get(f"{style}{role}".lower())
                if item:
                    sec = ride.section(pieces[i].name)
                    items.append({"kind": "platform", "piece": item["symbol"],
                                  "asset": _svd(item, item["symbol"]),
                                  "matrix": piece_matrix(sec, pieces[i]), "colours": None})

    # a train parked nose-first at the end of the first station run
    car_colours = (design or {}).get("colours", {}).get("car")
    if cars and train and runs:
        run = runs[0]
        # the station run, preceded by as many earlier pieces as the train
        # needs (a long train overhangs the station, as in the game)
        clen = (max(car_length.values()) if isinstance(car_length, dict) and car_length
                else car_length if isinstance(car_length, (int, float)) and car_length else 2.0)
        idx = list(run)
        length = sum(CentreLine(cat, ride.section(pieces[i].name)).length for i in idx)
        k = run[0]
        while length < cars * clen and len(idx) < len(pieces):
            k = (k - 1) % len(pieces)
            if not ride.section(pieces[k].name):
                break
            idx.insert(0, k)
            length += CentreLine(cat, ride.section(pieces[k].name)).length
        lines = [(pieces[i], rev[i], CentreLine(cat, ride.section(pieces[i].name))) for i in idx]
        total = sum(cl.length for _, _, cl in lines)
        roles = train["car_roles"]

        def car_of(c):
            role = ("front" if c == 0 else "rear" if c == cars - 1 and roles.get("rear")
                    else "second" if c == 1 and roles.get("second") else "mid")
            name = roles.get(role) or roles.get("mid") or roles.get("front")
            return name, train["cars"].get(name) or next(iter(train["cars"].values()))

        def length_of(car):
            if isinstance(car_length, dict):
                return car_length.get(car["svd"].split(":")[0].lower()) or 2.0
            return car_length or 2.0

        behind = 0.0          # distance from the station end to the current car's front
        for c in range(cars):
            name, car = car_of(c)
            ln = length_of(car)
            d_from_end = behind + 0.5 * ln
            behind += ln
            if d_from_end > total:
                break
            d = total - d_from_end
            for pc, r, cl in lines:
                if d <= cl.length:
                    break
                d -= cl.length
            dl = cl.length - d if r else d
            pos, fwd, rgt = cl.at(dl)
            if r:
                fwd = [-x for x in fwd]
                rgt = [-x for x in rgt]
            up = _cross(fwd, rgt)          # left-handed: up = fwd × right
            sec = ride.section(pc.name)
            f = piece_transform(sec, pc)
            wpos = f(pos)
            wf, wr, wu = (f(v, False) for v in (fwd, rgt, up))
            # car local frame (D3D convention): right = +X, up = +Y, forward = +Z
            cols = [wr, wu, wf]
            items.append({"kind": "car", "piece": name, "asset": car["svd"].split(":")[0],
                          "matrix": game_to_gltf_matrix(cols, wpos), "colours": car_colours})
    return {"ride": ride.name, "items": items}


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _fmt(sec: Section, rev: bool) -> str:
    si, bi = travel_state(sec, rev, "in")
    so, bo = travel_state(sec, rev, "out")
    return (f"{sec.name}{' (reversed)' if rev else ''}: "
            f"{SLOPES[si]}/{BANKS[bi]} -> {SLOPES[so]}/{BANKS[bo]}")


def cmd_next(cat: Catalog, args) -> int:
    ride = cat.ride(args.ride)
    a = ride.section(args.piece)
    if not a:
        print(f"{args.piece} is not a piece of {ride.name}", file=sys.stderr)
        return 1
    print(_fmt(a, args.reversed))
    for b, rev in sorted(successors(ride, a, args.reversed), key=lambda t: (t[1], t[0].name.lower())):
        print("  ->", _fmt(b, rev))
    return 0


def ordered_pieces(ride: Ride, pieces: list[Placed]) -> list[Placed]:
    """Pieces in travel order: StartDistance when the design has one,
    otherwise (all zero) by following the joints."""
    if len(pieces) > 2 and len({round(p.distance, 3) for p in pieces}) < len(pieces) / 2:
        return order_by_links(ride, pieces)
    return pieces


def cmd_check(cat: Catalog, args) -> int:
    tot_j = tot_ok = tot_rule = files = 0
    for path in args.trk:
        name, pieces = parse_trk(Path(path))
        if not pieces:
            continue
        try:
            ride = cat.ride(args.ride or name)
        except KeyError as e:
            print(f"{Path(path).name}: {e}")
            continue
        missing = sorted({p.name for p in pieces if not ride.section(p.name)})
        pieces = ordered_pieces(ride, pieces)
        rev, ok = solve_orientation(ride, pieces)
        rule_bad = []
        for i, good in enumerate(ok):
            if not good:
                continue
            j = (i + 1) % len(pieces)
            a, b = ride.section(pieces[i].name), ride.section(pieces[j].name)
            fine, why = can_follow(a, rev[i], b, rev[j])
            if not fine:
                rule_bad.append(f"{a.name}->{b.name}: {why}")
        files += 1
        tot_j += len(ok)
        tot_ok += sum(ok)
        tot_rule += len(rule_bad)
        flag = "" if all(ok) and not rule_bad else "  <--"
        print(f"{Path(path).name}: {len(pieces)} pieces, joints {sum(ok)}/{len(ok)}, "
              f"reversed {sum(rev)}, rule violations {len(rule_bad)}"
              + (f", unknown pieces {missing}" if missing else "") + flag)
        if args.verbose:
            for r in rule_bad:
                print("    ", r)
    print(f"\n{files} designs: {tot_ok}/{tot_j} joints geometrically connected, "
          f"{tot_rule} connected joints violating the rules")
    return 0


def cmd_show(cat: Catalog, args) -> int:
    name, pieces = parse_trk(Path(args.trk))
    ride = cat.ride(args.ride or name)
    pieces = ordered_pieces(ride, pieces)
    rev, ok = solve_orientation(ride, pieces)
    for i, (p, r) in enumerate(zip(pieces, rev)):
        sec = ride.section(p.name)
        desc = _fmt(sec, r) if sec else p.name + " (unknown)"
        print(f"{i:4d} d={p.distance:8.2f} tile=({p.x},{p.z}) h={p.height:3d} dir={p.direction} "
              f"{'ok ' if i >= len(ok) or ok[i] else 'GAP'} {desc}")
    return 0


def cmd_layout(cat: Catalog, args) -> int:
    name, pieces = parse_trk(Path(args.trk))
    design = None if args.infer else read_design(Path(args.trk))
    if design:
        print(f"design {design['name']!r}: {len(design['pieces'])} pieces, "
              f"{len(design['platforms'])} platforms, {len(design['items'])} entrance/exit, "
              f"colours {design['colours']}")
    ride = cat.ride(args.ride or (design or {}).get("ride") or name)
    trains = ride.data.get("train_names") or [None]
    sel = (design or {}).get("train_selection", 0)
    train_name = args.train or trains[sel if 0 <= sel < len(trains) else 0]
    if design and args.cars is None:
        args.cars = design["cars_per_train"]
    train = cat.train(train_name) if train_name else None
    car_length = args.car_length
    if train and car_length is None and args.glb_dir:
        # The game spaces cars by the car mesh's length along the track
        # (rerct3: Stats.Length from the svd's bounding box); a car points
        # along its local Z (unlike track pieces, which run along -X).
        lens = {}
        for c in train["cars"].values():
            key = c["svd"].split(":")[0]
            hits = sorted(Path(args.glb_dir).rglob(key + ".glb"))
            ext = glb_extent(hits[0]) if hits else None
            if ext:
                lens[key.lower()] = ext[1][2] - ext[0][2]
        car_length = lens or None
        if lens:
            print("car lengths (from the car meshes):", {k: round(v, 3) for k, v in lens.items()})
    lay = ride_layout(cat, ride, pieces, cars=(args.cars or 3) if train else 0, train=train,
                      car_length=car_length, platform_style=args.platform, design=design,
                      supports=args.supports)
    lay["train"] = train_name if train else None
    out = Path(args.output) if args.output else Path(args.trk).with_suffix(".layout.json")
    out.write_text(json.dumps(lay, indent=1))
    kinds = {}
    for it in lay["items"]:
        kinds[it["kind"]] = kinds.get(it["kind"], 0) + 1
    print(f"wrote {out}: {kinds}")
    return 0


def colour_dir(colours) -> str:
    """Sub-directory holding the .glb recoloured with this FlexiColour triple
    ("" = mask colours). Channel values >= 32 mean "no recolour"."""
    if not colours:
        return ""
    c = [v if 0 <= v < 32 else -1 for v in colours]
    return "" if all(v < 0 for v in c) else "c" + "_".join(str(v) for v in c)


def cmd_assets(cat: Catalog, args) -> int:
    """Extract every (svd, colours) a layout needs with ovlextract."""
    import subprocess
    lay = json.loads(Path(args.layout).read_text())
    index = json.loads(Path(args.index).read_text())
    index = index.get("symbols", index)
    out = Path(args.out)
    jobs = {}
    for it in lay["items"]:
        sub = colour_dir(it.get("colours"))
        if (out / sub / f"{it['asset']}.glb").exists():
            continue
        ovl = None
        if it.get("ovl_dir"):
            # a track piece's mesh: <piece>_data.ovl next to its track set (names
            # repeat across sets, so the global index cannot be trusted here)
            want = f"{it['asset'].lower()}_data.common.ovl"
            hit = [f for f in Path(it["ovl_dir"]).glob("*.common.ovl") if f.name.lower() == want]
            if hit:
                ovl = str(hit[0])[:-len(".common.ovl")]
        if ovl is None:
            ent = index.get(f"{it['asset'].lower()}:svd")
            if not ent:
                print("not in the index:", it["asset"], file=sys.stderr)
                continue
            ovl = str(Path(args.assets_root) / ent["ovl"])
        jobs.setdefault((ovl, sub), it.get("colours"))
    for (ovl, sub), colours in sorted(jobs.items()):
        cmd = [args.ovlextract, "-t", "scenery", "--texture-index", args.index,
               "--assets-root", args.assets_root, "-o", str(out / sub)]
        if sub:
            cmd += ["--recolor"] + [str(v if 0 <= v < 32 else -1) for v in colours]
        cmd.append(ovl + ".common.ovl")
        (out / sub).mkdir(parents=True, exist_ok=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        print(f"{Path(ovl).name} {sub or '(mask colours)'}: {r.stdout.strip() or r.stderr.strip()}")
    return 0


def cmd_flatlayout(cat: Catalog, args) -> int:
    """A flat ride (anr): its mesh at the origin and its guest / mechanic
    paths and loop as polylines, all in the glTF frame."""
    f = cat.root.rglob(f"{args.ride}.flatride.json")
    path = next(iter(f), None)
    if not path:
        print(f"no {args.ride}.flatride.json under {cat.root}", file=sys.stderr)
        return 1
    d = json.loads(path.read_text())
    sid = d.get("sid") or {}
    items = []
    if sid.get("svds"):
        ident = game_to_gltf_matrix([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])
        items.append({"kind": "ride", "piece": d["flat_ride"],
                      "asset": sid["svds"][0].split(":")[0], "matrix": ident})
    curves = []
    wanted = ([d["attraction"]["loop_spline"]] if d["attraction"].get("loop_spline") else []) + d["attraction"]["paths"]
    pool = {}
    for w in wanted:
        sp = d.get("splines", {}).get(w.split(":")[0]) or cat.any_spline(w)
        if sp:
            pool[w.split(":")[0]] = sp
    for name, sp in pool.items():
        pts = [[p[0], p[1], -p[2]] for p in sample_spline(sp, 8)]   # mirror Z like the meshes
        kind = ("loop" if name.lower().endswith("loop") else
                "mechanic" if "mech" in name.lower() else "guest")
        curves.append({"name": name, "kind": kind, "points": pts})
    out = Path(args.output or f"{d['flat_ride']}.layout.json")
    out.write_text(json.dumps({"ride": d["flat_ride"], "items": items, "curves": curves}, indent=1))
    print(f"wrote {out}: {len(items)} mesh, {len(curves)} paths")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("next", help="list the pieces that may follow a piece")
    n.add_argument("json_dir")
    n.add_argument("ride")
    n.add_argument("piece")
    n.add_argument("--reversed", action="store_true", help="the piece is driven backwards")
    c = sub.add_parser("check", help="validate saved designs against the rules")
    c.add_argument("json_dir")
    c.add_argument("trk", nargs="+")
    c.add_argument("--ride", help="override the ride name (default: [Ride] prefix of the file name)")
    c.add_argument("-v", "--verbose", action="store_true")
    s = sub.add_parser("show", help="print a design piece by piece")
    s.add_argument("json_dir")
    s.add_argument("trk")
    s.add_argument("--ride")
    l = sub.add_parser("layout", help="placements (glTF matrices) of track, platforms and a train")
    l.add_argument("json_dir")
    l.add_argument("trk")
    l.add_argument("-o", "--output")
    l.add_argument("--ride")
    l.add_argument("--train", help="train (rit) name, default: the ride's first")
    l.add_argument("--cars", type=int, default=None,
                   help="cars in the parked train (default: the design's, else 3)")
    l.add_argument("--supports", action="store_true",
                   help="add approximate support columns (ground at height 0)")
    l.add_argument("--infer", action="store_true",
                   help="ignore the design's stored objects (direction, platforms, colours) "
                        "and infer them from the geometry")
    l.add_argument("--car-length", type=float, default=None,
                   help="spacing between cars (default 2.0; the game uses the car mesh length)")
    l.add_argument("--platform", help="platform style (default: the ride's station name)")
    l.add_argument("--glb-dir", help="ovlextract -t scenery output; car spacing = car mesh length")
    fl = sub.add_parser("flatlayout", help="a flat ride's mesh + guest/mechanic paths")
    fl.add_argument("json_dir")
    fl.add_argument("ride", help="anr name, e.g. BuckingBronco")
    fl.add_argument("-o", "--output")
    a = sub.add_parser("assets", help="extract the .glb a layout needs (ovlextract -t scenery, recoloured)")
    a.add_argument("json_dir")
    a.add_argument("layout")
    a.add_argument("--index", required=True, help="ovlextract --build-index output")
    a.add_argument("--assets-root", required=True)
    a.add_argument("--out", required=True, help="glb directory (colour variants go to c<a>_<b>_<c>/)")
    a.add_argument("--ovlextract", default=str(Path(__file__).resolve().parent.parent / "build" / "cli" / "ovlextract"))
    args = ap.parse_args(argv)
    cat = Catalog(Path(args.json_dir))
    return {"next": cmd_next, "check": cmd_check, "show": cmd_show,
            "layout": cmd_layout, "flatlayout": cmd_flatlayout, "assets": cmd_assets}[args.cmd](cat, args)


if __name__ == "__main__":
    sys.exit(main())
