# Track piece supports (Mac x86_64): where a placed piece gets columns

Sources: `decompiled.c` (line numbers below are lines in that file), `otool -tv` disassembly where the decompiler
dropped register/XMM arguments, constants read from `RCT3_Exe` (file offset = VM − 0x100000000), sid data from
the shipped OVLs and `OVLExtractor-3/out/*/set/*.tracks.json`.
Tags: **[D]** read off decompile/disasm/data. **[I]** inferred (name or purpose guessed).
Companion: `rebuild_cell_visuals.md` (other agent) decodes RebuildCellVisuals / CreateColumnRecord / stacking /
girders in full; this file covers how a *track piece* gets there and which rules decide its columns.
Both files agree on the RCV mechanics; corrections to that file are listed in §7.

## 0. Summary (the important corrections)

1. **The Windows "TrackPiece_BuildSupports → SupportGrid_PlaceColumns/CreateColumn" chain does not build
   support columns.** On Mac it is `FUN_1003af7c4` → `FUN_1009ffe10` → `FUN_100a00082`, and it registers
   *terrain-hollow (tunnel) intervals* per tile, after auto-cutting the terrain under the track. The 1.1, the
   0.2/−0.5 z-fight and the ≤20-pass chain walk all belong to that tunnel/terrain-cut logic. [D + I, §1]
2. **Track supports are built by the scenery support system** (`ScenerySupportManager`, RCV `FUN_1009a17ee`).
   It sees each track piece through the `SceneryItem` that the piece owns. The piece's own `sid` drives it:
   `sid.flags`, the footprint squares and the 88-byte connection entries. [D, §2]
3. Two modes chosen by `sid.flags & 0x400`. Connection mode uses the 88-byte entries (steel sets). Per-cell mode uses the
   squares (wooden ts2, ts3, ts5 Heartline, ts7 giant flume, Air Powered/Flyturn/Reverser/Side-friction/...).
   Per-cell columns stand at the **tile centre**, from the ground (or the item below) up to `base + square.+4`. [D, §3]
4. **"One column out of two"** = checkerboard rule: when `sid.flags & 0x200` is set (or the tile is inside an
   animal-enclosure region) a column is only allowed on cells with `(x ^ z) & 1 == 1`. [D, §4]
5. **Double-support avoidance** = "first entry of a `+0x54` group that succeeds wins" (bits in `item+0x1a4`),
   plus a per-cell 3×3 sub-cell occupancy mask. There is **no** chain walk for supports. [D, §4]
6. **Sub-cell position comes from the sid**, not from the track geometry. Entry `+0x08/+0x0C` and `+0x24/+0x28` are
   **int32 sub-cell indices 0..2**. Our extractor reads them as floats, and `jnum()` then writes those denormals out as 0. [D, §3.3]
7. Styles ts1a / ts6 (Sky Swinger, Flyer, Frequent Faller): every or most entries are cross-cell with
   `flags & 1` ("inverted"). The tool needs a `start == end` main entry and so drops them. The game uses the girder path:
   a column in the end sub-cell beside the track, with a girder passing over it. [D, §5]

---

## 1. Mac "TrackPiece_BuildSupports" = terrain cut + tunnel/hollow registration

### Function map

| Mac | Windows name (rerct3) | Real role |
|---|---|---|
| `FUN_1003af7c4` (l.682427) | TrackPiece_BuildSupports `0x00aa1cd0` | TrackPiece: cut terrain, propagate along chain, register terrain-hollow intervals [I name] |
| `FUN_1003b1306` (l.683674) | — | TrackPiece_CutTerrain(piece, mode, apply): lowers terrain corners under the piece [I] |
| `FUN_1003b0d58` (l.683392) | — | world AABB of a per-ride mesh (ride+0x948) through the piece's SceneryItem matrix (+0x160..) [D], mesh identity [I] |
| `FUN_1009ffe10` (l.1698076) | SupportGrid_PlaceColumns `0x00acb0e0` | HollowGrid_AddRect [I name] |
| `FUN_100a00082` (l.1698180) | SupportGrid_CreateColumn `0x00ac8d30` | HollowGrid_AddInterval (0x2c record + 4 corner vertex intervals) |
| `FUN_1009ffc90` (l.1697992) | SupportGrid_SetColumnFlags | sets record +0x24 = colour, bit1 of +0x10 |
| `FUN_1003b0c80` | (remove) | RemoveColumns(grid DAT_101116bc8) + unregister from list DAT_100e95130 |
| `FUN_100a01786` (l.1699207) | — | point query "is (x,y,z) under terrain AND inside a recorded interval" |
| `FUN_1004cad50` | TerrainTileList_AddSupportVertex | per terrain vertex list of {col,row,corner,next,bottom,top} |
| `FUN_1004caf22` / `FUN_1004caf88` | — | read those vertex intervals: terrain ray-cast (`FUN_1004cb490`) and **terrain mesh build** (`FUN_1004ce4f2`, emits +4 verts / +6 indices per quad) |

