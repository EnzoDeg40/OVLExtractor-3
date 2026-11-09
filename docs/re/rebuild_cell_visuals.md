# RebuildCellVisuals (Mac x86_64): support columns + girders

Source: Ghidra decompile `decompiled.c`, cross-checked with `otool -tV` disassembly where the decompiler
dropped implicit XMM0 arguments. Constants read from `RCT3_Exe` (Mach-O, VM->file via LC_SEGMENT_64).
Tags: **[D]** = read straight off the decompile or disassembly. **[I]** = inferred, meaning a guessed name or purpose.
Windows equivalents are from `rerct3/research/re/supports.md`. Struct offsets differ there because Mac
pointers are 8 bytes. Many Mac "pointers" are 32-bit handles resolved through `FUN_1001f2550` / `FUN_1001f25e0`.

## Function map (Mac)

| Mac | Role | Windows |
|---|---|---|
| `FUN_1009a15c8` | dirty-cell sweep, calls RCV in 3 passes: `(cell,1,1)`, `(cell,0,1)`, `(cell,0,0)` [D] | — |
| `FUN_1009a17ee` | **RebuildCellVisuals(mgr, cellX, cellZ, dirtyNeighbours, groupRescan)** | `0x009b2c00` |
| `FUN_1009a4b02` | **CreateColumnRecord** (0x40-byte record at mgr+0x128, count +0x130, cap +0x134) | `0x009b0fa0` |
| `FUN_1009a3f78` | resolve support style of an item square + column direction type | — |
| `FUN_1009a469c` | find column bottom under a sub-cell (lower item or terrain) | — |
| `FUN_1009a5460` | place one column piece (unscaled) | — |
| `FUN_1009a565c` | place one column piece, Y-stretched (top piece) | — |
| `FUN_1009a49fa` | place cap piece (type 17) | — |
| `FUN_1009a4e34` | **PlaceAngledGirder** (used for every girder, flat or not) | `0x009b0390` |
| `FUN_1009a5f8a` | SupportEntry add-piece: route SID type 12..17 into arrays, sorted insert | AddVisualNode `0x009b00b0` |
| `FUN_1009a410e` | SupportEntry first-use load: girder heights + bubble sort, cap height, top half-width | LoadAndSortPieces `0x009b09b0` |
| `FUN_1009a63f4` | SupportEntry ctor (0x80 bytes) | `0x009af3a0` |
| `FUN_1007d2fe6` / `FUN_1007d2f6a` / `FUN_1007d304c` | connection i -> world cell of (+0x00,+0x04) / (+0x1C,+0x20) / (+0x34,+0x38) | — |
| `FUN_1007ca856` | world cell -> item-local cell (inverse rotation), bounds check | — |
| `FUN_1007d3394` / `FUN_1007d341c` | "group already supported" tests on item+0x1a4 | — |
| `FUN_1004cd588` | terrain tile min / max of the 4 corner heights | — |
| `FUN_10043e4a4` = `(int)ceilf(x)`, `FUN_10043e4b4` = `(float)i`, `FUN_10043e4be(m, r)` = rotY((r+2)·π·−0.5) | helpers [D] | — |

Constants [D]: `DAT_100bf4964`=3.0 (sub-cells per tile), `DAT_100bf98e0`=6.0, `DAT_100bf2da4`=0.5,
`DAT_100bf200c`=−1.0, `DAT_100bf2010`=1.0, `DAT_100c66980`=0.04, `_DAT_100c57cd0`={−0.5,−0.5},
`DAT_100c2cfe4`=1e−20 (pitch epsilon), `DAT_100bf668c`=π, `DAT_100bf4970`=−0.5.

## Structures touched

### Item (placed scenery / track piece) [D offsets, I names]
`+0xf8` SID, `+0x100/+0x104` grid origin, `+0x108` base height (int), `+0x10c` rotation 0..3,
`+0x140` 12 bytes copied into column records (I: recolour triple), `+0xc8→+0x5c` value passed as
"colour" to every piece, `+0x1a4` support bitmask (2 bits per connection, see §4), `+0xbc & 8` = skip item,
`+0x15c` (< −1 ⇒ treat the item below as terrain, purpose unknown).

