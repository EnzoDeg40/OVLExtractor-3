# RCT3 `.trk` — saved coaster design format

A `.trk` is a **saved coaster design**, written by the game when you save a
tracked ride from the build menu. On the macOS/Aspyr build they live under
`~/Documents/Aspyr/RollerCoaster Tycoon 3 <edition>/Coasters/`. It is **not** an
OVL — it is the game's FGDK self-describing object serialization, so it parses
with no schema. `scripts/trk_to_gltf.py` consumes the part the layout lives in.

The file has two parts.

## Part 1 — header + default-initialised object (first ~13 KB)

80-byte header (a `2a da 1e f1`-ish tag near the top, `ff ff ff ff …` at 0x40),
then `<u32 rootCount>` class blocks. Field grammar:

| element | encoding |
|---|---|
| field | `<u16 len><name ascii>` `<u16 len><type ascii>` `<payload>` |
| scalar (`int8/16/32`, `uint8`, `float32`, `bool`) & `string` | `<u32 size><size bytes>` (string size may be 0) |
| `struct` / `array` / `list` | `<u32 a><u32 count>` then `count` nested fields |
| `managedobjectptr`, `vector3` | **fixed** 8 / 12-ish byte header, *no* size prefix (do not treat as a sized leaf) |

In this part nearly every value is **default/zero** (an untested ride has all
stats 0, `OVERLAYFILENAME`/`SYMBOLNAME` empty) — it is a template. The real
layout is in part 2. A tolerant field walker (read tag pairs, resync on drift)
can dump it.

Notable field that names part 2's records: the `SceneryItemDataField` struct,
whose fields are `CORNER, DIRECTION, HEIGHT, POSX, POSZ` (all `int32`, in that
order) — the same five integers each placed piece carries below.

## Part 2 — placed track pieces (tail, ~13 KB → EOF)

One packed record per placed piece:

```
ef ef ef ef                      marker
<UTF-16LE> "<name>:tks"          piece symbol (track section), e.g. "straight:tks"
... <int32 CORNER>    @ symEnd+8    always 0 for track
    <int32 DIRECTION> @ symEnd+12   0..3 quarter turns
    <int32 HEIGHT>    @ symEnd+16   height step (1 unit)
    <int32 POSX>      @ symEnd+20   footprint min-corner tile X
    <int32 POSZ>      @ symEnd+24   footprint min-corner tile Z
    <int32>           @ symEnd+28   constant per design (8029 in MR_compact)
    <int32>           @ symEnd+36   4
    <f32 -1, f32 -1>  @ symEnd+40
    <f32 StartDistance> @ symEnd+48 cumulative distance along the circuit
```

(`symEnd` = first byte after the UTF-16 symbol.) Records are **not** stored
in travel order; sort them by `StartDistance`. An untested design stores 0
for every piece, so the order then has to be recovered by following the
joints (`scripts/rct3_track.py` does both). The ride type is the UTF-16
`<Ride>:trr` symbol in the file (e.g. `WoodenRC:trr`, `BallCoaster:trr`
for the `[CoasterBall]` designs). Each piece's **visual mesh OVL** is named
by the `Tracks\Coasters\<set>\<set>…` references in the same file (e.g.
`Track20`). The set is also the ride's `trr` track path, see
[RCT3_OVL_FORMAT.md §15](RCT3_OVL_FORMAT.md).

The file does not say whether a piece is driven forwards or backwards. That
falls out of the geometry: a `medslope` driven backwards is a down slope,
a `tightcurve` driven backwards is a right turn (§15.4).

### World convention (exact)

| quantity | value |
|---|---|
| grid tile | **4 units** |
| height step | **1 unit** |
| footprint | the piece's `sid` squares (`ovlextract -t track` → `<set>.tracks.json`), swapped on odd DIRECTION |
| centre | `((POSX + sx/2)·4, HEIGHT, (POSZ + sz/2)·4)` |
| rotation | `a = −(DIRECTION + 2)·90°` about Y: `x' = x·cos a − z·sin a`, `z' = x·sin a + z·cos a` (game, left-handed) |

The game frame is left-handed; for glTF mirror Z (positions, translation),
and the rotation keeps the same angle in glTF's `R_y`. With this transform
every joint of the 179 closed circuits among the game's 208 shipped designs
meets its neighbour. The old "even footprint → shift half a tile"
heuristic is no longer needed.

## Reconstructing the ride

```bash
python3 scripts/trk_to_gltf.py '[WoodenRC]MR_compact.trk' -o MR_compact.glb \
    --track-json out/track          # exact footprints from `ovlextract -t track`
python3 scripts/rct3_track.py show  out/track '[WoodenRC]MR_compact.trk'   # piece list, reversed flags
python3 scripts/rct3_track.py check out/track "$ASSETS/Coaster Designs"/*.trk
```

The OBJ meshes must come from a current `ovlextract -t model` (OBJ export
mirrors Z since `ebf95a0`; meshes from older builds are in the game frame and
come out scrambled).

The script reads the pieces, auto-detects the set, reuses one mesh per piece
type across all placements, and writes a `.glb`. Textures are embedded as PNG
when Pillow is available — each OBJ sub-mesh (`usemtl` group) maps to its
`<base>.tga` under the set's `_Textures` folder or `tracks/SharedTextures/`;
without Pillow (or with `--no-textures`) it stays dependency-free and uses flat
per-material colours. Note the `*_Flexi` textures carry magenta placeholder
regions that RCT3 recolours at runtime from the ride's FlexiColour channels —
not applied here.

What it does **not** do yet: tint the FlexiColour regions; place the station
platforms / entrance / exit (those are not `:tks` pieces); resolve the
`managedobjectptr` object graph from part 1; supports.