Evidence that this is not supports [D]:
- No support mesh ever reads grid `DAT_101116bc8`. RCV (`FUN_1009a17ee`) never references it. Its readers are
  the terrain mesh generator, the terrain ray-cast and `FUN_100a01786` (used by camera/peep/placement code).
- `FUN_100a01786` returns true only when `y <= tile min corner` (underground) and `bottom < y < top` of a record:
  that is a "hollow inside the terrain" test.
- The colour put in the record (`FUN_1009ffc90(grid, group, 0, &piece+0x534)`) is the TrackPiece field registered as
  **`"TunnelLightColours"`** at 0x534 (TrackPiece ctor `FUN_1003ad4d4`, l.~681740). The path builder's
  fixed `0xFFFFF299` is the path tunnel light tint, not a support colour (I).
- `FUN_1003b6a88` (setter of +0x534) re-applies it to the hollow records.

### Algorithm [D]
```
FUN_1003af7c4(piece):                                   # piece = TrackPiece
  RemoveHollow(piece)                                   # FUN_1003b0c80
  if piece.iface(+0x70).vfn[0x58]():  return            # FUN_1007d89bc: skip if iface+0x10c, or ride+0x948 mesh present
                                                        #   with +0x40 != 0, else sid.flags bit16 ((sid+6)&1)
  piece+0x530 = registerInList(DAT_100e95130, piece)    # I: tunnel manager entry
  rect = iface.vfn[0x40]()                              # FUN_1007db680: footprint grid rect (+ y range base..base+sid+0xc)
  lo, hi = +1e8, -1e8
  for each tile (x,z) in rect:
      if iface.vfn[0x48](x,z,&b,&t):                    # FUN_1007db900: needs square.flags bit0 (collision)
                                                        #   b = base + square.+4, t = base + square.+8
          tmin = min(4 corners of terrain tile)         # terrain tile stride 0x20 on Mac, corners at +0..+0xc
          if b + 0.2 < tmin: b += -0.5                  # DAT_100bf59e8=0.2, DAT_100bf4970=-0.5  (l.682525)
          lo = min(lo,b); hi = max(hi,t)
  if lo <= hi:
      bbox = FUN_1003b0d58(piece, all sub-meshes, recurse=1)   # extends rect and lo/hi by the ride mesh AABB
      if FUN_1003b1306(piece,0,0) and !piece+0x160:            # cut terrain under this piece
          repeat up to 20 times (iVar16 = 0x14, l.682606):     # walk Prev links (+0x270 ref, cache +0x298)
              changed = any FUN_1003b1306(p,0,1) over the chain   # and Next links (+0x238 ref, cache +0x260)
          while changed
      group = piece+0x528 ({-1,0,0})
      HollowGrid_AddRect(lo, hi, grid, rect, group)     # FUN_1009ffe10
      SetColour(grid, group, 0, &piece.TunnelLightColours)
```
`FUN_1009ffe10` (l.1698123 / l.1698152) [D]:
```
top += 1.1                                               # DAT_100c48e94
for tile in rect:
    tmin,tmax = min/max of the 4 corners
    if top <= tmin or tmax <= bottom:                     # interval does not cross the current terrain surface
        AddInterval(tile, bottom, top)                    # record {bottom, top+1.1}, linked per tile
```
So the interval is the volume the track (plus 1.1 head-room, plus 0.5 floor when buried) reserves inside the
terrain. Where the terrain covers it, the terrain mesh is cut (a tunnel). Where it crosses the surface, `FUN_1003b1306`
has already lowered the terrain corners to the track level (margins 2.0 above, 0.25 or 1.2 below, l.~683790).
The 20-pass loop re-runs that cut on every piece of the chain until no corner changes, because cutting
for one piece can change the terrain under its neighbours.
**This is not "double support avoidance".** The Windows note's reading was a guess.