### SID fields used
- `sid+0x04` flags [D]: `0x80` supports enabled (`(char)flags < 0`). `0x100` other columns may stand on
  this item. `0x200` checkerboard rule (§1). `0x400` connection mode: use the 88-byte entries. `0x800` place cap (type 17) piece.
  On support *pieces*: `0x4000` → entry+0x74=1, `0x8000` → entry+0x74=entry+0x75=1 (`FUN_1009a5f8a`).
- `sid+0x10` squaresX, `sid+0x18` square array (stride 0x14), `sid+0x34` connection count, `sid+0x38` connections,
  `sid+0x48` type, `sid+0x4c` support style name.
- Square (0x14) [D]: `+0x00` flags (bit1 `&2` = this square gets supports, bits 3..11 = 3×3 sub-cell
  "solid" mask, bit `3+sx+3·sz`). `+0x04` low height. `+0x08` high height. `+0x10` support direction type.
  A support piece's own height = `square[0].+8 − square[0].+4` (int). An item's top over a cell is
  `square.+8 + base`, and its underside is `square.+4 + base`. (The tool calls `+8` `height_span`. The game uses it as an absolute top.)

### Connection entry, 88 bytes (`sid+0x38`, stride 0x58)

| Off | Type | Use in RCV (all [D] unless noted) |
|---|---|---|
| +0x00/+0x04 | i32 | start cell (item-local). `FUN_1007d2fe6`, also passed to `FUN_1009a3f78` for style + column dir |
| +0x08/+0x0C | **i32** | start **sub-cell index** 0..2 (X,Z). Integer `2 − v` under rotation, `cvtsi2ss` later |
| +0x10/+0x14 | f32 | start **fraction inside the sub-cell** 0..1 (rotated as `1 − f`) |
| +0x18 | f32 | attach height above item base |
| +0x1C/+0x20 | i32 | end cell (where the column stands). `FUN_1007d2f6a` |
| +0x24/+0x28 | **i32** | end **sub-cell index** 0..2 (column sits at its centre) |
| +0x2C | f32 | read for angled entries, but only multiplied by 0.0 or overwritten before use: **dead** |
| +0x30 | i32 | angled flag (≠0) |
| +0x34/+0x38 | i32 | **angled far point cell** (item-local). `FUN_1007d304c`. The tool calls these `raw_34/raw_38` |
| +0x3C/+0x40 | i32 | angled far point sub-cell index |
| +0x44/+0x48 | f32 | angled far point fraction inside sub-cell |
| +0x4C | f32 | angled far point height above base |
| +0x50 | u32 | bit0 "inverted" (girder over the attach point). bit1 "no girder fallback" |
| +0x54 | i32 | **alternative-group id**, must be < 0x20. It is *not* a direction (see §4) |

The tool's `TrackReader.cpp` reads `+0x08..+0x14` as 4 floats and `+0x24/+0x28` as floats. Both pairs are ints.

World position of any connection point [D, lines 599-610 / 682-687]:
```
x = (cellX + (sub + frac) / 3) * world.tileX + world.originX     # world = DAT_101117128->+0x90: +0x10/+0x14 origin, +0x18/+0x1c tile size
start point : sub = +0x08/+0x0C, frac = +0x10/+0x14
end point   : sub = +0x24/+0x28, frac = 0.5            (sub-cell centre)
far point   : sub = +0x3C/+0x40, frac = +0x44/+0x48, y = +0x4C
pieces      : (cell + (2*sub + 1)/6) * tile + origin      (FUN_1009a5460, same as frac 0.5)
```
Rotation of sub/frac by item rotation r (`item+0x10c`) [D, lines 474-509]:
`r=1: (sx,sz)->(sz, 2−sx), (fx,fz)->(fz, 1−fx)`. `r=2: (2−sx, 2−sz), (1−fx, 1−fz)`. `r=3: (2−sz, sx), (1−fz, fx)`.
The end and far sub-cells rotate the same way. The tool's `_sub_transform` uses `start_sub[2..3]` alone as a tile
fraction. The game's tile fraction is `(start_sub[0] + start_sub[2]) / 3`, with `start_sub[0]` read as an int.

