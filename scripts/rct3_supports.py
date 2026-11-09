"""Support structures of a placed ride, the way RCT3 builds them.

Decoded from the Mac build (Ghidra, 2026-11-09; notes in docs §15.8 and
ghidra/notes/rebuild_cell_visuals.md, track_build_supports.md). A track piece
is an ordinary scenery item for supports: its sid decides everything.

- sid.flags & 0x80: the item has supports. & 0x400: connection mode (the
  88-byte entries, steel sets), else per-cell mode (one column at the centre
  of each footprint square with flag bit 1: wooden, water and a few others).
- & 0x200: checkerboard, a column only on tiles where (x ^ z) is odd (the
  "one column out of two" on runs of one-tile pieces). & 0x800: the style's
  cap piece is put at the attach point.
- Connection entries come in groups (+0x54): the first entry of a group, in
  array order, that can stand wins. An entry stands its column straight under
  the attach point when the attach point is near its sub-cell centre, not
  inverted and not angled; otherwise the column goes on the end sub-cell and a
  girder joins it to the attach point.
- A column's bottom is the terrain (ceil of the tile's lowest corner) or the
  item under it; another piece of track below blocks it unless the style may
  stand on items (support piece sid flag 0x4000/0x8000, the wooden ts2).

Everything is computed in the game frame (left-handed, Y up, a tile is 4
units), then turned into glTF matrices like the rest of rct3_track.
"""
from __future__ import annotations

import math
from pathlib import Path

TILE = 4.0
MAX_COLUMN = 0xE4                 # CreateColumnRecord rejects top - bottom above this
NEAR_CENTRE_SQ = 0.04             # start fraction within 0.2 of its sub-cell centre: vertical column

# Piece classes (sid type) of a support style.
MIDDLE, TOP, BOTTOM, BOTTOM_EXTRA, GIRDER, CAP = 12, 13, 14, 15, 16, 17

# Shape sizes measured on ts1 when the meshes are not at hand (docs §15.8).
DEFAULT_SHAPE = {"girder": {"min": [-1.827, -0.223, -0.36], "max": [1.827, 0.223, 0.36]},
                 "top_half_width": 0.36, "cap_height": 0.35}


def game_to_gltf_matrix(rot_cols, t):
    S = [1, 1, -1]
    m = [0.0] * 16
    for c in range(3):
        for r in range(3):
            m[c * 4 + r] = S[r] * rot_cols[c][r] * S[c]
    m[12], m[13], m[14], m[15] = t[0], t[1], -t[2], 1.0
    return m


def grid_transform(lx, lz, ox, oz, w, h, d):
    """SceneryItem_GridTransform: local footprint cell -> world tile."""
    return [(lx + ox, lz + oz), (lz + ox, (w - lx - 1) + oz),
            ((w - lx - 1) + ox, (h - lz - 1) + oz), ((h - lz - 1) + ox, lx + oz)][d & 3]


def rotate_sub(sx, sz, d):
    """A 3x3 sub-cell index turned with its item (RebuildCellVisuals)."""
    return [(sx, sz), (sz, 2 - sx), (2 - sx, 2 - sz), (2 - sz, sx)][d & 3]


def unrotate_sub(wx, wz, d):
    return [(wx, wz), (2 - wz, wx), (2 - wx, 2 - wz), (wz, 2 - wx)][d & 3]


def rotate_frac(fx, fz, d):
    return [(fx, fz), (fz, 1 - fx), (1 - fx, 1 - fz), (1 - fz, fx)][d & 3]


def column_direction(d: int, rot: int) -> int:
    """A square's support direction type turned with its item (FUN_1009a3f78)."""
    if 2 <= d <= 5:
        d += rot & 3
        if d >= 6:
            d -= 4
    elif d in (6, 7) and rot & 1:
        d = 13 - d
    return d


def piece_turn(d: int, p: int) -> int | None:
    """Quarter turns of a support piece of direction class p on a column of
    direction d, or None when the piece does not fit that column."""
    if 2 <= p <= 5:
        return (d - p) % 4 if 2 <= d <= 5 else None
    if p in (6, 7):
        return int(d != p) if d in (6, 7) else None
    if p == 1:
        return 0 if d == 1 else None
    return None