TrackPiece fields used (ctor `FUN_1003ad4d4`) [D]: `Next` managedobjectptr @0x238, `Prev` @0x270,
`PlatformPiece` @0x2b8, `TunnelLightColours` int32 @0x534. On Windows the +0x15C/+0x178 links are the same Next/Prev pair.

## 2. How a track piece reaches the support builder

- `TrackPiece` derives from **`SceneryItemPlacementSingle`** (ctor `FUN_1007dcaa8`, class name string set at l.1367554),
  with fields `SIDDatabaseEntry` @0x78, `SceneryItemDataField` @0xb0 {sid*, POSX, POSZ, HEIGHT, DIRECTION, CORNER},
  `FlexiColourField` @0xd0, **`SceneryItem` managedobjectptr @0xe8**, `Owner` @0x120. [D]
- `FUN_1007dd402` (l.1366837) resolves the owned SceneryItem (cache +0x110) and copies its
  x/z/height/rotation/corner (`+0x100..+0x110`) into the placement struct at +0xb8. So each track piece has a
  real `SceneryItem` in the world, using the piece's sid. [D]
- A SceneryItem's place/update `FUN_1007cf778` (l.1357098) [D]:
  ```
  if !(item+0xbc & 8):
      register footprint cells in world object grid (DAT_100e929a8)
      AddConnectionSupports(ScenerySupportManager DAT_1011141b0, item)   # FUN_1009a5b0c
  ...
  FUN_1007ccffa(item)        # scenery version of the hollow registration (NOT supports, see §1)
  ```
- `FUN_1009a5b0c` (= Windows AddConnectionSupports): for each entry with start cell ≠ end cell, it
  inserts `{item, ceil(base + entry.+0x18), i}` into the end-cell list `cell+0x38` (`FUN_1009a5c08`). [D]
- RebuildCellVisuals `FUN_1009a17ee(mgr, x, z, …)` builds the cell's item list (`FUN_1009a3ea4`). Items whose
  footprint covers the cell come from the world grid with connIdx −1. Cross-cell entries come from `cell+0x38` with connIdx i.
  The list is sorted by height, descending. [D build, I order]

So the support of a track piece is the scenery-support result for its SceneryItem. The style, the colours
(`item+0x140` FlexiColour COL0..2 and `item+0xc8→+0x5c`) and every rule below are per SceneryItem. [D]

## 3. Per-piece decision

Gate for any support [D, l.~1640140]: `sid.flags & 0x80` (byte `(char)flags < 0`) and `!(item+0xbc & 8)`.

### 3.1 sid flags of shipped track sids (from `out/*/set/*.tracks.json`) [D data]
| flags | meaning in RCV | where |
|---|---|---|
| 0x80 | supports enabled | every supported track sid |
| 0x400 | connection mode (88-byte entries) | all `ts1`, `ts1a`, `ts6`, `ts9` sets |
| 0x200 | checkerboard (§4) | one-tile orthogonal pieces (Straight, Stationmiddle, Invstraight, Medslope…); never on 45° pieces; per-cell sets: some giant flume/splash pieces |
| 0x800 | put the style's cap (type 17) at the attach point | Wild Mouse, Junior/Mini/Spiral, Mine Ride, Water coaster, some Giga |
| 0x100 | other columns may stand on this item | (not on track sids) |
| (no 0x400) | per-cell mode | Track20 wooden `ts2`, Air Powered/Flyturn/Reverser/Reverse/Side-friction/Splash `ts2`, `ts3` Minetrain/wooden wild mouse/Virginia reel, Heartline `ts5`, Giant flume `ts7`, `ts10` |