### SupportEntry (Mac, 0x80) [D: ctor `FUN_1009a63f4`, add-piece `FUN_1009a5f8a`, load `FUN_1009a410e`]
`+0x08 + k*0xC` = {handle, cap, count} for k = 0 middle (SID type 12), 1 top (13), 2 bottom (14), 3 bottom-extra (15).
Array elements are 0x30 bytes: `+0x08` piece ref, `+0x28` int height. Pieces are inserted in **ascending** height order
(insert before the first strictly taller one). `+0x38` f32 = max(maxX, maxZ) of the first top piece's
shape bbox, i.e. the top half-width (I). `+0x3C` girder array (handle, cap +0x40, count +0x44). Each element's `+0x28` is a float equal to
shape(+0x10) − shape(+0x04) = **maxY − minY (girder thickness)**. The girders are bubble-sorted ascending on it.
`+0x50` cap-piece ref (type 17), `+0x70` cap height (maxY − minY), `+0x74` "stand on any item", `+0x75`
"merge / on-track bonus", `+0x78` load refcount. Shape bbox = StaticShape {min xyz @0, max xyz @0xC} (I, layout).

### Column record (0x40) written by CreateColumnRecord [D]
Mac call: `CreateColumnRecord(float topF /*XMM0*/, mgr, bottom, top, entry, dir, onGround, colours*, colour, stretchTop, subX, subZ, caps)`.
Fields: `+0x00` bottom i32, `+0x04` top i32, `+0x08` topF f32 (exact attach), `+0x0C/+0x10` subX/subZ,
`+0x18` SupportEntry*, `+0x20` column dir type, `+0x24` caps (place bottom/top pieces), `+0x25` onGround,
`+0x28..+0x33` copy of item+0x140, `+0x34` colour, `+0x38` stretchTop. Rejected if `top − bottom > 0xE4` (228).

## 1. Producing column records (first half of RCV)

The cell has a list at `mgr+0x98` with 16-byte entries `{item*, ?, connIdx (+0xC)}` and count `mgr+0xa0`. A connection that spans two cells is registered in its end cell with connIdx = i. Plain registrations use −1 (I: matches Windows InsertSorted).
Entries are ordered so that later entries are lower (I). `FUN_1009a469c` and the case-A lower-item search both scan forward to find what is underneath.
For each entry whose `sid.flags & 0x80` is set and whose `item+0xbc & 8` is clear:

**Checkerboard rule [D, asm 0x1009a1fbd-0x1009a1fd6, decompile l.224 / l.393-400]:** a column is allowed when
`(x ^ z) & 1` (odd cell), or when `!(sid.flags & 0x200) && !cellOccupied`. Here `cellOccupied` = grid of singleton `DAT_1010893a0`
(+0xb0/+0xb4 dims, +0xb8 handle, stride 0x18, first int ≥ 0). Its meaning is unknown (I: path grid). For
connection items the parity is taken from the connection's **start** cell.

### Case A — plain scenery (`flags & 0x400 == 0`), connIdx must be < 0 [D l.222-360]
```
style, dir = FUN_1009a3f78(item, local cell)          # null if square.flags & 2 == 0
top = square.+4 + item.base                            # column ends at the item's underside (int, no stretch)
lower = next entry in cell list with connIdx<0 and FUN_1009a53d8()   # I: skips ghost/moving items
if lower:
    if style.+0x74 or lower.+0x15c < -1 or lower.sid.flags & 0x100:
        bottom = lower.square.+8 + lower.base
        if lower.sid.type == 0x12: bottom += (style.+0x75 ^ 1)      # 1 unit gap above track pieces
        if lower.+0x15c < -1: bottom = ceil(terrainMin)
        if bottom < top: CreateColumnRecord(float(top), bottom, top, dir, onGround=0, stretch=0, sub=(1,1), caps=1)
        if style.+0x75 and style(lower.sid.+0x4c) == style: lower.flags |= 0x10     # merge marker
    # else: no column
else:                                                   # terrain
    tmin, tmax = FUN_1004cd588(cell)                    # min/max of 4 corners
    if !blocked(tmin,tmax) and top - tmin > 0.5 and ceil(tmin) < top:   # asm 0x1009a1dc8-0x1009a1dec: ceil(tmin)
        CreateColumnRecord(float(top), ceil(tmin), top, dir, onGround=1, stretch=0, sub=(1,1), caps=1)
```
`blocked` = `FUN_10055b9f0`, a per-tile list of volumes in singleton `DAT_100e956d8`. A volume blocks when it overlaps
[tmin, tmax] (I: water / tunnels).