def turned_matrix(x, y, z, turns=0, scale_y=1.0):
    a = -(turns + 2) * math.pi / 2
    c, s = math.cos(a), math.sin(a)
    return game_to_gltf_matrix([[c, 0, s], [0, scale_y, 0], [-s, 0, c]], (x, y, z))


class Style:
    """A support style's pieces (Supports/Supports.ovl sids) and shape sizes."""

    def __init__(self, cat, name: str, glb_dir: Path | None):
        self.name = name
        self.pieces = {MIDDLE: [], TOP: [], BOTTOM: [], BOTTOM_EXTRA: [], GIRDER: [], CAP: []}
        self.stand_on_items = False      # SupportEntry +0x74
        self.no_track_gap = False        # +0x75: no 1-unit gap above a track piece below
        prefix = name.lower() + "_"
        cat.sid(name)                          # builds the catalogue's sid index
        for sym, sid in getattr(cat, "_all_sids", {}).items():
            if not sym.startswith(prefix) or sid.get("type") not in self.pieces:
                continue
            svds = sid.get("svds") or []
            asset = svds[0].split(":")[0] if svds else sid["symbol"]
            cells = sid.get("cells") or [[0, 0, 1, 1]]
            height = float((sid.get("size") or [0, 1, 0])[1])
            self.pieces[sid["type"]].append({"asset": asset, "height": height, "dir": cells[0][3]})
            if sid.get("flags", 0) & 0x8000:
                self.stand_on_items = self.no_track_gap = True
            elif sid.get("flags", 0) & 0x4000:
                self.stand_on_items = True
        for k in (MIDDLE, TOP, BOTTOM, BOTTOM_EXTRA):
            self.pieces[k].sort(key=lambda p: p["height"])
        self.top_half_width = DEFAULT_SHAPE["top_half_width"]
        self.cap_height = DEFAULT_SHAPE["cap_height"]
        self.girder_shape = DEFAULT_SHAPE["girder"]
        for g in self.pieces[GIRDER]:
            g["shape"] = self._extent(glb_dir, g["asset"]) or DEFAULT_SHAPE["girder"]
            g["thickness"] = g["shape"]["max"][1] - g["shape"]["min"][1]
        self.pieces[GIRDER].sort(key=lambda g: g["thickness"])
        if self.pieces[TOP]:
            e = self._extent(glb_dir, self.pieces[TOP][0]["asset"])
            if e:
                self.top_half_width = max(e["max"][0], e["max"][2], -e["min"][2])
        if self.pieces[CAP]:
            e = self._extent(glb_dir, self.pieces[CAP][0]["asset"])
            if e:
                self.cap_height = e["max"][1] - e["min"][1]

    @staticmethod
    def _extent(glb_dir, asset):
        if not glb_dir:
            return None
        from rct3_track import glb_extent
        for f in (Path(glb_dir) / f"{asset}.glb", *Path(glb_dir).glob(f"**/{asset}.glb")):
            if f.exists():
                e = glb_extent(f)
                if e:
                    lo, hi = e
                    return {"min": lo, "max": hi}
        return None

    @property
    def usable(self) -> bool:
        return bool(self.pieces[MIDDLE])

    @property
    def girder(self):
        # The game picks "the first girder whose thickness >= span", which no span ever
        # meets: in practice always the last one after sorting by thickness.
        return self.pieces[GIRDER][-1] if self.pieces[GIRDER] else None

    @property
    def girder_depth(self) -> float:
        return self.pieces[GIRDER][0]["thickness"] if self.pieces[GIRDER] else 0.0


class Item:
    """One placed piece as the support code sees it."""

    def __init__(self, index, sec, p):
        self.index, self.sec, self.p = index, sec, p
        self.sid = sec.data.get("sid") or {}
        self.flags = self.sid.get("flags", 0)
        self.w, self.h = self.sid.get("squares") or [1, 1]
        self.base = p.height

    def square(self, tx, tz):
        """The footprint square covering world tile (tx, tz), or None."""
        cells = self.sid.get("cells") or []
        for lz in range(self.h):
            for lx in range(self.w):
                if grid_transform(lx, lz, self.p.x, self.p.z, self.w, self.h, self.p.direction) == (tx, tz):
                    i = lz * self.w + lx
                    return cells[i] if i < len(cells) else None
        return None


