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
            self.sets[t["track_set"].lower()] = {
                s["name"].lower(): Section(s["name"], s) for s in t["sections"]}
        self.ride_files = {f.name[:-len(".ride.json")].lower(): f
                           for f in self.root.rglob("*.ride.json")}
        self._rides: dict[str, Ride] = {}

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
    args = ap.parse_args(argv)
    cat = Catalog(Path(args.json_dir))
    return {"next": cmd_next, "check": cmd_check, "show": cmd_show}[args.cmd](cat, args)


if __name__ == "__main__":
    sys.exit(main())