### Case B — connection items (`flags & 0x400`) [D l.362-835]
The loop runs over connections i ∈ [connIdx, connIdx] if connIdx ≥ 0, else over all of them.
```
style, dir = FUN_1009a3f78(item, c.start cell)          # null -> skip
startCell = FUN_1007d2fe6(i); endCell = FUN_1007d2f6a(i)
ok = endCell == thisCell and c.+0x54 < 0x20 and checkerboard(startCell)
if groupRescan: ok = ok and not FUN_1007d341c(item,i)   # an earlier same-group entry already supported
                (also re-dirties end cells of later same-group entries holding a girder bit)
else:           ok = ok and not FUN_1007d3394(item, c.+0x54)   # any same-group entry supported
if !ok: continue
rotate sub/frac by item rotation; S = start point, E = end point (see above)
h = float(item.base) + c.+0x18
flags = c.+0x50

# 1) vertical column right under the attach point
if !(flags&1) and c.+0x30 == 0 and connIdx < 0 and startCell == thisCell
   and (fx-0.5)^2 + (fz-0.5)^2 < 0.04                     # 0.04 = DAT_100c66980, i.e. within 0.2 of sub-cell centre
   and FUN_1009a469c(sx, sz) -> bottom, onGround:          # failure falls through to 2)
    item.+0x1a4 |= 1 << 2i
    if cap && sid.flags&0x800: h -= capH
    top = ceil(h) - (h - (ceil(h)-1) < 0.5)              # = floor(h + 0.5)
    if (onGround or bottom < top-1) and cap and 0x800: place cap at (S.x, h, S.z)
    subMask |= 1 << (sz*3 + sx)
    if bottom < top-1: CreateColumnRecord(h, bottom, top, dir, onGround, stretch=1, sub=(sx,sz), caps=1)
    continue

# 2) girder path
if flags & 2: continue                                   # bit1: no girder/offset column
yS = c.+0x18; yE = yS                                    # girder end heights (relative)
if not angled:
    if cap and 0x800: (flags&1) ? (yE = h0 + capH; h += capH) : (yS = yE = h0 - capH; h -= capH)
    if !(flags&1) and girders: h -= girder[0].thickness  # girder hangs below the attach line
else (angled, needs girders):
    F = far point (cell +0x34/+0x38, sub +0x3C/+0x40, frac +0x44/+0x48), hF = c.+0x4C
    L = |F-S|xz, u = (F-S)xz/L, cosP = L/len, sinP = (hF-h0)/len, g = girder[0].thickness
    t = dot(E-S, u) + style.+0x38                         # far edge of the column top
    h = ((t - g*sinP) * sinP)/cosP + (h0 - cosP*g) + base # underside of inclined girder above E
top = floor(h + 0.5) (same formula)
bit = ez*3 + ex
if subMask has bit: item.+0x1a4 |= 2<<2i; haveColumn = true      # column already built by a sibling
elif FUN_1009a469c(ex, ez) -> bottom, onGround:
    if top - bottom < 0xE4: item.+0x1a4 |= 2<<2i; subMask |= bit
    if bottom < top-1: haveColumn = CreateColumnRecord(h, bottom, top, dir, onGround, stretch=1, sub=(ex,ez), caps=1)
    elif !onGround: continue
    else haveColumn = false
else: continue
if cap and 0x800: place cap at (S.x, base + yS', S.z)     # yS' = h0 - capH (normal) or h0 (inverted)
if haveColumn and top-bottom < 0xE4 and girders:
    P0 = (S.x, base + yS', S.z)
    P1 = angled ? (F.x, base + hF, F.z) : (E.x, base + yE, E.z)
    L  = |P1-P0|xz
    k  = first girder (ascending thickness) with thickness >= L, else the last one
    PlaceAngledGirder(girder[k], P0, P1, inverted = flags&1)
```
`subMask` (`local_58`) is reset to 0 for each case-A item and otherwise carries over between case-B items of the same cell [D l.198, l.360].