class World:
    """What a column meets on its way down: the pieces under it, the terrain
    and the scenery of a park save."""

    def __init__(self, items, terrain=None, scenery=None):
        self.terrain = terrain or {}
        self.scenery = scenery or {}
        self.by_tile: dict = {}
        for it in items:
            for lz in range(it.h):
                for lx in range(it.w):
                    t = grid_transform(lx, lz, it.p.x, it.p.z, it.w, it.h, it.p.direction)
                    self.by_tile.setdefault(t, []).append(it)
        self.sub_used: dict = {}        # tile -> set of sub-cells with a column (shared by entries)

    def corners(self, tx, tz):
        g = self.terrain.get((tx, tz))
        return (g[0], g[1]) if g else (0.0, 0.0)

    def bottom(self, tx, tz, sub, above: Item, style: Style, connection: bool, top: float | None = None):
        """FUN_1009a469c: where a column on (tile, sub-cell) starts, or None if blocked.
        Returns (bottom, on_ground)."""
        # The cell list runs from the top down: what lies under the column's top is "below" it,
        # even when it stands higher than the item's own base (a bush under a high attach point).
        limit = above.base if top is None else top
        lower = sorted((it for it in self.by_tile.get((tx, tz), []) if it is not above and it.base < limit),
                       key=lambda it: -it.base)
        for it in lower:
            sq = it.square(tx, tz)
            if sq is None:
                continue
            if connection and it.flags & 0x400:
                lx, lz = unrotate_sub(sub[0], sub[1], it.p.direction)
                if not sq[0] & (1 << (3 + lx + 3 * lz)):
                    continue                         # the column passes beside it
            if not (style.stand_on_items or it.flags & 0x100):
                return None
            b = it.base + sq[2]
            if it.sid.get("type") == 0x12 and not style.no_track_gap:
                b += 1
            return b, False
        if any(h < limit for _, h in self.scenery.get((tx, tz), [])):
            return None
        tmin, _ = self.corners(tx, tz)
        return math.ceil(tmin - 1e-6), True


def checkerboard_ok(item: Item, tx, tz) -> bool:
    return bool((tx ^ tz) & 1) or not item.flags & 0x200


def stack(style: Style, record: dict) -> list[dict]:
    """Second half of RebuildCellVisuals: the pieces of one column record."""
    out = []
    d, (tx, tz), (sx, sz) = record["dir"], record["tile"], record["sub"]
    x = (tx + (2 * sx + 1) / 6) * TILE
    z = (tz + (2 * sz + 1) / 6) * TILE

    def place(piece, y, scale_y=1.0):
        out.append({"asset": piece["asset"], "matrix": turned_matrix(x, y, z, piece_turn(d, piece["dir"]), scale_y)})

    def fits(piece):
        return piece_turn(d, piece["dir"]) is not None

    cur, rem = record["bottom"], record["top"] - record["bottom"]
    first, second = (BOTTOM_EXTRA, BOTTOM) if record["on_ground"] else (BOTTOM, BOTTOM_EXTRA)
    for kind in (first, second):
        placed = False
        for piece in reversed(style.pieces[kind]):
            if fits(piece) and piece["height"] <= rem:
                place(piece, cur)
                cur += piece["height"]
                rem -= piece["height"]
                placed = True
        if placed:
            break
    for piece in reversed(style.pieces[TOP]):
        if fits(piece) and piece["height"] <= rem:
            rem -= piece["height"]
            y = cur + rem
            place(piece, y, (record["top_f"] - y) / piece["height"] if record["stretch"] else 1.0)
    gaps = [(cur, cur + rem)]
    for piece in reversed(style.pieces[MIDDLE]):
        if not fits(piece):
            continue
        h = piece["height"]
        nxt = []
        for a, b in gaps:
            i0, i1 = math.ceil(a / h - 1e-6), math.floor(b / h + 1e-6)
            if i1 > i0:
                for i in range(i0, i1):
                    place(piece, i * h)
                if a < i0 * h - 1e-6:
                    nxt.append((a, i0 * h))
                if i1 * h < b - 1e-6:
                    nxt.append((i1 * h, b))
            else:
                nxt.append((a, b))
        gaps = nxt
    return out


