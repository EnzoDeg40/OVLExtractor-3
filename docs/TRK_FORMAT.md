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
... <int32 CORNER>    @ symEnd+8
    <int32 DIRECTION> @ symEnd+12   0..3  → 90° yaw steps
    <int32 HEIGHT>    @ symEnd+16   vertical grid level
    <int32 POSX>      @ symEnd+20   grid tile X
    <int32 POSZ>      @ symEnd+24   grid tile Z
```

(`symEnd` = first byte after the UTF-16 symbol.) The ride type appears as
`<name>:trr` (e.g. `WoodenRC:trr`); each piece's **visual mesh OVL** is named by
the `Tracks\Coasters\<set>\<set>…` references in the same file (e.g. `Track20`,
the wooden-coaster piece set) — extract that set with `ovlextract --types model`
and the meshes land in `out/models/tracks/coasters/<set>/<name>_data/`.

### World convention (calibrated from the extracted meshes)

| quantity | value |
|---|---|
| grid tile | **4 mesh units** → world X/Z = `POSX*4`, `POSZ*4` |
| height level | **1 mesh unit** → world Y = `HEIGHT*1` |
| rotation | `DIRECTION * 90°` about **Y** (Y is up — matches glTF) |

Sanity checks on `[WoodenRC]MR_compact.trk`: a `straight` mesh is 4×4;
`tightcurve` is 8×8 (a 2×2-tile 90° turn); `medslope` rises ~2 units (ΔHEIGHT=2),
`steepslope` ~8 (ΔHEIGHT=8); station pieces sit flat at HEIGHT=6 and the
`medslopechain` lift climbs HEIGHT 1→3→5…→30. So every piece becomes a glTF node
= a translation + a 90°-step rotation quaternion, no per-vertex math required.

**Anchor offset.** `POSX/POSZ` name the SW corner of a piece's tile footprint,
but each mesh is modelled centred on its block. A piece spanning an *even*
number of tiles on an axis (e.g. `tightcurve`, 2×2) therefore has its centre
half a tile off the anchor — shift it back by `TILE/2` in world X/Z on each
even axis so curves meet the straights they bridge. (Asymmetric pieces —
`loosecurve`, `sbend`, the slope+curve combos — are only approximate this way;
exact placement would need RCT3's per-piece connection metadata.)

## Reconstructing the ride

```bash
python3 scripts/trk_to_gltf.py '[WoodenRC]MR_compact.trk' -o MR_compact.glb
```

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
`managedobjectptr` object graph from part 1.