**Girder selection [D, asm 0x1009a30e4-0x1009a3116]:** `ucomiss` compares the girder element's `+0x28` with L.
`+0x28` is maxY − minY of the girder shape (asm 0x1009a42d4-0x1009a4323). Real spans (≥ 1.33) are always larger than
girder thicknesses. In practice the game therefore always takes the **last girder in sorted order**. Equal thicknesses keep their load order,
because the bubble sort only swaps on `<`. This comparison looks like a game bug (I), but it is what runs. It matches the live
observation that the 3L girder is always used.

## 2. Stacking a column record (second half of RCV, l.878-1260) [D]
```
for each record R (count grows while looping, so new split records are processed too):
    cur = R.bottom; rem = R.top - R.bottom; topPlaced = false
    if R.caps:
        A = R.onGround ? 3 : 2            # 3 = bottom-extra (type 15), 2 = bottom (type 14)
        placed = greedy(A)                # tallest -> shortest; each piece at most once
        if !placed: greedy(the other of 2/3)
        # greedy for bottoms: if dirMatch and h <= rem: place at y=cur; cur += h; rem -= h
        greedy over array 1 (top, type 13):
            if dirMatch and h <= rem: rem -= h; y = cur + rem
                 R.stretchTop ? placeStretched(y, scaleY = (R.topF - y) / shape.maxY) : place(y)
                 topPlaced = true
    for middle pieces (array 0, type 12), tallest first, dirMatch only:
        a = cur, b = cur + rem
        i0 = ceil(a/h), i1 = floor(b/h)            # integer division with sign handling
        if i1 > i0:
            place pieces at y = i0*h, (i0+1)*h, ... (i1-1)*h  # grid-aligned to multiples of h
            lowGap = i0*h - a; highEnd = i1*h
            if lowGap == 0 and b == highEnd: break
            elif lowGap == 0: cur = highEnd; rem = b - highEnd
            elif b == highEnd: rem = lowGap
            else:  # both gaps: lower one continues here, upper one becomes a new record
                CreateColumnRecord(topPlaced ? float(b) : R.topF, highEnd, b, R.entry, R.dir,
                                   R.onGround, R.colours, R.colour, R.stretchTop, R.subX, R.subZ, caps=0)
                rem = lowGap
```
The `float(b)` / `R.topF` choice is checked in asm 0x1009a3afe-0x1009a3b96. Placement positions use `(cell + (2*sub+1)/6) * tile + origin`, and Y is the integer.
The split record holds only middles, and its gap is < h, so taller pieces cannot fit there anyway. The result is the same as continuing both gaps with the
remaining smaller sizes, which is what the tool's gap list does.

- Rounding [D]: bottom = `ceil(terrain min corner)` or the lower item's int top. Connection top = `floor(h+0.5)`
  (the asm computes it as ceil − (frac < 0.5)). Scenery top = item underside (int). The stretched top piece reaches the exact float `h`.
  A connection column needs `bottom < top − 1`, i.e. at least 2 units. A scenery column needs `bottom < top` and `top − tmin > 0.5`.
- Bottom pieces are only used when the record has `caps`. All primary records do, split records don't.

## 3. Girders: PlaceAngledGirder `FUN_1009a4e34` [D]
```
d = P1 - P0
pitch = atan2(d.y, |d.xz|);  yaw = atan2(d.z, d.x)
off = (0, inverted ? -shape.minY : -shape.maxY, 0)
if |pitch| >= 1e-20: off = rotate(off, axis = normalize(d.z, 0, -d.x), angle = -pitch)
pos = (P0 + P1) * 0.5 + off
s = (|d| + 2*style.+0x38) / (shape.maxX - shape.minX)    # overhangs each end by the top half-width
M = I; RotZ(-pitch); RotY(yaw); row X *= s                 # row-vector matrices: local X -> d/|d|
node.+0xb4 = (|d.x| < |d.z|)                                # FUN_100323a9e; non-geometric (I)
```
- Normal entries: the girder top lies on the P0–P1 line. Inverted: the girder bottom lies on it.
- Flat girders are the same function with pitch = 0. There is **no horizontal tiling**: one stretched mesh per connection.
- The 0.04 check (`DAT_100c66980`) does **not** test girder length. It decides "vertical column vs girder":
  squared distance of the start fraction from the sub-cell centre (0.5, 0.5) < 0.04.