def girder_matrix(style: Style, g: dict, p0, p1, inverted: bool):
    """PlaceAngledGirder (FUN_1009a4e34): one stretched girder from p0 to p1."""
    dx, dy, dz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
    flat = math.hypot(dx, dz)
    length = math.sqrt(flat * flat + dy * dy)
    if length < 1e-6:
        return None
    ux, uz = (dx / flat, dz / flat) if flat > 1e-9 else (1.0, 0.0)
    cp, sp = flat / length, dy / length
    ex = (ux * cp, sp, uz * cp)
    ey = (-ux * sp, cp, -uz * sp)
    ez = (-uz, 0.0, ux)
    shape = g["shape"]
    s = (length + 2 * style.top_half_width) / (shape["max"][0] - shape["min"][0])
    off = -(shape["min"][1] if inverted else shape["max"][1])
    pos = [(p0[k] + p1[k]) / 2 + off * ey[k] for k in range(3)]
    return game_to_gltf_matrix([[e * s for e in ex], list(ey), list(ez)], pos)


def item_supports(cat, item: Item, world: World, style: Style, records_out: list | None = None,
                  checkerboard: bool = True) -> list[dict]:
    """First half of RebuildCellVisuals for one item: column records and girders.
    Returns the placed pieces; `records_out`, when given, also receives the column records
    (tile, sub-cell, bottom, top, top_f, dir, start tile) and each girder/cap entry is tagged.
    `checkerboard=False` keeps the columns the checkerboard would drop (for a caller that applies it
    itself, once the piece's real tile is known)."""
    out = []
    sid, p = item.sid, item.p
    if not item.flags & 0x80 or not style.usable:
        return out
    cells = sid.get("cells") or []
    records = []

    def square_dir(lx, lz):
        i = lz * item.w + lx
        return column_direction(cells[i][3], p.direction) if i < len(cells) else 1

    if not item.flags & 0x400:
        # Per-cell mode: one column at the centre of each square that asks for one.
        for lz in range(item.h):
            for lx in range(item.w):
                i = lz * item.w + lx
                if i >= len(cells) or not cells[i][0] & 2:
                    continue
                tx, tz = grid_transform(lx, lz, p.x, p.z, item.w, item.h, p.direction)
                if checkerboard and not checkerboard_ok(item, tx, tz):
                    continue
                top = item.base + cells[i][1]
                found = world.bottom(tx, tz, (1, 1), item, style, connection=False, top=top)
                if not found:
                    continue
                bottom, ground = found
                tmin, _ = world.corners(tx, tz)
                if bottom < top and (not ground or top - tmin > 0.5):
                    records.append({"tile": (tx, tz), "sub": (1, 1), "bottom": bottom, "top": top,
                                    "top_f": float(top), "on_ground": ground, "stretch": False,
                                    "dir": square_dir(lx, lz), "start": (tx, tz),
                                    "checker": bool(item.flags & 0x200)})
    else:
        supported_groups = set()
        cap = style.pieces[CAP][0] if style.pieces[CAP] and item.flags & 0x800 else None
        cap_h = style.cap_height if cap else 0.0
        for c in sid.get("connections") or []:
            group = c.get("group", c.get("direction_type", 0))
            if group >= 0x20 or group in supported_groups:
                continue
            stx, stz = grid_transform(*c["start"], p.x, p.z, item.w, item.h, p.direction)
            etx, etz = grid_transform(*c["end"], p.x, p.z, item.w, item.h, p.direction)
            if checkerboard and not checkerboard_ok(item, stx, stz):
                continue
            sx, sz = rotate_sub(*c["start_sub"][:2], p.direction)
            fx, fz = rotate_frac(*c.get("start_frac", [0.5, 0.5]), p.direction)
            ex, ez = rotate_sub(*c["end_sub"][:2], p.direction)
            S = ((stx + (sx + fx) / 3) * TILE, (stz + (sz + fz) / 3) * TILE)
            E = ((etx + (2 * ex + 1) / 6) * TILE, (etz + (2 * ez + 1) / 6) * TILE)
            ls, lz_ = c["start"]
            dirc = square_dir(ls, lz_) if 0 <= ls < item.w and 0 <= lz_ < item.h else 1
            flags = c.get("flags", 0)
            inverted = bool(flags & 1)
            h0 = c["height"]
            h = item.base + h0
            # 1) a column straight under the attach point
            if (not inverted and not c.get("angled") and (stx, stz) == (etx, etz)
                    and (fx - 0.5) ** 2 + (fz - 0.5) ** 2 < NEAR_CENTRE_SQ):
                found = world.bottom(stx, stz, (sx, sz), item, style, connection=True,
                                     top=math.floor(h - cap_h + 0.5))
                if found:
                    bottom, ground = found
                    supported_groups.add(group)
                    hv = h - cap_h
                    top = math.floor(hv + 0.5)
                    if cap and (ground or bottom < top - 1):
                        out.append({"asset": cap["asset"], "kind": "cap", "matrix": turned_matrix(S[0], hv, S[1])})
                    world.sub_used.setdefault((stx, stz), set()).add((sx, sz))
                    if bottom < top - 1 and top - bottom <= MAX_COLUMN:
                        records.append({"tile": (stx, stz), "sub": (sx, sz), "bottom": bottom, "top": top,
                                        "top_f": hv, "on_ground": ground, "stretch": True, "dir": dirc,
                                        "start": (stx, stz), "checker": bool(item.flags & 0x200)})
                    continue
            # 2) a column on the end sub-cell and a girder to the attach point
            if flags & 2 or c.get("angled"):
                continue
            y_s = y_e = h0
            if cap:
                if inverted:
                    y_e = h0 + cap_h
                    h += cap_h
                else:
                    y_s = y_e = h0 - cap_h
                    h -= cap_h
            if not inverted and style.pieces[GIRDER]:
                h -= style.girder_depth
            top = math.floor(h + 0.5)
            used = world.sub_used.setdefault((etx, etz), set())
            if (ex, ez) in used:
                have = True
                supported_groups.add(group)
            else:
                found = world.bottom(etx, etz, (ex, ez), item, style, connection=True, top=top)
                if not found:
                    continue
                bottom, ground = found
                if top - bottom < MAX_COLUMN:
                    supported_groups.add(group)
                    used.add((ex, ez))
                if bottom < top - 1:
                    records.append({"tile": (etx, etz), "sub": (ex, ez), "bottom": bottom, "top": top,
                                    "top_f": h, "on_ground": ground, "stretch": True, "dir": dirc,
                                    "start": (stx, stz), "checker": bool(item.flags & 0x200)})
                    have = True
                elif not ground:
                    continue
                else:
                    have = False
            if cap:
                out.append({"asset": cap["asset"], "kind": "cap", "matrix": turned_matrix(S[0], item.base + y_s, S[1])})
            g = style.girder
            if have and g:
                m = girder_matrix(style, g, (S[0], item.base + y_s, S[1]), (E[0], item.base + y_e, E[1]), inverted)
                if m:
                    out.append({"asset": g["asset"], "kind": "girder", "matrix": m, "column": records[-1] if records and records[-1]["tile"] == (etx, etz) and records[-1]["sub"] == (ex, ez) else None})
    if records_out is not None:
        records_out += records
    for r in records:
        out += stack(style, r)
    return out


def ride_supports(cat, ride, pieces, terrain=None, scenery=None, glb_dir=None,
                  style_of=None) -> list[list[dict]]:
    """Support items per piece of a placed ride (same order as `pieces`)."""
    items = [Item(i, ride.section(p.name), p) if ride.section(p.name) else None for i, p in enumerate(pieces)]
    world = World([it for it in items if it], terrain, scenery)
    styles: dict = {}
    out = []
    # Higher items first: a cell's list runs from the top down, and sub-cells a
    # column already uses are shared with the entries processed after it.
    order = sorted((it for it in items if it), key=lambda it: -it.base)
    per_item = {}
    for it in order:
        name = (it.sid.get("supports") or (style_of(it.sec) if style_of else "")) or ""
        if not name:
            per_item[it.index] = []
            continue
        if name not in styles:
            styles[name] = Style(cat, name, glb_dir)
        per_item[it.index] = item_supports(cat, it, world, styles[name])
    for i in range(len(pieces)):
        out.append(per_item.get(i, []))
    return out