### 3.2 Per-cell mode (`flags & 0x400 == 0`) [D, l.1640152-1640290; call args checked in asm 0x1009a1cb5/0x1009a1e1c]
```
for every footprint square of the piece that covers this cell:
    skip if connIdx >= 0
    skip if checkerboard(x,z) fails                      # §4
    style, dir = FUN_1009a3f78(item, localCell)           # null if square.flags & 2 == 0  -> no column
    top = item.base + square.+4                           # integer, the piece's underside at that square
    lower = next item below in the list (skipping connIdx>=0 and TrackPlatformPiece-owned items, §6)
    if lower: stand on it only if style.+0x74 or lower.sid.flags & 0x100 or lower+0x15c < -1
              bottom = lower.base + lower.square.+8 (+1 if lower.sid.type == 0x12 and !style.+0x75)
              column if bottom < top
    else:     tmin = min corner of the tile; no column if water/volume (FUN_10055b9f0) on the tile,
              or top - tmin <= 0.5, or ceil(tmin) >= top
              bottom = ceil(tmin)
    CreateColumnRecord(bottom, top, dir, sub = (1,1), …)  # always the tile centre sub-cell
```
- Square fields [D]: `+0x00` flags (bit0 collision, bit1 supports, bits 3..11 = 3×3 sub-cell solid mask,
  bit `3 + sx + 3·sz`), `+0x04` low offset (underside), `+0x08` high offset (top, absolute from base, not a span),
  `+0x10` support direction type. The tool's `height_adjust` = `+0x04`, `height_span` = `+0x08`.
  Example wooden `straight`: `[0xFFB, 0, 4, 1]` → column top at base+0, all 9 sub-cells solid, dir 1.
- Direction type → column piece selection and rotation is in `rebuild_cell_visuals.md` §4. On wooden ts2, the
  `diag` pieces are the 2..5 types used by 45° squares (dir 2..5).
- Every square with bit1 gets a column (subject to §4/§6). A 2×2 diagonal piece can get up to 4.

### 3.3 Connection mode (`flags & 0x400`) [D, l.1640290-1640835]
An entry `i` is processed in the cell rebuild of its **end** cell (`+0x1C/+0x20`). For a main entry
(start == end), that is the item's own cell.
```
style, dir = FUN_1009a3f78(item, entry.start cell)      # start square needs bit1, gives dir
ok = endCell == thisCell and entry.+0x54 < 0x20 and checkerboard(startCell)   # asm 0x1009a2238-0x1009a22a9
ok &= not "earlier entry of same +0x54 group already supported"               # §4
attach h = float(base) + entry.+0x18
A) vertical column under the point, if  !(flags&1) and !angled and connIdx<0 and startCell==thisCell
   and (fx-0.5)^2+(fz-0.5)^2 < 0.04:
      bottom, onGround = FUN_1009a469c(sx, sz)          # §6; failure falls to B
      top = floor(h + 0.5)        (computed as ceil(h) - (frac<0.5), l.1640479)
      column at sub-cell (sx, sz) if bottom < top - 1
B) else, unless flags & 2: column at the END sub-cell (ex, ez) + one girder from the start point
      top = floor(h - girderThickness + 0.5) (normal) or floor(h + 0.5) (flags&1, inverted)
```
Positions [D, l.~1640590; `FUN_1009a5460`]:
```
start point  X = (startCellX + (sx + fx)/3) * tile + origin      sx = int +0x08, fx = float +0x10 (Z: +0x0C, +0x14)
column (A)   X = (cellX + (2*sx+1)/6) * tile + origin            -> 1/6, 1/2, 5/6 of the tile
column (B)   X = (endCellX + (2*ex+1)/6) * tile + origin         ex = int +0x24, ez = int +0x28
rotation r=1: (sx,sz)->(sz,2-sx), (fx,fz)->(fz,1-fx); r=2: (2-sx,2-sz),(1-fx,1-fz); r=3: (2-sz,sx),(1-fz,fx)
```
Raw check on `Track5.common.ovl` (Floorless), first sections [D data, script scan of 88-byte records]:
start cell (0,0), **sub (2,1)**, frac (0.5,0.5), h 1.347, flags 2, group 1. The neighbour entries go to (0,1)
with end sub (2,0) and to (0,−1) with end sub (2,2). Sub-index histogram over the file: (1,1) is the most common, but every
value 0..2 occurs. In `OVLExtractor-3/out/*.tracks.json` these ints always appear as 0. `TrackReader.cpp`
`as_f32(q[2..3])`, `as_f32(q[9..10])` turn them into denormals, and `jnum()` zeroes |v| < 5e-7.
**So the tool's 3×3 underside search should be replaced by the sid sub-index.** The game uses no track geometry here.