- Live check: `(gap + 0.73)/3.66` (tool) = `(|d| + 2·0.365)/(maxX−minX)` → the top half-width is 0.365. The tool's
  `GIRDER_DEPTH` 0.443 = `girder[0].thickness`, by which the column top is lowered.

## 4. Direction types and piece rotation [D]
Column direction (`FUN_1009a3f78`): `d = square.+0x10` of the item cell that carries the style, then with item rotation r:
```
if 2 <= d <= 5: d = d + r; if d >= 6: d -= 4
elif d in (6,7): if r odd: d = 13 - d          # toggles 6 <-> 7
(1 and anything else: unchanged)
```
Piece match and rotation (`l.1048-1063`, repeated for every array). `p = piece SID square[0].+0x10`:
```
p in 2..5 : matches d in 2..5, rot = (d - p) mod 4
p in 6,7  : matches d in 6,7,  rot = (d != p)     # 0 or 1 quarter turn
p == 1    : matches d == 1,    rot = 0
otherwise : never placed
```
The piece matrix is `rotY((rot + 2) · π · −0.5)` (`FUN_10043e4be`), with the same half-turn convention as scenery.
Caps use rot 0. `c.+0x54` is not a direction: it is a group id (< 32) used only for "one support per group".
`item+0x1a4` holds bit `2i` (vertical column built for connection i) and bit `2i+1` (girder column).
Within a group, the first connection in array order that succeeds wins. The tool already groups by `+0x54` ("direction_type").

## 5. Hanging / inverted
No code path builds a column downward from a structure. Every record runs bottom → top, with the bottom found by
`FUN_1009a469c` (lower item or terrain). "Inverted" (`+0x50 bit0`) only changes the following [D]:
- the vertical-under-the-point path is disabled, so a girder is always used and the column stands at the end sub-cell beside the track;
- the cap is added *above* the attach height (`h += capH`, girder P1 raised by capH);
- the column top is not lowered by girder thickness;
- the girder is offset by −minY, so it rests on top of the line instead of hanging under it.

The result is a column beside the track whose girder passes over the track. With `+0x50 bit1` set, no
offset column or girder is built: only the vertical path can support that entry.

## Differences from the tool (rct3_track.py / TrackReader.hpp)
1. `start_sub[0..1]` and `end_sub_x/z` are int sub-cell indices. Positions are `(sub + frac)/3`, and the end point is `(sub+0.5)/3`.
   `_support_point`'s underside search can be replaced by this exact formula.
2. `raw_34/raw_38` = angled far-point cell. `+0x2C` is effectively unused.
3. The vertical column is used only when the start fraction is within 0.2 of the sub-cell centre, the entry is not angled, not inverted, and the start cell is the end cell.
   Otherwise a girder is used, even inside the same tile.
4. Thresholds: a connection column needs `bottom < round(h) − 1` with `bottom = ceil(min corner)`.
   The tool's `top − ground_max < 1.1` is not what the game does. (The 1.1 is the placement layer's top extension on Windows.)
5. Bottom piece: type 15 is preferred on terrain and type 14 on another structure, each falling back to the other. Several bottom pieces can stack greedily.
6. Columns respect the square direction type (1 / 2–5 / 6–7): a piece with a non-matching type is skipped.
7. Girder choice: always the last after the sort by thickness (§1). The scale overhang = 2 × top-piece max(maxX, maxZ).
8. The checkerboard rule (`sid.flags & 0x200`) and the lower-item rules (`0x100`, entry+0x74/+0x75, +1 on type-18 track) are absent from the tool.