Entry `+0x50` flags [D]: bit0 = inverted (no vertical path; cap above; girder rests on top of the column),
bit1 = direct only (no girder fallback). Main entries of steel sets are usually `flags 2`, neighbours `0`.
Hanging styles use `1`.

## 4. Which pieces skip their columns ("double supports", "one out of two")

1. **Checkerboard** [D, per-cell: l.1640153-1640154 with `uVar10 = local_fc ^ local_9c` (l.~1640083);
   connection: asm 0x1009a2268-0x1009a22a6, parity of the entry's *start* cell]:
   ```
   allowed = ((x ^ z) & 1) or (not (sid.flags & 0x200) and not inEnclosure(x,z))
   ```
   `inEnclosure` = `FUN_1002f4d00()` singleton (`DAT_1010893a0`): `Tiles` array @0xb8 (stride 0x18,
   `{Region, BrokenState,…}`) has `Region >= 0`. The same manager registers `Regions` with `NumFences`,
   `NumberBrokenFences` and `AssessmentVegitation` (ctor `FUN_100722bba`) → these are **animal enclosure** regions
   (Wild) [D fields, I purpose]. Outside enclosures only flag 0x200 matters.
   Consequence: on a straight run along X or Z of 0x200 pieces, every other tile keeps its column. A diagonal
   run keeps all or none: parity is constant along a diagonal, but 45° sids don't carry 0x200.
2. **One support per group** [D, `FUN_1007d341c` l.1359904 / `FUN_1007d3394`, `FUN_1007d30b2`]:
   `item+0x1a4` holds bit `2i` (entry i built a vertical column) and bit `2i+1` (entry i built a girder column).
   Entry i is skipped when any entry j < i with the same `+0x54` has either bit set. Bits are cleared when the
   cell holding that column is rebuilt. When an earlier entry starts succeeding, the end cells of later
   same-group entries are re-dirtied (`FUN_1009a14ac`). Net rule: **in each `+0x54` group, the first entry in array
   order that can stand wins.** The main entry (index 0, start == end) comes first in shipped sids.
3. **Sub-cell sharing** [D, `local_58` mask]: within one cell rebuild, a girder-path column whose end sub-cell is
   already used by an earlier column of a connection-mode item reuses it. Only the girder is added.
4. No track-chain walk, no "previous piece already supported" test, no check against the Next/Prev pieces.

## 5. Style resolution and special styles

- Style name [D, `FUN_1007d60b0` via `FUN_1009a3f78`]: `item+0xa8 ? item+0xa0 (override string) : sid+0x4c`
  (`sid.supports`, a 32-bit handle resolved through `FUN_1001f2550`; the OVL struct keeps the 32-bit layout).
  `FUN_1009a5406` finds or creates a `SupportEntry` keyed by name (`mgr+0x48`).
  The override setter `FUN_1007d611c` has one caller, `FUN_10053ede2` (a non-track class that copies another
  sid's `+0x4c`). Nothing in the track path sets it, so **track pieces use their own `sid.supports`,
  per piece** (e.g. `loopleft` in the wooden set uses `ts1`) [D for the call graph, I that no track code path sets it].
- Pieces are grouped by SID type (12 middle, 13 top, 14 bottom, 15 bottom-extra, 16 girder, 17 cap), not by name.
  `ts1a`, `ts6` and `ts7` all have middle pieces in `Supports.common.ovl` (`ts6_support_1H..8H`,
  `ts6_supporttop_1H`, `ts6_girder_3L`; `ts1a_*` incl. `ts1a_supportCap`; `ts7_support_*`,
  `ts7_supportbotextra_2H`) [D strings]. So "no columns" for these styles in the tool is not a missing-mesh issue:
  - `ts1a` Sky Swinger: all 59 entries are cross-cell with `flags 1`. `ts6` Flyer: 295 of 352 are `flags 1`,
    plus 25 main entries with `flags 1`. Frequent Faller: 61 of 63 are `flags 1`. The tool's `support_columns` skips any group
    without a `start == end` entry, and it never does the inverted path. The game builds them via path B: column at the
    end sub-cell, top `floor(h+0.5)`, girder over it (`rebuild_cell_visuals.md` §5).
  - `ts7` Giant Flume, `ts5` Heartline, `ts2/ts3` sets: per-cell mode (§3.2), no entries needed.
- Inverted/hanging: there is no downward column. Everything is built bottom → top (see companion §5).

## 6. Terrain and obstacle tests for the support columns

Column bottom finder `FUN_1009a469c(mgr, sx, sz, myIndex, style.+0x74, &bottom, &onGround)` (l.1641502) [D]:
```
for entries after myIndex in the cell list (lower items):
    skip entries with connIdx >= 0
    skip items whose Owner is a TrackPlatformPiece        # FUN_1009a53d8: cast to "SceneryItemPlacementSingle"
                                                          #   (DAT_10108be58) then "TrackPlatformPiece" (DAT_101089d88,
                                                          #   name set at l.1304220). Station platforms are transparent.
    lower = that item
    if lower.sid.flags & 0x400:
        if lower's square at this cell has no solid bit 3 + sx' + 3·sz' (sub-cell rotated into lower's frame):
            continue                                       # the column passes beside it
        stand = style.+0x74 or (lower.sid.flags & 0x100)
    else:
        stand = (lower.sid.flags & 0x100) or style.+0x74
    if !stand: return FAIL                                 # blocked -> vertical path fails, try girder alternatives
    bottom = lower.base + lower.square.+8; onGround = 0; return OK
# nothing below
tmin, tmax = min/max of the tile's 4 corners (FUN_1004cd588)
if FUN_10055b9f0(tmin, tmax, DAT_100e956d8, x, z): return FAIL    # water / volume on the tile
bottom = ceil(tmin); onGround = 1; return OK
```
- Other track pieces (all `0x400`, every sub-cell solid on straight squares) **block**: a column under a
  piece that runs over another piece fails. It moves to a group alternative (girder) or disappears.
  Track sids never carry 0x100, so a column can't stand on track unless the style has `+0x74` (set from the
  support piece sid flags 0x4000/0x8000, see the companion file).
- Non-ride scenery under the point blocks the same way, unless it has 0x100. A bush at height 0 is enough.
  Footprint sub-cell masks matter only for connection-mode items below.
- Height thresholds: connection column needs `ceil(tmin) < floor(h+0.5) − 1` (≥ 2 integer units).
  Per-cell column needs `top − tmin > 0.5` and `ceil(tmin) < top`. CreateColumnRecord rejects
  `top − bottom > 228`. **The 1.1 is not a support threshold.** It is the hollow head-room of §1 (Windows
  "SupportGrid"). The 0.2/−0.5 z-fight also belongs there, not to supports.
- Ground height is the tile's **min** corner, ceiled; the max corner is not used for supports.

## 7. Notes for `rebuild_cell_visuals.md` (other agent) and the tool

- Its "`FUN_1009a53d8` (I: skips ghost/moving items)" → it skips items **owned by a TrackPlatformPiece** [D, class
  names via registration strings]. Its "`cellOccupied` (I: path grid)" → **animal-enclosure region grid** (fields `Region`,
  `BrokenState`, `Regions`, `NumberBrokenFences`) [D fields, I purpose].
- Tool (`OVLExtractor-3`):
  1. `TrackReader.cpp` l.511/515-516: read `q[2],q[3],q[9],q[10]` as int32. `jnum()` destroys them now.
  2. `support_columns`: implement the 0x400 / per-cell split, the checkerboard rule (0x200 + parity of start
     cell), the group rule (first entry of a `+0x54` group that stands), entry flags bit0/bit1, and blocking by
     lower track/scenery (§6) instead of `blocked()`'s `hb < below − 1.1`.
  3. Drop `_support_point` (underside search) and the "nearest sub-cell" guess for neighbours. Use the
     `(2·sub+1)/6` formula with the sid indices.
  4. The 1.1 test → `ceil(tmin) < round(h) − 1`. Ground = min corner.
