# RCT3 OVL Format — Reverse-Engineering Notes

Findings accumulated while building the cross-platform OVL extractor
(`OVLExtractor-3`). This is **not** a complete spec — only the bits we needed
to extract textures, sounds, and meshes, and that were either undocumented,
wrong in the legacy OVLExtractor-2 source, or absent from cobra-tools.

Reference sources we cross-checked:
- `OVLExtractor-2` (legacy .NET reader by Belgabor et al., partial)
- `cobra-tools-master` (Frontier games — JWE, Planet Zoo, Planet Coaster;
  has **no** RCT3 support for `ftx`/`tex`/`gsi`)
- `rct3dump` (Jonathan Wilson's *RCT3 File Dumper*, 2005, GPL) — the original
  Direct3D-based reference reader (`rct3tex.cpp`). It loads OVLs into memory and
  hands the raw blocks to D3D9/D3DX for decoding, so it doesn't contain a
  hand-written DXT decoder, but it **is** authoritative on: the D3D format-code
  table (§4.4), the `FlicHeader`/`FlicMipHeader` texture layout (§4.1), the 40
  named `txs` shader styles (§12), and the mesh / bone / animation / scenery
  structs (§13). Cross-checked field-by-field against our empirical findings.
- Empirical hex inspection of `.common.ovl` files from the Steam release
- Cross-checks against ~14 952 OVLs from a full RCT3 Complete Edition install

For extractor implementations, CLI workflows and the Blender helper, see
[EXTRACTORS.md](EXTRACTORS.md).


## 1. High-level structure

An RCT3 asset comes as a **pair** of files:
- `<name>.common.ovl` — shared data (textures, palettes, geometry)
- `<name>.unique.ovl` — per-instance data (gsi sprite refs, model instances…)

The parser reads both into `data_[0]` (common) and `data_[1]` (unique).
Cross-side references are normal: a `gsi` on the unique side typically
points at an `ftx`/`tex` texture on the common side. Cross-OVL references
are *also* normal — a `shs` mesh in `Track6.unique.ovl` may reference
`gigacoaster:ftx` in `Track6_Textures.common.ovl` (see §9 for the index
that resolves these).

Each side has:
- A header (magic `0x4b524746` = `"FGRK"`, versions 1/4/5/6 — v6 deferred)
- 9 chunks of payload data (`OvlData::chunks[0..8]`, each a list of `Block`s)
- A loaders table (which loader handles each data block, see §2)
- A linkedfiles table (the actual data entries, each tied to a loader + symbol)
- A symbol-resolves table (relocation slots that bind pointers to symbol names)
- A relocations table (pointer-fixup list)
- An **extra-data stream** right after the relocations: for every loader
  reference in order, `hasextradata` records of `{u32 size, bytes}` that are
  not part of any block (`OvlData::extradata`, §4.1). This is where texture
  pixels live.

Internal offsets in any of the above are resolved to file positions through
`OvlParser::offset_to_position(off)`, which walks the chunk/block table to
find which `Block` contains `off` and returns the corresponding file pos
plus the `OvlSide` of the OVL that hosts it.

The parsing fidelity has been verified on **14 952 real OVL files** from a
complete RCT3 installation (Main + all expansions) — zero parse errors.


## 2. Loader registry

| Tag    | Meaning                          | Status                                  |
|--------|----------------------------------|-----------------------------------------|
| `ftx`  | FlexiTexture (palette indexed8)  | Fully decoded (§3)                      |
| `tex`  | Texture (page of the bitmap table) | Fully decoded (§4): DXT1 / DXT3 / DXT5 / A8R8G8B8, every symbol mapped to its page |
| `btbl` | BmpTbl — FlicHeader table + pixel payloads (extra data) | Fully decoded (§4.1); no symbol of its own |
| `flic` | Flic — page index into the `btbl`, or inline per-mip pixels (v2) | Fully decoded (§4.1); no symbol of its own |
| `txs`  | Texture shader / blend style     | 40 styles known (§12); applied to `.mtl` — opaque/cutout/blend → `d`/`map_d` (§8.5) |
| `fts`  | TextureSet?                      | Treated as ftx (untested)               |
| `ftt`  | TextureType?                     | Treated as ftx (untested)               |
| `gsi`  | Graphic Sprite Info (atlas rect) | Fully decoded (§5); every sprite of the game crops |
| `fct`  | Font (face, glyph table, glyph page) | Fully decoded (§17)                 |
| `txt`  | Text (UTF-16LE string)           | Fully decoded (§18)                     |
| `psi` | ParticleSkin (particle sprite set) | Fully decoded (§21.3): texture, type, mode, animation speed, sprite UV rectangles |
| `snd`  | Sound (.wav)                     | Fully decoded (§6)                      |
| `sid` | SceneryItem (placement, size, price, colours) | Fully decoded (§21.5, §13.4, §15.3): footprint squares and height bits, support connections, cost and removal cost, breakdown timer, default colours, svds / icon / group, parameters, sounds and their scripts, flat-ride fields, add-on pack, billboard aspect; `unk_44`, `unk_E4` have no reader in the game |
| `spl`  | Spline (Bézier rail / profile)   | Fully decoded (§15.1)                   |
| `tks`  | TrackSection (one track piece)   | Fully decoded incl. Soaked/Wild ext (§15.2); connection rules §15.4 |
| `trr`  | TrackedRide                      | Decoded: pieces, trains, track set path, speeds, groups (§15.5) |
| `mms`  | Morph Mesh (vertex animation)    | Fully decoded, every keyframe (§7)      |
| `ter`  | TerrainType (land tool surface) | Fully decoded (§19); `--types terrain` → `.terrain.json` |
| `prt` | PeoplePart | Fully decoded (§20.2, §21.6): texture, mesh, palette slot; the constant `unk_00` (1) is never read |
| `ppg` | PeoplePartGroup | Fully decoded (§20.3, §21.6): person type / sub type / slot, expansion, optional-part chances, selection weight, 2 × 3 LOD part lists, heads; `unk_00`, `unk_08`, `unk_10` are never read |
| `phd` | PeopleHead | Fully decoded (§20.4, §21.6): hat and face attachment matrices, texture, `sal`, 2 × 6 expression skins; the constant `unk_80` (1) is never read |
| `ssk` | SimpleSkin (rigid head mesh) | Fully decoded (§20.5, §21.6): bones, vertex runs, triangles, vertices |
| `shs`  | Static Shape (rigid mesh)        | Fully decoded (§8)                      |
| `bsh`  | Bone Shape (skinned mesh)        | Rest-pose mesh decoded (§8.6); skeletal animation open |
| `ban`  | Bone Animation (keyframes)       | Tracks decoded → JSON sidecar (§8.7); skinned render open |
| `svd`  | SceneryItemVisual (LOD + mesh refs) | Struct known via rct3dump (§13); not extracted |
| `ptd` | PathType | Fully decoded (§21.2): flags, names, textures, 19 × 4 piece names, research, extended tunnel pieces and paving; `unk_154` (always 1) is never read |
| `qtd` | QueueType | Fully decoded (§21.2): names, texture, 7 piece names, research |
| `pty` | PoolType | Fully decoded (§21.2): 89 piece names, research, cost / refund per segment type, water surface offset, edge inset, 6 × 6 swim splines; `unk_1B8`, `unk_1C0` (0.1, 1.0) have no reader |
| `chg` | ChangingRoom | Fully decoded (§21.2): Attraction, scenery item, spline |
| `enc` | EnclosureType (fence) | Fully decoded (§21.2): names, 9 piece names, fence / gallery cost and refund, 4 splines, strength, animal offset, height |
| `asd` | AnimalShelterDesc | Fully decoded (§21.2): hut placement, troughs, eat / drink / sleep splines, food type, proxy tiles |
| `vwg` | ViewingGallery | Fully decoded (§21.2): Attraction |
| `cid` | CarriedItem | Fully decoded (§21.3): names, icon, shapes, flags, carry type, hunger / thirst, cost and three values, litter, extras, billboard, Soaked! morphs; `unk_00`, `unk_08`, `unk_80`, `unk_94` have no reader |
| `ced` | CarriedItemExtra (condiment) | Fully decoded (§21.3): name, icon, hunger, thirst; `unk_0C` (Mustard 4), `unk_18` (spicy 0.3) have no reader |
| `rcg` | ResearchCategory | Fully decoded (§21.3): state, names, initial level, development cost, dependencies |
| `wai` | WildAnimalItem (enrichment item) | Fully decoded (§21.3): name, shape, cost / refund, wear rate, size, icon, texts, animal flags, weight; `unk_04/08/0C` (always 2), `unk_28`, `unk_40..4C` have no reader |
| `was` | WildAnimalSpecies | Fully decoded (§21.3): 4 variants (model, animations, size, texts, icon, sound table), genetics, stats; 13 variant words and a sound-record word have no reader |
| `ent` | EntertainerDesc | Fully decoded (§21.3): speed factors, people parts, height, names, uniform colours, VIP thoughts, animation masks, pose times and offsets |
| `san` | SimpleAnim (people head / neck keyframes) | Fully decoded (§21.4): times, per-channel translation and rotation axes for every key |
| `sal` | SimpleAnimList (bones and animations of one head) | Fully decoded (§21.4): bone names and parents, animation names, `san` references |
| `modelanim` | ModelAnim (wild-animal clip) | Fully decoded (§21.4): bone names (extra chunk), translation and quaternion keys at 30 fps |
| `wad` | WildAnimalAnimData (clip set and locomotion) | Fully decoded (§21.4): clip references, durations, speeds, head-look chains, run blend speeds; 6 scalars (+0x18…+0x24, +0x30, +0x34) have no reader |
| `mdl` | Model (wild-animal skinned model) | Fully decoded (§21.4): bounds, bones (bind pose, inverse bind, parents, names), materials, every LOD's meshes (vertices: position, normal, 4 bone weights, colour, UV) and indices; mesh `unk_0C` (0), `unk_0E` (0xFFFF) have no reader |
| `sta` | Stall | Fully decoded (§21.5): Attraction (cost, refund, upkeep…), scenery item, items sold and their prices, appeal per guest age and gender |
| `sat` | SpecialAttraction | Fully decoded (§21.5): Attraction, scenery item |
| `mam` | ManifoldMesh (water-cut proxy of underwater paths) | Fully decoded (§21.6): bounding box, vertices, triangle indices |

What "fully decoded" buys you, per loader, is documented in
[EXTRACTORS.md](EXTRACTORS.md).


## 3. FTX texture format

Every `ftx` linked file points at the on-disk form of rct3dump's
`FlexiTextureInfoStruct`, followed (normally inline) by a frame-offset array,
one `FlexiTextureStruct` per frame and the 1024-byte palette. Pixel indices
and the optional alpha plane live in a separate chunk. All format codes
(3, 4, 5, 6, 7, 8, 9) share this layout — indexed8 + BGRA palette (+ 8-bit
alpha plane) — so a single decode path works across all of them.

> **Correction (Oct 2026).** Earlier versions of this document (and of the
> extractor) read the palette at `+0x40`. That word is the *alpha-plane
> pointer*; the palette starts at `+0x44` for a single-frame texture. Reading
> from `+0x40` shifted every colour by one palette entry, which produced the
> "noisy / speckled" look of many extracted textures (stall signs, wood,
> metal). With the pointers followed properly, **1594 / 1594** ftx in a full
> Complete Edition install decode, including the 5 entries previously written
> off as "stubs" (§3.7).

### 3.1 Header layout (at `loaderreference.datapointer`)

Every "ptr" is an internal offset fixed up by the relocation table — resolve
it with `offset_to_position()` rather than assuming it is inline.

```
FlexiTextureInfoStruct (36 B)
+0x00 u32  scale         log2 size class: width = height = 2^scale (8 = free dims)
+0x04 u32  width
+0x08 u32  height
+0x0C u32  fps           animation rate, 0 for static textures
+0x10 u32  recolorable   bit 0 / 1 / 2 = 1st / 2nd / 3rd remap colour (§3.8)
+0x14 u32  offset_count  length of the frame-sequence array (1 when static)
+0x18 ptr  offsets    →  u32[offset_count] frame indices (the old "mipmap_count
                         is garbage" quirk: this word is a pointer, §10)
+0x1C u32  frame_count   number of FlexiTextureStruct entries (1 when static)
+0x20 ptr  frames     →  FlexiTextureStruct[frame_count]

FlexiTextureStruct (28 B per frame; for a static texture it sits at +0x28)
+0x00 u32  scale, +0x04 width, +0x08 height     (repeat of the header)
+0x0C u32  recolorable                          (repeat of the header)
+0x10 ptr  palette  →  256 × BGRA (1024 B; for a static texture inline at +0x44)
+0x14 ptr  texture  →  width × height u8 palette indices (separate chunk)
+0x18 ptr  alpha    →  width × height u8 alpha, or 0 when the texture has none
```

Single-frame example (`BalloonStall:ftx`, 256 × 256, block base `B`):
`+0x18 = B+0x24` (offsets array, one `0`), `+0x20 = B+0x28` (frame 0),
frame `palette = B+0x44`, `texture = 0x22D`, `alpha = 0x1022D` — the alpha
plane sits right after the 65 536 index bytes.

Survey over a full install: 1594 ftx, **1169 carry an alpha plane**, 437 are
recolorable, 20 are animated (`frame_count` 3 – 17: lava, water, TV screens…).

### 3.2 Pixel data lives elsewhere

The header + palette occupy the data block at the loader's `datapointer`; the
1-byte **indices** and the **alpha** plane are in a separate chunk, reached
through the frame struct's `texture` / `alpha` pointers. Decode:

```
bgra[i] = palette[texture[i]]          (B, G, R from the palette entry)
alpha[i] = alpha_ptr ? alpha[i] : 255  (palette byte 3 is always 0 — not alpha)
```

which is exactly rct3dump's `ReadTexture2` (`pals[s->texture[i]] |
s->alpha[i] << 24`).

An earlier version of the extractor hard-coded `index 0 → alpha = 0`
(chroma-key heuristic). It worked for textures with a keyed-out background
(Carcass, foliage cutouts) but broke any model where index 0 is a real
color used in the mesh interior — most notably the Dice cube. The alpha
plane is the real mechanism the heuristic was approximating.

### 3.3 The `format_code` is a size class, NOT a pixel format

This is the single most important and most surprising finding. The byte
at offset 0 looks like a "format type" (DXT/RGBA/etc.), but is actually
the log2 of the texture dimension. In the full game scan:

| `format_code` | Dimensions  | Sample count |
|---------------|-------------|--------------|
| 3             | 8 × 8       | 1            |
| 4             | 16 × 16     | 3            |
| 5             | 32 × 32     | 28           |
| 6             | 64 × 64     | 107          |
| 7             | 128 × 128   | 322          |
| 8             | *variable*  | 1130         |
| 9             | 512 × 512   | 3            |

**format=8 is the special case** — non-power-of-2 dimensions allowed.
For all other codes the dimensions field is redundant. The actual on-disk
encoding is identical: indexed8 + BGRA palette. The format byte
encoded a size class for what looks like a memory-layout optimization.

### 3.4 The R-vs-B trap in the palette

Palette entries on disk are **BGRA**, not RGBA. The legacy code's source
comments contradicted themselves (one place said ARGB, another said RGB),
and an early version of this extractor decoded as RGB → produced an image
where a brown carcass (R=139, G=69, B=19) rendered as bright blue
(B=19, G=69, R=139). Always copy palette bytes straight to TGA's BGRA.

### 3.5 Alpha plane vs. material: when transparency applies

The alpha plane is data, but it only **means** something where the sampling
sub-mesh's `txs` style enables alpha testing/blending (§12): `SIAlphaMask*`,
`SiAlpha*`, `BillboardStandard`, `GUIIcon` (cutout) and `SIGlass` (blend).
Opaque styles ignore it — and on the regions only opaque sub-meshes sample,
the plane is frequently **0** (e.g. `HersheyStall`: 63 736 of 65 536 texels
at alpha 0). One ftx is routinely shared by opaque and cutout sub-meshes, so
baking the plane into the one texture an opaque material uses punches holes
in it (Blender renders those faces black / see-through).

Extractor convention therefore:

- `<tex>.tga` — opaque (alpha forced to 255): what opaque materials use and
  what you want to look at.
- `<tex>_rgba.tga` — with the alpha plane, written only when the plane
  exists; `.mtl` cutout/blend materials reference it for both `map_Kd` and
  `map_d`.
- In `.glb` exports (§14) each material embeds the variant it needs:
  `alphaMode: MASK` (cutoff 0.5) + RGBA image for cutouts, `BLEND` for
  `SIGlass`, `OPAQUE` + opaque image otherwise.

### 3.6 The "false positive" with A8

During reverse engineering we briefly believed non-format-8 textures were
A8 (grayscale alpha masks). They looked coherent as grayscale because
RCT3 palettes are frequently near-monotonic and the indices themselves
form recognizable shapes. The real format is palette indexed8 — using the
palette yields full color. (BambooSign should be green, not grey.)

### 3.7 The 5 "stubs" (resolved)

Five ftx used to fail with a "wildly invalid `pixel_internal_offset`" such as
`1065353216` (float `1.0`). That value was read from a fixed `+0x3C`, which
is only the index-plane pointer when the frame struct sits at `+0x28`. With
the `frames` pointer followed, all five decode.

### 3.8 Recolorable textures (remap palette)

When `recolorable != 0` the palette is not a picture palette but RCT3's
**remap palette** — three 85-entry ramps, one per player-chosen colour:

| Indices   | Channel (bit) | Mask colour in the file | Ramp |
|-----------|---------------|-------------------------|------|
| 0         | —             | pure green `00FF00`     | unused / key |
| 1 – 85    | 1st (bit 0)   | blue                    | `E2E2FF` (near white) → `0000FF` (idx 42) → `000028` (dark) |
| 86 – 170  | 2nd (bit 1)   | magenta                 | `FDC3FD` → `FE00FF` → `320332` |
| 171 – 255 | 3rd (bit 2)   | yellow                  | `FFFED0` → `FFFC00` → `272700` |

(values from `GenericStall:ftx`, flags = 7.) Each ramp goes from a whitened
tint through the pure channel colour to a darkened one; the game swaps a
ramp for the same shading of the colour the player picks. Extracted as-is,
recolorable textures therefore show the blue / magenta / yellow mask colours
(GenericStall, most ride and scenery "Flexi" textures). The flag is carried
into glTF material `extras.recolorable`.

**Recolouring like the game** (`--types scenery --recolor A B C`). The
player picks one of **32 swatches** per channel. The game builds, at start-up,
a 32 × 85 table of RGB ramps (`FlexiColour_InitRecolorRampLUT` in rerct3's
Windows notes). Position 41 of a ramp is the pure swatch colour, 0–40
lighten it and 42–84 darken it, and palette index `i` of band `c` is drawn
with `table[swatch_c][i − 1 − 85c]`. On the Mac build the table sits at
`0x100e43480`. It was captured from the running game through
rct3_saves' probe (`scripts/capture_flexicolour_lut.py`) and is byte for byte
the table rerct3 captured on Windows. It ships as
`extractors/src/FlexiColourLut.inc`. A channel value ≥ 32 is the game's
"no recolour" default and keeps the mask ramp. Where the triples come from:
`Track.TrackFlexiColours` (track, platforms, supports),
`TrackedRideInstance.CarFlexiColours` / `CarLightFlexiColours` (cars),
`SceneryItemPlacementSingle.FlexiColourField` (entrance / exit…), all in a
park save **and** in a `.trk` (§15.8); `sid.default_colours` for a fresh
item. Pure swatches (index 41): `0 #2f4343, 1 #839797, 2 #d3dbdb, …, 12
#639b77, …, 18 #f3cb1b, 24 #8f7f6b, 27 #ab0000, 28 #e80701, 31 #ef786d`. Survey: flags 7 (315 textures),
3 (70), 1 (19), 5 (16), 2/4 (7 each), 6 (3).

### 3.9 Animated flexitextures

`frame_count > 1` textures hold one `FlexiTextureStruct` per frame (each with
its own palette / index / alpha pointers) and a frame sequence in `offsets[]`
played at `fps`. `--types texture` writes frame 0 as `<name>.tga` and frame k
as `<name>_fNN.tga` (plus `_rgba` copies when the frame has an alpha plane);
the `.json` sidecar carries `fps` and `frame_sequence` (the `offsets[]` play
order). The install has 20 animated flexitextures (2 to 16 frames: the queue
TV `TVSeq` is a 16-frame on-ride video at 5 fps, `lavabubble`, `UfoLights`,
`flexilight`…); all their 140 extra frames decode.


## 4. TEX texture format — bitmap-table pages

`tex` is the second texture container in RCT3 (alongside `ftx`). Where `ftx`
keeps 8-bit indexed pixels inside its own block, a `tex` symbol is a thin
wrapper that names one **page of the pair's bitmap table**: the pixels are
DXT-compressed (or raw A8R8G8B8) and live in the **extra-data stream** that
follows the relocation table, consumed by the `btbl` and `flic` loaders. The
association is by pointer *and* by load order, which is why the pointer-based
parser alone could not find them (the earlier "trailing-section scan" of this
section was reading that stream without knowing it).

### 4.1 The chain: `tex → flic → btbl → extra data`

Three loaders cooperate (rct3dump `ReadOvl`, OpenCobra `Textures.cs`):

- **`tex`** (TextureStruct, 96 B, on the side that declares the symbol —
  usually unique) carries at **`+0x34` a `FlicStruct**`**: the internal offset
  of the pointer slot that precedes a FlicStruct inside the btbl block.
- **`btbl`** (BmpTbl, usually on common, no symbol) has a data block
  `{u32 unk = 0, u32 count}` followed by `count × {ptr → FlicStruct,
  FlicStruct{Texture* = 0, u32 1, f32 1.0}}` (16 B per entry — one table pads
  an entry to 20 B, hence "follow the pointer, don't assume +4"). It owns
  **two extra-data chunks**: `{u32 0, u32 0, count × FlicHeader}` with
  `FlicHeader = {u32 format, width, height, mipcount}`, then every page's
  pixels back to back, each a packed mip chain sized from its header.
- **`flic`** (one loader reference per page, data pointer = the FlicStruct
  above) owns **one 4-byte chunk: its slot index** into the btbl that precedes
  it in loader order. Loader *version* 2 flics are different: no btbl, the
  single chunk is `FlicHeader` then per level `FlicMipHeader{u32 w, h, pitch,
  blocks}` + `pitch × blocks` bytes (rct3dump's `ReadTexture`, copied row by
  row). 44 pages of the install use this inline layout (path ground
  textures, `test/Shapes`, staff uniforms, rain).

So, for a `tex` symbol: read `+0x34` → the slot's value is the FlicStruct
offset → the `flic` loader reference with that data pointer → its chunk is
the page index → the nearest preceding `btbl` on that side → header `index`
of chunk 0 and payload `index` of chunk 1 (offset = sum of the mip-chain
sizes of the slots before it). `TextureExtractor::resolve_tex_page` does
exactly this; nothing is scanned or guessed.

The texture wrapper itself: eight `0x00070007`, then `+0x20 u32 1`,
`+0x24 u32 8`, `+0x28 u32 0x10` (`0x0D` for a few), `+0x2C` the `:txs` style
slot (resolved by symbol), `+0x30 u32 0x20001` (`0x10001` on some),
`+0x34 FlicStruct**`, `+0x38 ptr` → a `{tex*, flic*}` pair, then zeros. The
`Texture*` inside the FlicStruct is 0 on disk; the game fills it at load.

#### 4.1.1 The extra-data stream

The stream starts right after the relocation table — after one extra `u32`
on v4 files and on v5 files whose header subversion is 1 — and holds, for
every loader reference in on-disk order, `hasextradata` records of
`{u32 size, bytes}` (on v5 the count is the low 16 bits of that field). The
parser records each record's file position and size in
`OvlData::extradata[loader_reference_index]`; `--dump` lists them. Besides
textures, `txt` loaders of `Main` own two records each.

Census over the complete install (184 pairs with `tex` symbols, 783 symbols):
**766 pages decode** — 404 DXT3, 188 DXT1, 174 A8R8G8B8, 0 DXT5 — and the
other 17 are render targets and dynamic textures with no FlicStruct pointer
at all (`DrawSolidColour`, `GUIRenderer*`, `TerrainDebug`, `WaterSurface`,
`FullscreenFadeToBlack`, `Dummy`, `White`, the `ShapeShadow*` …): nothing to
extract, by design.

### 4.2 Page formats

RCT3 reuses the Direct3D format enumeration (rct3dump's
`RCT3DFormatToD3DFormat`). The bitmap tables of the stock game use four codes:

| code   | D3D format     | bytes                | decoded as |
|--------|----------------|----------------------|------------|
| `0x02` | A8R8G8B8       | 4 / pixel            | copied — little-endian ARGB is B, G, R, A in memory |
| `0x12` | **DXT1 (BC1)** | 8 / 4×4 block        | opaque, or punch-through alpha when `c0 ≤ c1` |
| `0x13` | **DXT3 (BC2)** | 16 / 4×4 block       | explicit 4-bit alpha ×17, colour block always 4-colour |
| `0x14` | DXT5 (BC3)     | 16 / 4×4 block       | interpolated alpha (implemented to spec, absent from stock assets) |

The rest of the table for completeness: `0x01` R8G8B8, `0x03` X8R8G8B8,
`0x04` R5G6B5, `0x05` X1R5G5B5, `0x07` P8, `0x08` A1R5G5B5, `0x09` X4R4G4B4,
`0x0A` A4R4G4B4, `0x0B` L8, `0x0C` A8L8, `0x0E` V8U8, `0x10` UYVY, `0x11`
YUY2, `0x15` R3G3B2, `0x16` A8, `0x100`–`0x103` depth formats.

A packed mip chain is the sum over levels of `ceil(w/4) × ceil(h/4) × block`
bytes (levels below 4×4 still take a block), or `w × h × 4` for A8R8G8B8 —
e.g. DXT3 256², 4 mips = 65536 + 16384 + 4096 + 1024 = 87040 B. Only level 0
is decoded to TGA.

### 4.3 Orientation

Every `tex` page is stored **bottom-up**: its first stored row is the bottom
of the picture, as OpenGL uploads it. This holds for the GUI icon sheets
(`GUI2Main`, `GUI2LargeIcons*`, `FrontEndIcons`, `FrontEndParts`,
`StaffIcons`, `TerrainIcons`, scenery icons), the splash logos
(`gui/Banner`), the loading screen, the career-map images of
`gui/FrontScreen`, `FrontEndLogo` and every font page. The texture
extractor writes each page upright (rows reversed) and the atlas extractor
slices that upright page (§5).

Earlier revisions said the GUI icon sheets were top-down. They only look
plausible that way (an icon sheet has no obvious up), but turned the other
way up their people, mascots and arrows stand upright, and the gsi rects
(§5), which count from the top of the upright page, land exactly on their
sprites: cut from the stored rows the track editor's icons came out as
slices of their neighbours (2026-11-10).

### 4.4 History: the "trailing header"

Earlier revisions of this section described a 48-byte header found by
scanning forward from the end of the parsed structures. That was the extra-
data stream seen through a keyhole: `+0x10 = 0x18` was the size of the btbl's
first chunk (`8 + 1 × FlicHeader`), `+0x1C..+0x28` the FlicHeader of slot 0
and `+0x2C` the size of the second chunk. It worked for single-page tables
and found 3 of `Main`'s 88 pages; the chain above finds all 78 that have
pixels. OVLExtractor-2 never decoded `tex` (its handler is a stub).


## 5. GSI — atlas region descriptor

Each `gsi` linkedfile is 16 bytes describing one rectangular region
within a `tex` (or sometimes `ftx`) texture:

```
+0  u32  texture_ref_slot_a — bytes are 0 on disk (relocation slot)
+4  u32  texture_ref_slot_b — bytes are 0 on disk (relocation slot)
+8  u32  coord_block_offset → internal offset to the rect (16 bytes)
+12 u32  padding (0)
```

At `coord_block_offset`:
```
+0  u32  left    — pixel-space
+4  u32  top
+8  u32  right
+12 u32  bottom
```

The texture reference is **not stored in the gsi data block** — both
slots (+0, +4) are zeroed and get filled by relocations at game load.
To recover the link offline, look up `SymbolResolve.pointer == gsi_off + 4`
(or `+ 0` as a fallback) in the symbol-resolves table; its `stringpointer`
field is the texture symbol (e.g. `"Extras:tex"`).

The legacy `OVLReader::ReturnDatablocknameFromOffset((startoffset+4),true)`
in `OVLExtractor-2` was looking up a linkedfile whose `datapointer`
matched `gsi+4` — which only worked for textures stored as separate
linkedfiles. For `tex`-backed atlases that's the wrong table; you need
`symbolresolves` instead.

With every `tex` page reachable (§4.1) the `gsi` of the install crop:
3 651 sprites of 3 679  `Main` alone yielding 1 860 interface sprites from
its 31 GUI pages. The rest are 20 degenerate rects (zero area on disk) and
gsi referencing `RCT1_icons:tex`  a texture no OVL defines  or a render
target. The rect is
in pixels of the **upright** page, from its top-left corner: since pages
are stored bottom-up (§4.3), the rows of a sprite are
`height - bottom .. height - top` of the stored page, read in reverse.


## 6. SND — sound (fully decoded)

Each `snd` linkedfile points at an 80-byte header followed by raw PCM
payload(s) at the offsets named by the header. The layout matches the
legacy `SidSound` struct (cf. `core/include/ovl/SFStructs.hpp`):

### 6.1 Header layout (80 bytes at `loaderreference.datapointer`)

```
+0x00 u16  fmt_tag         — 1 = WAVE_FORMAT_PCM (the only value we've seen)
+0x02 u16  numchannels     — 1 (mono) or 2 (stereo)
+0x04 u32  samplerate      — Hz
+0x08 u32  byterate        — samplerate × blockalign
+0x0C u16  blockalign      — numchannels × (bitspersample / 8)
+0x0E u16  bitspersample   — 8 or 16
+0x10 ...  44 bytes of unknown metadata (volumes, mix params, …)
+0x3C i32  loop            — 0 = one-shot, 1 = loop entire sample
+0x40 u32  channel1        — internal offset to channel 1 PCM
+0x44 i32  channel1_size   — bytes
+0x48 u32  channel2        — internal offset to channel 2 PCM (0 if mono)
+0x4C i32  channel2_size   — bytes
```

### 6.2 Stereo storage

Stereo sounds are stored as two **separate** channel blobs, not interleaved.
Extraction must interleave them to `LRLRLR…` when emitting a standard
stereo WAV. Per-sample bytes = `bitspersample / 8`.

### 6.3 The `loop` boolean

Empirically verified on `Sounds.{common,unique}.ovl`: **276 entries
loop=0, 58 entries loop=1**. All `loop=1` entries are ambient/continuous
SFX (water, hums, lava, gears, hydraulics, …) — one-shots (footsteps,
clicks, voice lines) are uniformly `loop=0`. When the flag is set, the
WAV writer appends a standard `smpl` chunk with a single forward loop
spanning the entire sample, so Audacity / SoundForge / DAWs round-trip
it automatically.


## 7. MMS — morphable mesh (fully decoded)

The `mms` loader holds the vertex-animated meshes: every guest, staff and VIP
body part (§20), the low-detail heads, and the small animals (duck, seagull,
fish…). Each "morph" is one **animation**; every keyframe of it stores every
vertex position and normal. Decoded on 2026-11-12 (all 785 meshes of
`Characters/` and the 13 of `Animals/` decode; checked by rendering bodies,
duck and seagull, and every keyframe of a walk cycle).

### 7.1 Header (40 bytes at datapointer)

```
+0x00 u32  vertex_count
+0x04 u32  index_count        — u16 indices, /3 = triangles
+0x08 u32  position_count     — distinct positions per keyframe (≤ vertex_count)
+0x0C u32  normal_count       — distinct normals per keyframe
+0x10 u32  type_flag          — usually 0, 1 on some attachments
+0x14 u32  morph_count        — animations (192 on a guest body part)
+0x18 ptr  vertices           — 12 B each (§7.2)
+0x1C ptr  indices            — u16 triangle list
+0x20 u32  0
+0x24 ptr  morphs             — 64 B each (§7.3)
```

`position_count` / `normal_count` were the "algo_unknown_1/2" of older notes:
vertices that share a position (UV seams) share one stored position, and
likewise for normals.

### 7.2 Vertex (12 B)

```
+0 u16  position index   — into each keyframe's positions
+2 u16  normal index     — into each keyframe's normals
+4 f32  U
+8 f32  V
```

### 7.3 Morph (64 B) — one animation

```
+0x00 f32[3] bias        — model-space minimum of the animation
+0x0C u32    0
+0x10 f32[3] scale       — metres per quantisation step, per axis
+0x1C u32    0
+0x20 char*  name        — "IdleStandard", "Walk", "RideSitBarLoop", …
+0x24 u32    keyframe_count
+0x28 f32*   times       — seconds, one per keyframe
+0x2C ptr    keyframe data
+0x30 ptr    attachment extras (0 on most)
+0x34 12 B   0
```

### 7.4 Keyframe data

Keyframes follow each other at a fixed stride of
`align4(position_count × 4 + normal_count × 3)` bytes. One keyframe is:

- `position_count` × **u32**, each a quantised position:
  `x = bias.x + (w >> 21) × scale.x` (11 bits),
  `y = bias.y + ((w >> 11) & 0x3FF) × scale.y` (10 bits),
  `z = bias.z + (w & 0x7FF) × scale.z` (11 bits).
  The scales are the animation's extent / (2^bits − 1): a guest torso has
  scale ≈ (1.3e-4, 3.6e-4, 1.2e-4) m, i.e. 0.26 × 0.37 × 0.25 m.
- `normal_count` × **3 × s8**, a direction (not unit length: renormalise).
- 0–3 pad bytes.

Vertex *i* of a keyframe is `positions[pos_index_i]`, `normals[normal_index_i]`.
Positions are model space (Direct3D, left-handed, Y up), so the body parts of
one person line up without any bone: the guest's feet are at y = 0 and it
faces +Z.

The game keeps the decoded positions in a 16-byte stride buffer and
interpolates between keyframes (rerct3 `mesh-animation.md`, morph dispatch
table `0x010ffa48`); the 11/10/11 split is the same bit layout the bone
animations use for their packed quaternions (`Anim_BuildBoneMatrixFromKeyframes`).

### 7.5 What the tools output

- `--types model`: one `<name>.obj` per `mms` in a single pose —
  `IdleStandard` keyframe 0 when the mesh has it (people), otherwise the first
  morph's first keyframe — with UVs and normals.
- `--types people`: §20.

History: older versions of this section tried 17 per-vertex position decoders
(int8, half, packed 10-bit…) on the stream as a vertex array; none worked
because positions are indexed (`position_count` < `vertex_count`) and the
packing is 11/10/11 bits with a per-animation bias and scale.


## 8. SHS — Static Shape mesh (fully decoded)

`shs` is the rigid-mesh format used by scenery, vehicles, and props (everything
that doesn't morph or animate skeletally). Survey across 76 shs from a 40-OVL
random sample: 75/76 (98.7%) extract cleanly into multi-material OBJ. The one
failure is an empty placeholder (`vc=ic=0`).

### 8.1 Header (100 bytes at `loaderreference.datapointer`)

```
+0x00 .. +0x17  bbox: float min[3], float max[3]
+0x18           u32 vertex_count   (sum across all sub-meshes)
+0x1C           u32 index_count    (sum across all sub-meshes)
+0x20           u32 num_submeshes  (duplicated at +0x24)
+0x28           u32 → sub-mesh table          ← THE pointer we follow
+0x2C .. +0x63  scalar + pointer fields, content varies by mesh family
                (transform basis, name string, per-resolve metadata) — none
                of these are required for geometry extraction.
```

The sub-mesh table at `+0x28` is the universal entry point: every variant we
tested (Dice / Litter / 45medslopechain / track pieces / coaster cars) uses
it, regardless of the value of other header fields. The earlier
heuristic-based approach (scanning from `+0x30` for a vertex-shaped record)
worked for single-sub-mesh files but produced truncated meshes for
multi-material shs.

### 8.2 Sub-mesh table

A simple list of pointers to sub-mesh descriptors, terminated by
`0xFFFFFFFF`:

```
ptr_0 (u32) ptr_1 (u32) ... ptr_N-1 (u32) 0xFFFFFFFF
```

The number of sub-meshes equals the number of `(ftx, txs)` pairs in this
shs's SymbolResolve slice (verified on multi-material samples — see §8.5).

### 8.3 Sub-mesh descriptor (≥ 40 bytes per entry)

Fields we use (everything else is `0` or per-variant metadata we ignore):

```
+0x00 .. +0x17  unknown / flag bytes (mostly 0, first u32 = 0xFFFFFFFF)
+0x18           u32 vertex_count    (this sub-mesh)
+0x1C           u32 index_count     (this sub-mesh, count of u32 indices)
+0x20           u32 vertex_offset   (virtual offset to this sub-mesh's verts)
+0x24           u32 index_offset    (virtual offset to this sub-mesh's indices)
```

Verification: sum of per-sub-mesh `vc`/`ic` exactly matches the header totals
(e.g. for `45medslopechain_HI`: 210 + 224 + 24 = 458 ✓ ; 468 + 336 + 36 = 840 ✓).
Indices are sub-mesh-local (`0 .. vc-1` within the sub-mesh's own vertex
buffer — they do **not** index into a global concatenated vertex array).

### 8.4 Per-vertex layout (stride 36 B)

```
+0x00  pos.x, pos.y, pos.z       (3× float32)
+0x0C  norm.x, norm.y, norm.z    (3× float32)
+0x18  0xFFFFFFFF                (sentinel, fixed across all SHS we've seen)
+0x1C  uv.u, uv.v                (2× float32)
```

The sentinel at `+0x18` is the strongest fingerprint of the format. UV `v`
is flipped on output (OBJ `vt v` = `1 - input_v`) to match standard tooling
conventions. The per-vertex normal at `+0x0C` is now emitted as OBJ `vn` (faces
are `v/vt/vn`), giving smooth shading instead of flat-faceted import. This
applies to both `shs` and `bsh` (`bsh`'s `VERTEX2` keeps the same pos+normal
prelude, §13.1).

### 8.5 Material binding

SHS files do **not** embed texture references in the geometry blob. Each
SHS LoadReference owns a slice of `SymbolResolve` entries (filter by
`SymbolResolve.loadpointer == lf.loaderreference.internal_offset`). Within
the slice, resolves appear as consecutive `(ftx_symbol, txs_symbol)` pairs,
**one pair per sub-mesh in order**:

```
slot 0  →  ('gigacoaster:ftx', 'SIOpaqueSpecular50Reflection:txs')
slot 1  →  ('gigacoaster:ftx', 'SIAlphaMaskLow:txs')
slot 2  →  ('chain:ftx',       'SIOpaque:txs')
```

- `:ftx` = the texture (decoded by `TextureExtractor`)
- `:txs` = the shader / blend mode (e.g. `SIOpaque`, `SIAlphaMaskLow`,
  `SIOpaqueSpecular50Reflection`). It is now **applied to the `.mtl`**: each
  sub-mesh becomes one `newmtl <ftx>__<txs>` (so the same texture under two
  shaders yields two materials), and the txs style is classified (§12) as
  opaque / cutout / blend → `d` + `map_d` (cutout materials get `map_d <tex>.tga`
  so the texture's own alpha channel masks the pixels). Verified on the
  Helicopter (8 cutout + 14 opaque materials; `AirCrane__SIAlphaMask` emits
  `map_Kd` + `map_d`).

Survey of 76 shs across a 40-OVL random sample:

- **75/76 (99%)** reference at least one `:ftx`
- **Zero** reference a `:tex` — atlas wrapper textures are only used by `gsi`,
  not by 3D meshes. Cracking `tex` is **not** required for textured models.
- Sub-mesh count distribution: 29× single, 19× double, 20× triple, 7× quad

The texture often does not live in the same OVL as the SHS — coaster pieces
in `tracks/coasters/Track6/45medslopechain_data.unique.ovl` reference
`gigacoaster:ftx` which lives in `tracks/coasters/Track6/Track6_Textures.common.ovl`.
The global symbol index (§9) resolves these cross-OVL references.


### 8.6 BoneShape (`bsh`) — rest-pose decode

`bsh` is the skinned-mesh format (animated scenery, vehicles, animals). It
shares `shs`'s entire structure — the header prelude (vc@+0x18, ic@+0x1C,
sub-mesh table ptr @+0x28), the `0xFFFFFFFF`-terminated sub-mesh table, the
per-sub-mesh local vertex/index buffers, and order-based `(ftx, txs)` material
binding — and differs in exactly two ways:

- **Vertex stride 44, not 36** (`VERTEX2`, §13.1): `pos(12) normal(12)
  bone(u32) unk(u32) color(u32) uv(8)`. The extra `bone`/`unk`/`color` u32s sit
  between normal and UV; `shs`'s `VERTEX` has a single u32 there.
- **u16 indices, not u32.**

The on-disk vertex **positions are model-space, at the bind pose**: every
vertex of a bone sits around that bone's model-space bind transform (checked
per bone on `IceCreamStall`: the "chocolate" vertices cluster at the
chocolate bone's world position, 3 m up). So a rest-pose export uses the raw
positions directly, and the bind pose is also frame 0 of the item's
animation (§8.7), i.e. what the game shows when the animation starts.

#### Bone table (BoneShape1 header, after the sub-mesh table pointer)

```
+0x2C u32  bone_count
+0x30 ptr  bones   → { char* name; i32 parent; }[bone_count]     (8 B each)
+0x34 ptr  local   → D3DMATRIX[bone_count]   bind transform relative to parent
+0x38 ptr  world   → D3DMATRIX[bone_count]   model-space bind transform
```

- **`parent`** is what rct3dump calls `BoneNumber`. It is the **index of the
  parent bone**, `-1` for a root — not a remap of the vertex bone index as
  rct3dump's viewer assumes. Typical stall rig: `Scene Root` (identity),
  the building (`hut`), a spinning sign/prop bone, and `peep01..04` /
  `vendor` marker bones (guest queue and vendor positions, no vertices).
- **`local` (BonePositions1) = parent-relative, `world` (BonePositions2) =
  model space**, with `world = local × parent.world` in D3D row-vector
  convention. Verified exactly on IceCreamStall: chocolate `local`
  translation (-0.141, -0.033, -0.088) composed with its parent '' gives
  (0.593, 3.030, -0.777) = chocolate `world` translation. They are equal
  only for root bones, which is why they looked "≈ identical".
- **Skin weights**: VERTEX2 `+0x18` is **4 × u8 bone indices** and `+0x1C`
  (rct3dump's "unk") **4 × u8 weights summing to 255** — standard D3D
  `BLENDINDICES` / `BLENDWEIGHT`. Most scenery uses one bone per vertex
  (`indices = (b,0,0,0)`, `weights = (255,0,0,0)`), which made the field look
  like a plain u32 bone index. `SunglassesStall` blends up to 3 bones
  (e.g. indices `02 01 03`, weights `0x9B 0x32 0x32`); reading the word as a
  u32 gives "bone 196866" and rejected its three detailed LODs until this was
  fixed.

#### Skinning, for exporters

```
v_model(t) = v_bind · inverse(world[bone]) · W_bone(t)
W_bone(t)  = L_bone(t) · W_parent(t)
L_bone(t)  = R(rotate key) with translation = translate key
             (or local's translation when the bone has no translate track)
```

`SceneryExtractor` (§14) writes exactly this as a glTF skin: joint nodes with
TRS = `local`, `inverseBindMatrices = inverse(world)`, `JOINTS_0` / `WEIGHTS_0` = the 4
indices / normalised weights.

Verified on `WildAnimals/Helicopter` (4 LODs: h/m/l + DartCamMode) and
`WildAnimals/Crates` (40 bsh): **44/44 decode cleanly**, sub-mesh vc/ic sums
match the header, positions land in a plausible model-space box (Helicopter
±8.6 m), and UVs sit in [0, 1] (7552 UVs, 0.2 % outside — consistent with
texture tiling, not a decode error). The clean UV range is the strongest
fingerprint that the 44-byte stride is right (UV is the last 8 bytes of the
vertex; a wrong stride scrambles it). Every one of the 23 animated stalls /
shops / toilets also decodes and validates as a skinned glTF.

### 8.7 BoneAnim (`ban`) — animation tracks → JSON

`ban` holds the skeletal animation keyframes that drive `bsh` meshes. Since OBJ
can't represent a skeleton, the extractor emits the animation **data** as a JSON
sidecar (`<name>.anim.json`) per `ban` symbol — a foundation for a future glTF
exporter or for direct inspection.

On-disk layout (rct3dump §13.3; pointers are internal offsets):

```
BoneAnim      { u32 BoneCount; BoneAnimBone* Bones; f32 TotalTime; }
BoneAnimBone  { char* Name; u32 TransCount; txyz* Translate;
                u32 RotCount; txyz* Rotate; }      (20 B/entry)
txyz          { f32 Time, X, Y, Z; }               (16 B/entry)
```

`Translate` keyframes are **parent-relative** positions; `Rotate` keyframes
are **parent-relative axis-angle vectors** (magnitude = angle in radians,
which may exceed π for continuous spins). The JSON stores each track as
`[[t,x,y,z], …]`.

Verified numerically: on IceCreamStall, Rodrigues(rotate key at t=0) equals
the bone's `local` bind rotation to 3 decimals, for both animated bones —
the bind pose *is* frame 0. rct3dump's `rotmath()` builds the row-vector
(D3D) form of that rotation but has a typo in `_33` (it squares
`sin·nx` instead of using `nz²`); use the textbook formula.

Example: the IceCreamStall cone bone has 6 rotate keys over 4.97 s whose
rotations *relative to the bind pose* are 0°, 179°, 297°, 300°, 302°, 360°,
all about the bone's local +Y: the giant cone **spins once on its axis**
while the chocolate flake bobs (translate track). Linear interpolation
between quaternions taken on the short arc (glTF `LINEAR`) reproduces it,
since consecutive keys are < 180° apart.

A visual (svd LOD) can list several animations, each a full `ban`: the
toilets (`Toilet01`) carry 8 — `Door1Open` … `Door4Close` — one per door
and direction; animated stalls carry one looping idle animation shared by
all their bsh LODs.

Verified on `WildAnimals/Helicopter` and `Crates`: the decode produces readable,
semantically-correct bone names (`tailrotor` @ 3.3 s and `gun` @ 0.667 s on the
Helicopter; `b1a…b2f` rig bones on the Crates) and **every** keyframe time falls
in `[0, total_time]` (6927 Crates keyframes, 0 out of range) — the strongest
evidence that the 20-byte bone stride and the pointer chain are read correctly.

Tracks bind to `bsh` bones **by name** (case-insensitive; an empty name is a
valid bone name and matches). Applying them is done by the glTF exporter
(§14).


## 9. Global symbol index

`ovlextract --build-index <out.json> <Assets/>` recursively scans every
`.common.ovl` under the given directory, parses each OVL pair, and writes a
JSON map of every linked-file symbol → defining OVL. Used by downstream
tools to resolve cross-OVL references (e.g. linking a shs sub-mesh material
to its `:ftx` texture file when the texture lives in a different OVL).

JSON layout:

```json
{
  "gigacoaster:ftx": {
    "symbol": "gigacoaster:ftx",
    "ovl":    "tracks/coasters/Track6/Track6_Textures",
    "side":   "common",
    "tag":    "ftx"
  }
}
```

- **Keys are lowercased.** RCT3's symbol resolution is case-insensitive
  (e.g. references to `StationLights:ftx` target the linked file declared
  as `stationLights:ftx`). The original case is preserved in the `symbol`
  field for display purposes.
- **First match wins on collision.** Symbols that legitimately appear in
  multiple OVLs (commonly used textures duplicated across content packs)
  keep the first occurrence in sorted-path order. The duplicate count is
  reported on stdout for diagnostics.
- **`ovl` is the path stem relative to the scan root**, without the
  `.common.ovl` / `.unique.ovl` suffix — directly consumable by `ovlextract`
  as an `input` argument.

Scale (full RCT3 Complete Edition install, macOS): ~7 500 OVL pairs →
~53 000 indexed symbols, ~5.5 MB JSON, ~10 s cold-cache.

Resolution test on a 40-OVL random sample (179 distinct `:ftx` references
made by 76 shs files): **179/179 (100%)** resolve via the index when the
lookup is lowercased.

End-to-end workflows (`--texture-index` + `--auto-textures`) are documented
in [EXTRACTORS.md](EXTRACTORS.md).


## 10. Quirks worth remembering

- **ftx `+0x18` is a pointer, not `mipmap_count`.** It reads like garbage
  (511821, 263360…) because it is the internal offset of the frame-sequence
  array (§3.1). FTX has no mipmaps on disk.
- **RCT3 is left-handed (Direct3D).** Exporting raw coordinates into a
  right-handed format (OBJ consumers, glTF) mirrors every model: text on
  signs reads backwards ("SESSIK" for "KISSES" on the Hershey stall). Mirror
  Z (positions, normals, translations; rotation axis `(x,y,z)` →
  `(-x,-y,z)`; matrices conjugated by `diag(1,1,-1)`) and swap two indices
  per triangle to restore the winding. UV origin is top-left in both D3D and
  glTF (OBJ needs `v → 1-v`).
- **Animated textures (Dolphin, lavabubble, TVSeq…) used to fail extraction.**
  Symptom: tiny `pixel_internal_offset` (6, 64). Root cause: previous
  attempts read pixels from the wrong block; the palette-decode path
  works fine because both palette and indices are reachable.
- **`ftx` uses no DXT — but `tex` does.** ⚠️ *Superseded note:* we originally
  concluded "no DXT compression in RCT3" after hours of failed DXT1/3/5 +
  RGB565 + BGRA8888 attempts on **ftx**, which really is indexed8 palette. That
  conclusion is correct for `ftx` and **wrong for `tex`**: the `tex` container
  is genuinely DXT-compressed (BC1/BC2/BC3), as the `format_code` at `+0x1C`
  (§4.4) and rct3dump's format table confirm. The two containers simply use
  different encodings — palette for `ftx`, hardware DXT for `tex`.
- **Dice texture appears "stretched vertically"** in the extracted TGA.
  Reading dimensions from offset 4 + 8 gives 128 × 128 (matches what's
  in `format_repeat` block at +0x2C). Not investigated further; likely
  a non-square aspect applied at render time.
- **A relocated `char*` of 0 is a real string** (the first entry of the
  string table), not null. Check the slot against the relocation table
  before treating 0 as null (§15.8: support style names).
- **Header version 6 is deferred.** Both this rewrite and the legacy
  OVLExtractor-2 stop at v5; v6 OVLs are flagged as `parser.valid() == false`
  with a warning but the parser doesn't throw.
- **Hard-skip MMS in bulk batches.** Until position decode is solved, the
  17 diagnostic OBJ variants per MMS file would drown the output of a
  full-install sweep. `ModelExtractor::extract` only emits SHS in batch
  mode; MMS variants are produced when its `process_mms` helper is invoked
  from a code path that opts in.


## 11. What's still open

| Item | Impact when solved |
|---|---|
| `RCT1_icons:tex` | Referenced by 20 `gsi` of `Main`, defined by no OVL of the install (an unshipped RCT1 icon sheet). |
| People head placement (glTF) | The pose is decoded (`san` channels Bip01 Neck / Head, §20.5, §21.4); `--types people` still writes the `ssk` heads in bone space. Applying neck × head to place them is an exporter change, not a reading one. |
| `txs` specular/reflection | Alpha is solved (ftx alpha plane §3.5 + txs class). Still approximate: `*Specular*` → glTF roughness 0.5, `*Chrome*` / `*Reflection*` → metallic 0.8. The environment maps they sample (`chrome:ftx`, `two:ftx`) are not in any OVL — presumably built into the engine — so chrome parts export untextured. |
| Recolor (glTF) | Recolorable ftx export with their blue/magenta/yellow mask ramps (§3.8). The items' default colours are decoded (`sid` `default_colour_1..3`, §21.5, in `.data.json`); applying them by default in `--types scenery` needs the `sid` → `svd` link across pairs (the sid lives in the Style pair). |
| `UseAdTexture:ftx` | Placeholder the generic stall signs bind; the game substitutes the chosen advert at runtime. Exported untextured. |
| OVL header v6 | Not in this install: the 14 952 files of the Mac Complete Edition (Soaked! and Wild! included) are v1, v4 or v5 (header words checked on every file). `OvlParser` still only warns on v6; nothing to extract here. |
| Dice vertical stretch | Minor cosmetic question, not investigated. |


## 12. `txs` shader styles (from rct3dump)

A `txs` symbol (e.g. `SIOpaque:txs`, `SIAlphaMaskLow:txs`) names a render style,
not data — rct3dump resolves it against a hard-coded table of 40 styles
(`rct3tex.cpp:110-354`) and applies it with D3D render states:

```
SetRenderState(D3DRS_SRCBLEND,        style.SrcBlend)
SetRenderState(D3DRS_DESTBLEND,       style.DestBlend)
SetRenderState(D3DRS_ALPHATESTENABLE, style.AlphaTestEnable)
SetRenderState(D3DRS_ALPHAFUNC,       D3DCMP_GREATER)
SetRenderState(D3DRS_ALPHAREF,        style.AlphaRef)   // keep texel if alpha > ref
```

**The actionable rule** (for `.mtl` flags and for ftx alpha re-masking, §3.5):

- **`AlphaTestEnable == true`** → the texture is a **cutout**: texels with
  `alpha ≤ AlphaRef` are discarded. A sub-mesh with such a `txs` *wants*
  texture transparency. This is every `SiAlpha*` / `SiAlphaMask*` style, plus
  `BillboardStandard` and `GUIIcon`.
- **`AlphaTestEnable == false`** → **opaque**; ignore any texture alpha. This is
  every `SIOpaque*` style, plus `SIFillZ`, `SIGlass`, and the opaque `*Chrome`
  variants.

`AlphaBlendEnable` is `true` for all 40. `SrcBlend`/`DestBlend` is
`SRCALPHA`/`INVSRCALPHA` (standard alpha blend) for all but four specials:

| Style              | SrcBlend | DestBlend     | AlphaTest | AlphaRef | Note |
|--------------------|----------|---------------|-----------|----------|------|
| `SIOpaque`         | SRCALPHA | INVSRCALPHA   | no        | 0x00     | the default opaque material |
| `SiAlpha`          | SRCALPHA | INVSRCALPHA   | yes       | 0x08     | standard cutout |
| `SiAlphaMask`      | SRCALPHA | INVSRCALPHA   | yes       | 0x08     | mask cutout |
| `SiAlphaMaskLow`   | SRCALPHA | INVSRCALPHA   | yes       | 0x64     | higher threshold (100) |
| `SiAlphaText`      | SRCALPHA | INVSRCALPHA   | yes       | 0x32     | text (threshold 50) |
| `BillboardStandard`| SRCALPHA | INVSRCALPHA   | yes       | 0x80     | billboards (threshold 128) |
| `SiAlphaMaskChrome`| ONE      | ZERO          | yes       | 0xD0     | opaque replace + high cutout |
| `GUIIcon`          | ONE      | ZERO          | yes       | 0xD0     | GUI sprites |
| `SIFillZ`          | ZERO     | ONE           | no        | 0x00     | depth-only (writes no colour) |
| `SIGlass`          | ONE      | INVSRCALPHA   | no        | 0x08     | additive glass |

`AlphaRef` values seen: `0x08`(8), `0x32`(50), `0x64`(100), `0x80`(128),
`0xD0`(208). The full 40-row table (all the `*Specular*` / `*Reflection*` /
`*Chrome*` / `*Unlit*` permutations) is in `rct3tex.cpp:110-354`; they only
differ from the representatives above in name and `AlphaRef`, never in a way
that changes the opaque-vs-cutout decision.


## 13. Other structures recovered from rct3dump (for future work)

`rct3tex.cpp` is primarily a **mesh viewer**; textures are a means to an end.
Its struct definitions are the most complete public description of RCT3's
geometry / scenery formats, so they're transcribed here for when the model
side (`shs`/`bsh`/`mms`) is extended. All structs are the **in-RAM** form
(pointers fixed up by relocations); on disk those pointers are internal
offsets resolved via `offset_to_position()`.

### 13.1 Vertex layouts — confirms §8.4

```
VERTEX  (static, shs — 36 B)          VERTEX2 (skinned, bsh — 44 B)
+0x00  float position[3]               +0x00  float position[3]
+0x0C  float normal[3]                 +0x0C  float normal[3]
+0x18  u32   color  (D3DCOLOR)         +0x18  u8[4] bone indices (§8.6)
+0x1C  float u, v                      +0x1C  u8[4] bone weights, sum 255
                                       +0x20  u32   color (D3DCOLOR)
                                       +0x24  float u, v
```

**Cross-check:** the "`0xFFFFFFFF` sentinel at +0x18" we identified empirically
in §8.4 is actually the **vertex `color`** field (D3DCOLOR), which is
`0xFFFFFFFF` = opaque white on virtually all scenery. So it's a real field, not
padding — worth emitting as OBJ/glTF vertex colour rather than discarding.

### 13.2 Static mesh (`shs`) — confirms §8.1–8.3

```
StaticShape1 (header)                  StaticShape2 (sub-mesh)
  D3DVECTOR BoundingBox1, 2              u32       unk1 (0xFFFFFFFF)
  u32 TotalVertexCount, TotalIndexCount  ptr       fts  (FlexiTextureInfo, 0 on disk)
  u32 MeshCount2, MeshCount              ptr       TextureData (0 on disk)
  StaticShape2** sh   ← sub-mesh ptrs    u32       PlaceTexturing, textureflags, unk4
  u32 EffectCount                        u32       VertexCount, IndexCount
  D3DMATRIX* EffectPosition              VERTEX*   Vertexes
  char**     EffectName                  u32*      Triangles  (32-bit indices)
```

`EffectName[]` (e.g. attachment / light points) is a bonus the current extractor
doesn't surface. Note `shs` triangles are **u32**; `bsh` triangles are **u16**.

### 13.3 Skinned mesh (`bsh`) + animation (`ban`)

```
BoneShape1                             BoneStruct
  D3DVECTOR BoundingBox1, 2              char* BoneName
  u32 TotalVertexCount, TotalIndexCount  i32   BoneNumber  ← actually the PARENT index (§8.6)
  u32 MeshCount2, MeshCount
  BoneShape2** sh                       BoneShape2: like StaticShape2 but
  u32 BoneCount                           Vertexes are VERTEX2, Triangles u16
  BoneStruct* Bones
  D3DMATRIX* BonePositions1  ← bind pose, parent-relative (§8.6)
  D3DMATRIX* BonePositions2  ← bind pose, model space

BoneAnim (ban)         BoneAnimBone               txyz (keyframe)
  u32 BoneCount          char* Name                 float Time
  BoneAnimBone* Bones    u32 TranslateCount         float X, Y, Z
  float TotalTime        txyz* Translate
                         u32 RotateCount
                         txyz* Rotate
```

rct3dump's skinning experiment (`DoShapes`) multiplies each vertex by
`BonePositions1[vertex.Bone]`, overwrites the matrix with the keyframe, and
then **discards** the result (it uploads the raw positions). It treats
`BoneNumber` as a bone remap. Both are wrong; the working recipe is in §8.6:
`BoneNumber` is the parent index (`-1` = root), `BonePositions1` the local and
`BonePositions2` the model-space bind transform.

### 13.4 Scenery visual (`svd`) and item (`sid`)

```
SceneryItemVisual (svd)                SceneryItemVisualLOD
  u32 unk1, unk2                         u32   MeshType  (0 = StaticShape, 3 = BoneShape)
  float unk3, unk4                       char* LODName
  u32 unk5                               StaticShape1* StaticShape (0 on disk)
  u32 LODCount                           BoneShape1*   BoneShape   (0 on disk)
  SceneryItemVisualLOD** LODMeshes       float distance  (LOD switch distance)
  u32 unk6..unk11                        u32   AnimationCount
                                         BoneAnim*** AnimationArray
```

On-disk layout as used by `read_scenery_visual` (verified on all 71
stalls / shops / restaurants / utilities):

```
svd  +0x00 u32 flags (0x70 on stalls)  +0x14 u32 LODCount  +0x18 ptr → ptr[LODCount] → LOD
LOD  +0x00 u32 MeshType (0 shs, 3 bsh)   +0x04 char* LODName
     +0x08 slot StaticShape  /  +0x10 slot BoneShape   ← symbol resolves "name:shs|bsh"
     +0x38 f32 distance   +0x3C u32 AnimationCount   +0x44 ptr → ptr[AnimationCount] → slot
     (each slot is patched by a symbol resolve to "name:ban")
```

The mesh/animation "pointers" are 0 on disk; find the `SymbolResolve` whose
`pointer` equals the slot's internal offset to get the referenced symbol.
Stalls have 4 LODs, e.g. IceCream: `icecreamlod100` (bsh, 503 tris,
distance 40) / `lod60` (bsh, 100) / `lod20` (bsh, 300) / `lod04` (shs, 4000);
the last LOD samples a shared low-detail atlas
(`VanillaShopsUltraLowPage01:ftx`). `distance` is the far switch distance in
metres.

#### Shop metadata: `sta` (Stall) / `sat` (SpecialAttraction) and `txt`

A shop's gameplay record is a `sta` (stalls) or `sat` (toilets, ATM, first
aid, information kiosk…). Only 4 stalls ship it in their own OVL pair; the
rest live in the theme's `Style/<Theme>/Style.unique.ovl`, **named after the
svd** (`HotDogStall:svd` ↔ `HotDogStall:sta`). Its symbol resolves are the
useful part:

| resolve | meaning | example (IceCream:sta) |
|---|---|---|
| `…Name:txt` / `…Description:txt` | display name / description | "Ice Cream" / "A stall selling various flavors of ice cream." |
| `…:cid` (×n) | carried items sold | `IceCream`, `IceCreamDouble`, `IceCreamChoc`, `IceCreamChocDouble` |
| `…:sid` | the scenery item placed in the park | `IceCreamStall:sid` |
| `:gsi`, `:spl` | icon and (empty-named) spline slots | |

`txt` resources live in `gui/<Language>.common.ovl` (English, French,
German…): the datapointer holds a **null-terminated UTF-16LE** string.
`--types scenery` writes the name, description, `sid` and items into the
manifest's `shops` array and the name into the glTF scene `extras.name`
(66 of the 71 shop/stall/utility visuals resolve; the 5 restaurants have no
`sta`/`sat`).

`sid` (`SceneryItem`) is large (~50 fields): 64 placement flags, `size` class,
`xsquares`/`ysquares`, world `xpos/ypos/zpos` + `xsize/ysize/zsize`, `cost`,
`refund`, `type` (see the 47 `TypeNames`: tree, fence, ride track, stall …),
`svdcount` + `svd**` (its visuals), a `gsi` icon, `OvlName`, and a wide-char
`Name`. rct3dump dumps a trimmed `SIDData` record per item
(`rct3tex.cpp:2162`). Full layout: `rct3tex.cpp:954-1041`.

### 13.5 GUI icon rect (`gsi`) — confirms §5

```
GUISkinItem            GUISkinItemPos
  u32 unk1               u32 left, top, right, bottom
  TextureStruct* tex     (rct3dump carries a "swapped left/top" caveat — its
  GUISkinItemPos* pos     author flipped them vs. the on-disk order, so trust
  u32 unk2                §5's empirically-verified left/top/right/bottom)
```

### 13.6 Container parsing reference

`ReadOvl` (`rct3tex.cpp:1602`) + `DoReloc` (`:1500`) are a complete, if terse,
implementation of the v3/v4/**v5** OVL container: header variants, the 9
file-type blocks, the relocation/pointer-fixup pass, and symbol resolution
(`FindSymbol`, `:2723`, which also handles the `:txs` style lookup). Useful as
a second opinion if a v5 edge case ever disagrees with our parser. Loader
dispatch by tag is the big `if (stricmp(LoaderNames…))` ladder at `:1891`
onward — a ready-made catalog of every loader tag and the struct it maps to.


## 14. Packaging scenery as glTF (`--types scenery`)

A scenery item (stall, shop, restaurant, toilet, decoration…) is spread
across loaders: the `svd` lists LODs, each LOD names a `shs`/`bsh` and its
`ban` animations, each mesh binds `(ftx, txs)` pairs, and the textures may
live in other OVLs (`Style/sharedtextures/…` for the cash register, the
`*UltraLow*` atlases…). `SceneryExtractor` follows that graph and writes one
self-contained glTF 2.0 binary per item:

| glTF | from |
|---|---|
| mesh primitives | one per sub-mesh (§8.2); positions/normals Z-mirrored, winding swapped (§10) |
| materials | one per `(ftx, txs)`; `alphaMode` from the txs class (§12), `doubleSided` for cutouts/blend, `extras: {ftx, txs, recolorable}` |
| images | ftx (§3) / tex (§4) decoded and embedded as PNG — opaque copy for opaque materials, RGBA copy (alpha plane) for MASK/BLEND |
| skin | bsh bone table (§8.6): joints = bones, hierarchy from `parent`, TRS from `local`, IBM = inverse(`world`) |
| animations | one per `ban` on the LOD, tracks matched by bone name; translations Z-mirrored, axis-angle → quaternion with `(-x,-y,z)` axis, hemisphere-continuous, `LINEAR` |
| scene `extras` | `{rct3_svd, lod, lod_distance, name}` (name from the shop's `txt`, §13.4) |

The most detailed LOD (most triangles) goes to `<svd>.glb`; `--all-lods`
adds `<svd>_<mesh>.glb` for the others. A `<svd>.json` manifest lists every
LOD (mesh, type, distance, vertex/triangle/bone counts, animations), the
materials with their alpha mode and the OVL each texture came from, the shop
record (`sta`/`sat`: display name, description, sold items, `sid`) and any
unresolved texture.

Validation (Oct 2026): every `ShopsAndStalls`, `Resturants` and `Utilities`
item of the Complete Edition — 71 items, 23 of them skinned + animated, plus one orphan placeholder mesh —
exports with **0 errors / 0 warnings** in the Khronos glTF validator and
renders correctly in Blender (textures, cut-outs, readable sign text, cone
spin on the ice-cream stall, door animations on the toilets). The only
unresolved textures are engine-side: `chrome:ftx` / `two:ftx` (reflection
maps) and `UseAdTexture:ftx` (advert placeholder).



## 15. Tracked rides: `trr`, `tks`, `spl`, `sid` (`--types track`)

A tracked ride (coaster, flume, transport ride, slide…) is spread over three
places:

| where | holds |
|---|---|
| `Tracks/Coasters/<Ride>/<Ride>.ovl`, `tracks/TrackedRides/<Ride>/…` | the `trr` (TrackedRide): ride parameters, the list of pieces it may use, its trains, the **relative path of the track set** holding the pieces (`../Track20/Track20`), plus profile `spl`s |
| `…/<Set>/<Set>.ovl` (e.g. `Track20`) | one `tks` (TrackSection) + one `sid` (SceneryItem) + two `spl` (left/right rail splines) **per piece** |
| `…/<Set>/<piece>_data.ovl` | the piece's `svd` + `shs` LODs (meshes, `--types scenery` / `model`) |
| `…/<Set>/<Set>_Textures.ovl`, `tracks/SharedTextures` | the textures |

The layouts below come from rct3-importer's `tracksection.h`,
`trackedride.h`, `spline.h`, `rct3constants.h` (Jonathan Wilson / Belgabor,
mirrored at github.com/chances/rct3-importer) and the rerct3 Ghidra notes
(`rerct3/research/re/track.md`). Each was checked against the data, and
corrected where the data disagreed. `ovlextract -t track` decodes every
`trr`/`tks` of the Complete Edition (121 rides, 79 track sets, 3 805
sections) with 0 errors.

### 15.1 Spline (`spl`)

```
+0x00 u32  nodecount            +0x04 ptr  SplineNode[nodecount]
+0x08 u32  cyclic               +0x0C f32  total length    +0x10 f32 1/length
+0x14 ptr  f32 lengths[seg]     +0x18 ptr  SplineData[seg] (14 B, opaque)
+0x1C f32  max_y
SplineNode (36 B) = pos, cp1, cp2 (f32×3 each); cp1/cp2 are RELATIVE to pos
segment i = cubic Bézier  pos[i], pos[i]+cp2[i], pos[i+1]+cp1[i+1], pos[i+1]
```

### 15.2 TrackSection (`tks`) — the piece definition

`TrackSection_V` is 228 B; when `entry_flags & 0x00400000` (Extended) a
136 B Soaked extension follows (version 2), and a 28 B Wild one (version 3).
Most retail sets are version 3; Vanilla-only sets are version 1.

| off | field | notes |
|---|---|---|
| +0x00 | char* internal name | = symbol |
| +0x04 | `sid` ref | footprint + mesh, §15.3 |
| +0x08 / +0x0C | entry / exit **curve** | 0 Straight, 1/2/3 Gentle/Medium/Sharp **Left**, 4/5/6 Gentle/Medium/Sharp **Right**, 7 Special. The curve-slider value that selects the piece: entry = building forwards, exit = building backwards, so a left curve has (1,4), (2,5), (3,6) |
| +0x10 | special type | the "special pieces" menu entry: 1 Brakes, 2 BlockBrakes, 3 Station, 12 Loop, 7 Corkscrew… (`tks_special_name`; values are reused by later sets) |
| +0x14 | direction | 0 symmetric/straight, 1 left, 2 right. A curve with direction 0 (`tightcurve`) is the left version; the right turn is the **same piece driven backwards** (§15.4) |
| +0x18 / +0x1C | entry / exit flags | `StartSection 0x1, Chain 0x2, ForwardOnly 0x4, Capped 0x8, Transfer 0x10, CableLift 0x20, Station 0x40, TowerRideBase 0x80, Hold 0x100, LiftBottom/Middle/Top 0x200/0x400/0x800, WaterOption 0x1000, Accelerating 0x4000, Reverser 0x10000, AlternateSpline 0x20000, Extended 0x400000, Booster 0x1000000…` |
| +0x20 / +0x24 | left / right rail `spl` | **R is the geometric right rail** in the game's left-handed frame |
| +0x28…+0x34 | join L/R, extra L/R `spl` | join = same as main on most pieces; extra = reverser / alternate car path |
| +0x48 / +0x4C | entry **slope** / **bank** | slope 0 Flat, 1 MediumUp, 2 MediumDown, 3 SteepUp, 4 SteepDown, 5 VerticalUp, 6 VerticalDown, 7 AutoEnd |
| +0x50 | char* entry track group | rarely set |
| +0x64 / +0x68 | exit slope / bank | |
| +0x6C | char* exit track group | |
| +0x70 / +0x74 | speed count / ptr | 3 f32 (Vanilla) / 5 f32 (Extended) per entry, e.g. station `[-99999, 8, 10000, -1, -1]` |
| +0x8C | animations | Vanilla: 9 inline i32 (stopped, starting, running, stopping, hold…); Extended: count + ptr |
| +0xD4…+0xDC | water-spline flag + L/R `spl` | |
| Sext +0x20 | char* auto group | auto start/mid/end selection |
| Sext +0x30/+0x34 | entry / exit wide | wide-track rides |
| Sext +0x58…+0x84 | 6 × (count, char**) | group is-at / must-have / must-not-be at entry / exit |
| Wext +0x00 / +0x08 | splitter half / rotator type | Splitting coaster, rotating tower |

**Bank.** The enum is the **roll in 45° steps, positive = banked right**
(right rail lower): 0 flat, 1 right 45°, 2 right 90°, 3 right 135°, 4
inverted, 5 left 135°, 6 left 90°, 7 left 45°. Measured on the rail
splines of all 3 805 sections. rct3-importer's names (`Half_Left = 1`, …)
are **mirrored** left/right.

**Slope** checks out as named: measured pitch 27° (medium, 2 height
units per tile) or 19° on diagonal pieces, 63° / 54° (steep), 90°.

### 15.3 SceneryItem (`sid`) — footprint

The subset a ride needs (full struct: rerct3 `scenery-item.ts`):
`+0x08 u16 position type` (0 for track: footprint-centred),
`+0x10/+0x14 squares x/z`, `+0x18 ptr cells[x·z]` (20 B: flags, height
adjust, height span, ·, direction type), `+0x1C f32[3] position`,
`+0x28 f32[3] size` (metres), `+0x3C cost`, `+0x48 type` (18 = ride track),
`+0x4C char* supports` (support style, e.g. `ts1`), `+0x50/+0x54 svd count/refs`,
`+0x8C u32[3] default colours`.

### 15.4 Piece frame, placement and the connection rules

**Local frame.** A piece is modelled centred on its footprint
(`squares × 4` units) and travels along **local −X**. The rail spline
mid-points give the joints: the flat straight goes from (2, 1.25, 0) to
(−2, 1.25, 0), the 2×2 `tightcurve` from (4, 1.25, 2) to (−2, 1.25, −4).
The rail height above the piece's HEIGHT level is a per-set constant
(1.25 on Track20, 0.13 on the monster trucks…). Gauge is 1.45 on Track20.

**Placement.** A placed piece (`.trk` record, park save) stores its
footprint's min-corner tile `(X, Z)`, a height step `H` and a quarter-turn
`D`. Its world transform is

```
squares (sx, sz) swapped when D is odd
centre = ((X + sx/2)·4, H, (Z + sz/2)·4)
a      = -(D + 2)·90°            x' = x·cos a − z·sin a,  z' = x·sin a + z·cos a
```

which is rerct3's `SceneryItem_InitRotationMatrix` (`(dir+2)·π·−0.5`).
Verified exactly: with it, every joint of every closed circuit of the 208
shipped designs meets its neighbour (≤ 0.2 units, the MonsterTrucks
splines are authored that loosely).

**Reversal.** A piece may be driven backwards: the train enters at its exit
joint. The file does **not** store this. It falls out of the geometry,
and 30–45 % of a typical layout is reversed: every down slope (`medslope`
backwards), every right turn of a symmetric curve (`tightcurve`
backwards). Driving backwards maps
`slope: Up ↔ Down` (1↔2, 3↔4, 5↔6) and `bank: b → (8 − b) mod 8`.

**What may follow a piece** — rules that hold on the shipped designs
(`scripts/rct3_track.py check`):

1. **Same (slope, bank)** on both sides of the joint, in travel direction:
   `out(A) == in(B)`, where for a forward piece out = exit, in = entry,
   and for a reversed one the reversal mapping applies. So after a
   `straight2medslope` (out = MediumUp/Flat) only pieces entering
   MediumUp/Flat fit: `medslope`, `medslope2straight`,
   `medslope2steepslope`, `medslopetightcurveleft`, the chain versions…,
   or any piece whose *exit* is MediumDown/Flat driven backwards. To turn
   on a slope you need a `medslope…curve` piece. On the flat you cannot
   bank without a `straight2tilt…` transition. Bank and slope transitions
   are separate pieces, and the joint state is exactly (slope, bank).
2. **Grid heading**: joints are either axis-aligned or on a 45° diagonal
   (`45*` pieces, reached through `loosecurve`s). A diagonal joint only
   meets a diagonal one. The quarter turns D cover the rest.
3. **ForwardOnly** pieces (most stations, block brakes, some specials) are
   meant to be driven forwards only. The shipped designs break this 7 times,
   always with a station or a tower cap as the start piece, so it is a
   building-UI rule rather than a hard one.
4. **Groups**: entry/exit track groups and the Soaked/Wild
   is-at / must-have / must-not-be lists constrain special sequences
   (lift tops, tower bases, slide ends).
5. The piece must be in the ride's `trr` section list (§15.5).

Result on the 208 `Assets/Coaster Designs/*.trk`: 179 closed circuits fully
connected, 22 open layouts (slides, shuttles, launchers) with exactly the
one expected gap, the 3 Splitting-coaster designs (two parallel tracks)
not handled; 14 506 / 15 014 joints connected overall, and only 12
connected joints outside rules 1–3 (the 7 ForwardOnly starts above + 5
white-water-rapids joints whose water splines meander off the grid
heading).

### 15.5 TrackedRide (`trr`)

Vanilla `TrackedRide_V` = `Attraction_V` (56 B) + `Ride_V` (24 B) +
`TrackedRide_Common` (300 B). Soaked/Wild rides (most of the retail set,
`WoodenRC` included) start directly with Common, whose first u32 is then
`0xFFFFFFFF`, followed by an extension. Common, by **0-based** u32 slot (the
`// 10`, `// 20` comments in trackedride.h count from 1, and reading them as
0-based shifts everything after `lift_car` by one):

| slot | field | | slot | field |
|---|---|---|---|---|
| 0 | section count (V) / 0xFFFFFFFF | | 41–43 | upkeep per train / car / station |
| 1 | `tks*[]` the ride's pieces | | 44 | car rotation step |
| 2, 3 | train name count, `char*[]` | | 47 | loop not allowed |
| 4 | char* cable lift | | 51 | cost per 4 height units |
| 9 | char* lift car | | 52 | char* track path (Vanilla) |
| 14 | char* station name (platform style) | | 53–56 | other top/mid, tower top/mid `tks` |
| 15, 16 | platform height over track, only on water | | 57 | auto complete |
| 18–20 | start preset, start possibilities, blocks possible | | 60, 61 | chain speed min / max |
| 21 | station roll speed | | 64, 65 | section-cost count, `{char* name; u32 cost}[]` |
| 23, 25 | launched / chain speed preset | | 66, 67 | launched speed min / max |
| 26–30 | chain force, brake speed, brake force, block brake speed, booster force | | 35 | gravity scale |
| 31 | constant speed preset (< 0 = none) | | 69–72 | track, track-big, car, car-swing profile `spl` |
| | | | 74 | constant speed hill rate (`+0x128`, < 0 = a quarter of the constant speed) |

Soaked/Wild extension (after the 300 B): `+0x04` section count, `+0x1C/+0x20`
group-definition count / arrays (`{u32 n; {flags, char* group | tks ref}×2 [n]}`,
the auto start/mid/end rules), `+0x3C/+0x40` track-path count / `char*[]`.

E.g. `WoodenRC`: track path `../Track20/Track20`, 85 pieces, trains
`WoodenCoaster, WagonCoaster, 6PeepWoodenRC, …`, station `Wooden`, chain
speed 4 m/s (2.24–6.26), upkeep 1100/300/1000.

A powered ride drives its train at the constant speed (`FixedSpeed` on the
ride, seeded from slot 31): rerct3's train update clamps the speed, while
running, to between that speed minus the hill rate when a car climbs and
plus it when one descends (`ride-physics.fixed-speed-hill-rate`). `Ghost`:
4 m/s, hill rate −1 (so 1 m/s), gravity scale 0.1.

### 15.6 Output of `--types track`

`<set>.tracks.json`: per section — name, version, special (+name),
direction, flags (+names), `entry`/`exit` `{slope, slope_name, bank,
bank_name, curve, group, wide}`, groups, speeds, animations, spline symbols,
**`geometry`** (length + entry/exit `{pos, dir, up, yaw, pitch, roll,
gauge}` measured on the rails, local frame) and the **`sid`** (squares,
size, cost, supports, svds, cells); then every referenced spline (nodes +
lengths). `<ride>.ride.json`: the §15.5 fields, the piece list and costs,
group definitions and the profile splines. `scripts/rct3_track.py`
implements §15.4 on top of these files.

### 15.7 Trains (`rit`) and cars (`ric`)

`Cars/CoasterCars/<Train>/<Train>.ovl` (and `Cars/TrackedRideCars/…`) hold
one `rit` (RideTrain) + its `ric` (RideCar) + the car `svd`/`bsh`/`ban`.
The trr's `train_names` are rit symbols. 186 trains, 0 errors.

`RideTrain_V` (188 B): `+0 char* name` (or `0xFFFFFFFF` → Soaked/Wild, the
name is then in the extension at +188: `version, char* name, icon`),
`+4 description`, **`+8…+28` car slots front, second, mid, penultimate,
rear, link**, `+32/36/40` cars per train min/max/default, `+44/48/52`
dynamic friction / static friction / air drag (0.04/0.06/0.02),
`+56` camera look-ahead, `+140` overtake, `+176/+180` lift-car splines,
`+184 char* station` (platform style, overrides the trr).

**Train composition**: car 0 = front, car 1 = second (if set), the last =
rear (if set), the one before = penultimate (if set), every other one =
mid; an empty mid means "front again". The link object (`ArrowsLink`…)
sits between cars. Only the front slot is set on 120 of 186 trains
(identical cars). The steam train is front = Caboose, second = Tender,
mid = Carriage. The inverted dragon uses all five (head, legs, middle,
legs, tail).

`RideCar_V` (240 B): `+0 name, +4 username, +8 u8 seating, +9 u8 version,
+12 svd (body), +16 inertia, +20 moving svd` (spinning/swinging part),
`+24 axis type` (0 none, 2 across the track, 3 along, 4 vertical),
`+28 moving inertia, +32 axis stability, +44 axis momentum, +56 axis max,
+60 bobbing, +76 20 animation indices` (start, started idle, stop, stopped
idle, belt open/idle/close/closed idle, doors open/idle/close/closed idle,
row both/left/right, canoe ×5; each indexes the svd's `ban` list),
`+156/+160 seat types, +164…+192 four wheel svds + flipped flags, +196/+200
axle svds, +204/+208 axle types`.

**Car frame**: unlike track pieces, a car points along its local **+Z**
(game frame; right = +X, up = +Y). The front cars' noses (Arrows,
InvertedDragonHead) extend to +Z, the dragon's tail to −Z. Cars are
spaced by their mesh length along Z (WoodenCoaster 2.52); rerct3 reads the
same value from the svd bounding box at runtime (`Stats.Length`,
RideCarInstance +0x190).

### 15.8 Station platforms and park saves

`trr.station_name` (or `rit.station`) names a platform style:
`Tracks/Platforms/Vanilla/<Style>.ovl` holds 16 `sid`s
`<Style>{start,middle,end,startEnd}{,1stairLeft,1stairRight,2stair}`, whose
meshes are in sibling `_data` OVLs. A platform mesh covers the whole 4×4
tile (deck on both sides of the rails). Read from the campaign parks
(`rct3_saves`, class `TrackPlatformPiece`):

- each station track piece owns one platform piece with the **same tile,
  height and direction** (`TrackPiece.PlatformPiece` ↔ `TrackPlatformPiece.TrackPiece`);
- `start` = first station piece in travel order (lowest StartDistance),
  `end` = last, `middle` between, `startEnd` for a one-piece station;
- `TrackPlatformPiece.Flags`: bit0 start, bit1 end, bit2 stairs left,
  bit3 stairs right. The stairs variants go where the entrance / exit
  buildings join the platform.
- **Signal lights.** `start`, `end` and `startEnd` meshes are `bsh` (the
  `middle` ones are `shs`) carrying a lamp post per station end: at +X for
  `start` (where trains come in, pieces running towards −X), at −X for `end`.
  Each post is a box with both lamps unlit (`StationLights:ftx`, own bone
  `meshlightstart_hi` / `meshlightend_hi` in Wooden and NoPlatform, in the
  deck's bone for standard). What lights one is a 5-triangle glow sprite
  (`stationMisc:ftx`, `SIAlphaUnlit`, bones `lightglow{start,end}{red,green}`)
  facing ±X; the svd's four one-frame `ban`s `<Style>{Start,End}{Green,Red}`
  move the lit colour's glow 0.05 out of the lamp and the other in, and swap
  `lightend01–04` (bones with no triangles, z 1.251 ↔ −1) — probably the
  point-light markers, alongside `lightcolourNN_rRgGbB` and
  `simplelightstartNN_<end><colour>`. Extracted as is by `--types scenery`
  (skinned glb with the four animations).

In park saves (`TrackPiece`) the travel direction **is** stored:
`Reversed` (bool), next to `StartDistance`, `StartDistanceBackwardsSpline`,
`Prev`/`Next`, `BrakeSpeed`, `HoldingTime`, `FlexiColourField`. Checked on
the 74 tracks of the 42 campaign parks (`rct3_saves/tools/track_geometry.py`):
2999/3024 joints meet under §15.4, and `Reversed` agrees with the geometric
travel direction on 1124/1140 directed pieces. The 16 others are ties
where both orientations connect. **A `.trk` is the same object stream** (rct3_saves' `Walk` reads it):
it holds the `TrackedRideInstance` (name, `TrackedRideSymbolName`,
`TrainSelection`, `NCarsPerTrain`, platform style/type, every FlexiColour
triple), the `Track`, every `TrackPiece` **with `Reversed`**, the
`TrackPlatformPiece`s with their exact sid and stair flags, and the entrance
/ exit as `SceneryItemPlacementSingle` (`StandardEntrance` /
`StandardExit` from `style\Vanilla\style`). On the 208 shipped designs
`Reversed` agrees with the geometric inference on 7503/7624 directed
pieces. The geometric inference (§15.4) is only needed when the object
stream can't be read. When several ride types share piece
names over different track sets (Corkscrew / LoopingRC / Twister…), pick
the type whose geometry connects the most joints, or read
`TrackedRideInstance.TrackedRideSymbolName`.

**Operating settings in a design** (`read_design(...)["operation"]` and each
piece's `speed`). The `TrackedRideInstance` keeps the operating panel the
design was saved with; the game seeds it from the ride's `trr` Common block
(`TrackedRideInstance_InitOperatingParameters @ 0x00a83570` in rerct3), so a
fresh ride's chain and launch speeds are the `trr` presets **clamped to the
`trr` min/max** (Splash Boats: preset 4, range 2.235–2.235 → 2.235 m/s).
Measured on the 208 shipped designs:

| field (save name) | meaning | values seen |
|---|---|---|
| `LiftChainSpeed` f32 | chain lift speed, m/s | 1.36–6.26 (4.0 on 136) |
| `LaunchSpeed` f32 | launch speed, m/s; used by `OpMode` 1 / 14 | 4.5–50 |
| `OpMode` i32 | operating mode (rerct3 `park.enum.op-mode`: 0 continuous circuit, 1 powered launch, 4 race, 11 reverse cable lift, 13 aquarium, 14 multiple powered launch, 15 lazy river…) | 0 ×180, 1 ×20 |
| `BrakeSpeed`, `BlockBrakeOffSpeed` f32 | default target of brakes / block brakes, m/s | always 4 |
| `FixedSpeed` f32 | constant speed for powered rides, < 0 none | −1, 4 (dark rides, cars), 7 (Mine Ride) |
| `MinWaitTime`, `MaxWaitTime` i32 | station waits, s; −1 = off | 10 / 120 mostly |
| `WaitForLoad` i32 | load to wait for (0 none, 4 = the default) | 0, 4 |
| `NumCircuits` i32 | laps per ride | 1 (2 and 3 once each) |
| `SyncTrains`, `LeaveIfAnotherTrainEntersStation`, `BlockSectionedMode` bool | station options | |
| `DepartureIntervalSecs` i32 | fixed departure interval, s; −2 = off | −2, 5, 10, 20 |
| `FrictionMultiplier` f32 | | always 1 |

Per piece, `TrackPiece.Speed` (int32, runtime `TrackPiece+0x194`) is the
target speed the player sets on a **brake** (the train is braked with the
ride's brake force while faster), a **block brake** (pushed with the booster
force while slower, once its block is clear) and a **booster**; −1 means the
ride default above. Shipped values: brakes 1–14 (8, 4 and 1 most often),
block brakes 1–30, boosters 5–24. On every other piece `Speed` holds
leftovers (1, 4, 8, 30…) the game never reads. `HoldingTime` (int32) is the
Vertical Drop's holding-brake wait in seconds (5 on the one shipped
`holdingbrake`); elsewhere it is leftovers too.

**Supports** are not part of the piece meshes; the game builds them at
runtime. **A track piece is an ordinary scenery item for supports**: it owns
a `SceneryItem` with its own `sid`, which goes through the scenery path
(AddConnectionSupports → `RebuildCellVisuals`). The Windows
`TrackPiece_BuildSupports` / `SupportGrid_PlaceColumns` chain, long taken for
the support builder, only records tunnel (terrain hollow) intervals.
Decoded from the Mac build with Ghidra (2026-11-09): `docs/re/rebuild_cell_visuals.md`
and `docs/re/track_build_supports.md` hold the addresses and pseudo-code;
`scripts/rct3_supports.py` implements it. Its `item_supports(..., alternatives=True)`
works out every entry of every group, each as if it were the one that stands, tagged with
its `group` and `entry`, for a caller that picks a group's entry against its own world
(the rct3-neoforge converter does: the mod's world then builds the first that can stand).

**Style.** `sid.supports` (+0x4C) names it. Its pieces are the `sid`s of
`Supports/Supports.ovl` named `<style>_*`, grouped by **sid type**: **12**
middle (`_support_1H/2H/4H/8H`, height = sid size y), **13** top, **14**
bottom, **15** bottom-extra, **16** girder, **17** cap. Each piece's first
square `+0x10` is its direction class: 1 vertical, 2–5 diagonal, 6–7
mirrored. A support piece sid flag `0x4000` (or `0x8000`) lets the style's
columns stand on other items (the wooden `ts2`); `0x8000` also drops the
one-unit gap above a track piece below.

**The item's sid decides everything** (`sid.flags`):

| flag | meaning |
|---|---|
| `0x80` | the item has supports |
| `0x400` | connection mode (the 88-byte entries, all steel sets); clear = per-cell mode (wooden `ts2`, `ts3`, `ts5`, `ts7`, water rides) |
| `0x200` | checkerboard: a column only on tiles where `(x ^ z) & 1` (the start cell of an entry). Set on one-tile orthogonal pieces, never on 45° ones: this is the "one column out of two" on straight runs |
| `0x800` | the style's cap (type 17) is put at the attach point (Wild Mouse, Junior, Mine Ride…) |
| `0x100` | other columns may stand on this item |

**Per-cell mode.** Each footprint square with flag bit 1 gets one column at
the tile centre, from its bottom up to `base + square.+4` (an integer: no
stretch). Square `+8` is the item's top there, which is where a column from
above may stand. Square bits 3–11 mark which 3×3 sub-cells are solid.

**Connection entry** (`SceneryItemTrack`, 88 B, `sid +0x34` count, `+0x38` array):

| offset | type | field |
|---|---|---|
| +0x00/+0x04 | i32 | attach (start) cell, item-local |
| +0x08/+0x0C | i32 | attach **sub-cell index** 0..2 on the tile's 3×3 grid |
| +0x10/+0x14 | f32 | attach position inside that sub-cell (0..1) |
| +0x18 | f32 | attach height above the item base (the track's underside) |
| +0x1C/+0x20 | i32 | cell the column stands on |
| +0x24/+0x28 | i32 | its sub-cell index (the column stands at the sub-cell centre) |
| +0x2C | f32 | read, never used |
| +0x30 | i32 | angled (a sloped girder to the far point); no shipped entry sets it |
| +0x34/+0x38, +0x3C/+0x40, +0x44/+0x48, +0x4C | | angled far point: cell, sub-cell, fraction, height |
| +0x50 | u32 | bit0 inverted (the girder passes over the track), bit1 no girder fallback |
| +0x54 | i32 | **alternative group** (< 32), not a direction |

A point is at `(cell + (sub + frac) / 3) · 4`; under item rotation r the
sub-cell turns as the footprint does (`r=1: (sz, 2−sx)`, `r=2: (2−sx, 2−sz)`,
`r=3: (2−sz, sx)`, fractions with `1−f`). Earlier versions of the extractor
read +0x08..+0x14 and +0x24/+0x28 as floats, so the integer indices came out as
denormals printed as 0, and the sub-cell had to be fitted from the track
geometry (77/79 columns of a live Giga park; the data agrees with that fit on
90 % of the Giga's entries, and the game uses no geometry).

**Connection mode, per entry, in array order:** an entry is skipped when its
group already has a support or the checkerboard forbids its start tile. Then:
1. **Column under the point** when the entry is not inverted, not angled, starts
   and ends in the same cell, and its attach position is within 0.2 of its
   sub-cell centre (`(fx−½)² + (fz−½)² < 0.04`): the column ends at
   `floor(h + 0.5)` and its top piece is stretched to the exact `h`; with the cap
   flag, `h` is lowered by the cap's height and the cap sits at the point. It
   needs `bottom < top − 1`.
2. Otherwise (unless bit1), a **column on the end sub-cell plus one girder**
   from the attach point to the end sub-cell centre. The column top is lowered
   by the thinnest girder's thickness (0.443 on `ts1`) unless inverted. A
   sub-cell that already holds a column in this cell is reused (girder only).
   The girder is **one stretched mesh** (no tiling): length scale
   `(|d| + 2·w) / (maxX − minX)` with w the top piece's half-width (0.365 on
   `ts1`), pitched along the line, its top on the line (bottom when inverted).
   The game picks "the first girder thicker than the span", which never
   happens: it always uses the last girder after sorting by thickness (`3L`).

**Bottom of a column** (`FUN_1009a469c`): the nearest item below in the cell,
station platforms ignored. A connection-mode item below lets the column pass
when the sub-cell is not solid in its square; otherwise the column stands on
it only if the style may (0x4000) or the item has 0x100 — **track below
blocks** (the entry falls back to its group's other entries). Standing on an
item: bottom = its `base + square.+8` (+1 above a track piece unless 0x8000).
Nothing below: `ceil(lowest terrain corner)`, unless water or another volume
is on the tile. A column taller than 228 units is rejected.

**Stacking a column** (`bottom`, `top` integers): on the ground the
bottom-extra piece (15) is preferred, on an item the bottom piece (14), each
falling back to the other; then the top piece (13) right under `top`; then the
middles, tallest first, at multiples of their own height. A piece is used only
if its direction class fits the column's (the square's `+0x10`, turned with the
item): 2–5 turn by `(d − p) mod 4`, 6–7 by `d ≠ p`, 1 only on 1. Pieces face
`rotY((turns + 2)·π·−½)` like any scenery item.

**Hanging styles** don't hang: every column stands on the ground or an item.
The inverted / Sky Swinger / Flyer sets (`ts1a`, `ts6`) use inverted entries
whose column stands beside the track with a girder over it.

**Checked in the running game** (2026-11-09, The Money Pit, Giga, every support
node read with the probe, `scripts/verify/live_supports.py`): 467 of 506
predicted column pieces sit exactly (< 0.06) on a piece the game built, 6 of 8
girders, and the game builds no column the rules don't predict. Two things the
live game fixed: a girder's node sits at the centre of its thickness (the mesh
spans −0.223..+0.223), and what lies **under the column's top** blocks it, not
only what lies under the item's base (a bush at 3 under a piece based at 2 whose
column reaches 16). Still unexplained: the game builds no column under a few
`Medcurve` / `Medslope…curve` pieces with nothing on their tiles.
In the game, `SupportLOD` is capped at 3 and supports are built around the
camera only: aim it with `Save.look_at` (rct3_saves `tools/mac/launch_supports.sh`).

`rct3_track.py layout --supports` (with `--glb-dir` for the exact shape sizes)
implements all of this; the park save provides terrain and scenery (a `.trk`
has neither: ground 0, no obstacles). Not modelled: the animal-enclosure grid
that also triggers the checkerboard, and water volumes.

⚠ **The style string is usually at offset 0 of the OVL's string table**, so
`sid +0x4C` holds a *relocated 0*. Reading it as a null pointer loses it
(every Track3 piece would have no style). A zero `char*` is null only when
its slot is not in the relocation table.

### 15.9 Putting a ride together

```bash
# once: the ride data of the whole game (≈15 s) and the symbol index
find "$ASSETS/Tracks" "$ASSETS/Cars" "$ASSETS/Style" -name '*.common.ovl' ! -name '*_data.*' |
  while read f; do ovlextract -t track -o json "$f"; done
ovlextract --build-index idx.json "$ASSETS"

python3 scripts/rct3_track.py layout json design.trk --glb-dir glb      # → design.layout.json
python3 scripts/rct3_track.py assets json design.layout.json --index idx.json \
        --assets-root "$ASSETS" --out glb      # each mesh, recoloured as the design says
python3 scripts/rct3_track.py layout json design.trk --glb-dir glb      # again: car lengths
blender -b --python scripts/blender_ride.py -- design.layout.json glb --glb ride.glb --render ride.png
```

`layout` reads the design's objects when rct3_saves is next to this
repository (`../rct3_saves/src`, or `RCT3_SAVES_SRC`): pieces in `Next`
order with their `Reversed` flag, the exact platform pieces, entrance and
exit, the selected train and car count, the colours. Without it (or with
`--infer`) it infers direction and platforms (§15.4, §15.8). `assets` takes
a track piece's mesh from the `_data` OVL next to *its* set (piece names
repeat across sets) and everything else through the index, and writes
recoloured copies to `glb/c<a>_<b>_<c>/`. Cars are spaced by their own mesh
length, and a train longer than the station overhangs the preceding pieces.
Sub-meshes with `textureFlags & 0x9000` (shadow / collision) are left out
of every export (`ShapePart::texture_flags`).

The layout lists every track piece (§15.4 transform), one platform piece per
station piece (§15.8) and a train parked nose-first at the end of the first
station, its cars sampled on the rail centre line (§15.7). All are glTF-frame
matrices for the Z-mirrored `.glb`s: `M = S·[R|t]·S` with `S = diag(1,1,−1)`.


## 16. Flat rides (`anr`) and the shared Attraction / Ride blocks

### 16.1 Attraction_V (56 B) — every stall, flat ride and tracked ride

| off | field |
|---|---|
| +0x00 | type (`typech[1] == 2` → AttractionA) |
| +0x04 / +0x08 / +0x0C | name `txt`, description `txt`, icon `gsi` (symbol refs) |
| +0x10 / +0x14 | cost / refund |
| +0x20 | base upkeep |
| +0x24 | **loop `spl`**: flat rides; the closed path around the ride |
| +0x28 / +0x2C | path count / `spl*[]`: **guest paths** from the loop to each seat, the **last one is the mechanic's** |
| +0x30 / +0x34 | flags / max build height |
| (Soaked ext) +0x38 | add-on pack (0 vanilla, 1 soaked, 2 wild) |

### 16.2 Ride_V (24 B) + Ride_Sext

`+0 attractivity, +4 seating, +8 RideOption**` (0-terminated),
`+12 min circuits, +16 max circuits, +20 entry fee`. Soaked/Wild append
`Attraction_S*` (the attraction block lives behind this pointer in S/W
layouts), attractivity, and **`RideStationLimit[]`**: `{i32 x, i32 z,
i32 height, u32 flags}` tiles where an entrance/exit may attach (flags
1 −X, 2 +Z, 4 +X, 8 −Z, 0x10 entrance, 0x20 exit). E.g. the dolphin show allows
both on the +Z side of its back row, `(0..4, 6)`, flags 0x32. Rides without
limits accept them anywhere on the footprint edge.

`RideOption {u32 type; payload}`: payload sizes by type 0:0, 1:1, 8:5,
9/12:3, others 2 words. **Type 8 is the rating block**: excitement,
intensity, nausea, the option id it applies to, a factor (BuckingBronco
base: 2.5 / 2.7 / 2.74).

### 16.3 AnimatedRide (`anr`)

Vanilla `AnimatedRide_V` = Attraction_V + Ride_V + `+80 sid ref` + 4 u32.
Soaked/Wild `AnimatedRide_SW` = `0xFFFFFFFF, 4×u32, +0x14 Ride_S*,
+0x18 sid, 2×u32, +0x24 show-item count, +0x28 {u32 animation index;
char* name}[]` (shows: `DolphinShowAnimTrick1…`). The ride's `sid`
gives the footprint (Aviator 6×6, BuckingBronco 3×3), default colours,
support style (`scaffolding`) and the svd. Its tail
(`+0xB8` individual animation count / `char*[]`, `+0xC4…+0xD0` age
alternatives / animation chunks / alternate runs / cycles per circuit)
drives the run cycle. The `anr` usually sits in the theme's `Style` OVL
while the mesh and splines are in `Style/…/Rides/<Ride>/<Ride>.ovl`.

`ovlextract -t track` writes `<anr>.flatride.json` (attraction, ride
options + ratings, station limits, sid, show items, splines). All 86 flat
rides of the Complete Edition decode with 0 errors. Paths and loop are in
the ride's local frame, which is the mesh frame (checked on the Aviator: 16
guest paths end at the 16 gondolas, the loop circles the base):

```bash
python3 scripts/rct3_track.py flatlayout out/track Aviator -o aviator.layout.json
blender -b --python scripts/blender_ride.py -- aviator.layout.json out/glb --render aviator.png
```

Tracked rides carry the same two blocks: `<ride>.ride.json` has
`attraction` and `ride_info` (Vanilla: at the start of the `trr`;
Soaked/Wild: behind the extension's `Ride_S*`).


## 17. Fonts (`fct`)

The interface fonts live in `Main` (`GUIFontTiny/Small/Medium/Large`,
`GUIFontSmallNumbers`, `LucidaConsole`) and, per screen resolution, in
`gui/<800|1024|1280>/Resolution` (`GUIFontScaledTiny/Small/Medium/Large`).
An `fct` symbol is a Win32-style face description plus a glyph cache: the
`<name>:tex` page it resolves to (§4) holds the pre-rendered glyphs, and the
glyph table says where each one is. The point size is a separate float
symbol declared next to it, `GUIFontSizeScaledTiny:flt` = 9.0 (Small 12,
Medium 16, Large 21).

Font, 112 bytes at the data pointer (every "ptr" is an internal offset):

```
+0x00 f32 line_gap   in em: 1 or 2 px over the size (1/9, 1/6, 2/16, 1/21)
+0x04 f32 ascent     in em  (9/9, 12/12, 15/16, 19/21)
+0x08 f32 descent    in em  (2/9, 2/12, 3/16, 5/21)
+0x0C u32 0
+0x10 u32 glyph_count    4210 for the Unicode fonts, 59 / 128 for the ASCII ones
+0x14 ptr texture        relocation slot, resolved by symbol to "<name>:tex"
+0x18 u16 has_codes      1: a code-point table follows (sparse font)
                         0: glyph i is code point i (GUIFontSmallNumbers, LucidaConsole)
+0x1A u16 1
+0x1C u32 1
+0x20 u32 0
+0x24 u32 weight         Win32 FW_NORMAL 400 / FW_BOLD 700
+0x28 char[32] face      "Tahoma", "Verdana", "Arial", "Lucida Console" (NUL-terminated,
                         the bytes after the NUL are leftovers)
+0x48 u32[8]             runtime scratch — pointers from the authoring machine, ignore
+0x68 ptr glyphs         → glyph_count × Glyph (36 B)
+0x6C ptr codes          → glyph_count × u16 code point, ascending (last = 0 pad)
```

Glyph, 36 bytes:

```
+0x00 f32 advance    pen advance in em (× size = pixels)
+0x04 f32 spacing    one pixel in em for most rendered glyphs, 0 otherwise (role unconfirmed)
+0x08 f32 0
+0x0C f32 u0  +0x10 f32 v0  +0x14 f32 u1  +0x18 f32 v1
                     rectangle in the page, 0..1, in the page's STORED orientation
+0x1C f32 offset_x   bitmap offset from the pen, pixels, to the right
+0x20 f32 offset_y   bitmap offset from the baseline to the bitmap's bottom edge,
                     pixels, UPWARD (negative for descenders: g/j/p/q = -4 in Large;
                     +10 for the quotes)
```

A page only caches part of the font: the four 4210-glyph fonts pre-render the
same 163 glyphs (Latin-1 letters, digits, punctuation); every other code point
has `u0 = u1 = 0` and is rasterised by the game from the system font at run
time. Font pages are DXT3 256², white with the glyphs in the alpha plane, and
are stored bottom-up (§4.3): a glyph's upright pixel rect is
`[u0·w, (1 − v1)·h, u1·w, (1 − v0)·h]` on the vertically flipped page. Drawing
a string is then `x += advance × size` per glyph, each bitmap placed at
`(pen_x + offset_x, baseline − height − offset_y)` — verified by recomposing
"Hello RCT3! gjpq" from `GUIFontScaledLarge` (Arial bold 21 px).

`--types font` writes `<name>.font.json` (face, weight, metrics, every glyph
with its code, advance, offsets, uv and — when the page decodes — upright
`rect`) and `<name>.tga`, the page flipped upright. 18 fonts in the install,
all decode (`LucidaConsole`'s page `LucidaConsole:tex` is declared by no OVL,
so its JSON keeps uv only).


## 18. Text (`txt`)

A `txt` symbol is a NUL-terminated **UTF-16LE** string at its data pointer,
nothing else. The three language files `gui/English`, `gui/French` and
`gui/German` each hold the 12 388 strings of the interface under the same
symbol names (`AOP_Name_0`, `ResearchName_…`, `TooltipText_…`); `Main` has
1 523 more and each `Style` / content pack a few (names and descriptions of
their shops, resolved by `--types scenery` for the manifests, §13.4). In
`Main` the `txt` loader references also own two extra-data records each
(§4.1.1), unused here.

`--types text` writes one `<ovl>.txt.json` per pair, `{"Symbol": "string"}`
in UTF-8, sorted by symbol: 38 726 strings over the 16 pairs that carry any.

### 18.1 Writing a language file (verified in the game)

A language pair is the simplest OVL to re-encode, and `scripts/ovl_text_pack.py`
does it from a template pair and a JSON `{"Symbol": "text"}` (what
`--types text` writes). What the common side of `gui/<Language>` holds, in
order, and what the writer does with it:

| Part | Content | Writer |
|---|---|---|
| Header, loaders, v5 loader extra | `FGRK`, header v2, OVL v5, one `txt` loader; per-loader `{0, symbol count}` | copied |
| Chunk 1 | block 0 = the symbol-name string table (`Name:txt\0`, sorted by hash); blocks 1… = the UTF-16LE texts, ~16 KB per block | names copied, texts re-packed |
| Chunk 3 | block 0 = symbol table (`{name ptr, data ptr, u16 1, u16 0xFFFF, hash}` × n); block 1 = loader references (`{0, data ptr, hasextradata, symbol ptr, 0}` × n) | pointers recomputed |
| v5 extra struct (`temp` = 64 B) | the loader references that own extra data: `{u32 index, u16 0xFFFF, u16 1, u32 1, u16 1}` × 4 | copied |
| Relocations | the 4 pointer fields of every symbol / loader reference, ascending | regenerated |
| Extra data | one `{4, 1}` record for 4 of the `txt` loaders | copied |
| Tail | `FilesAndLines`: per block, `{start, end, ?}` then one `{source path, line, offset}` per string — the authoring source map (`c:\coding\safari\resource\gui\<Language>\General.txt`, 3.7 MB) | **dropped** |

The unique side only declares the loader and three empty blocks; its tail
repeats the common's total size three times. The writer emits it without the
tail. Keeping the symbol set of the template means the symbol hashes travel
unchanged, so no hash function is needed; new symbols are refused.
(The hash is djb2 over the lower-cased `name:tag` — see
[LIBOVLNG_WRITER.md](LIBOVLNG_WRITER.md) §1 — so new symbols can now be added.)
The C++ writer `ovlwrite text strings.json out` (core `TextOvl`, on top of
`OvlBuilder`) builds a pair from scratch, without a template; see
LIBOVLNG_WRITER.md §5.

Verified on the Mac build (`RCT3_Exe` 3.3.4): the game loads a re-packed
`French.common.ovl` whose strings changed length — the main menu shows the new
texts, wrapped and ellipsised by the GUI — and a pair truncated right after
its extra data (no tail) loads too, so the source map is not read. The
writer reproduces `German` and `French` byte for byte (minus the tail) and
`English` up to the split of the text blocks (one of its blocks closes at
15 586 B for no visible reason).

What the game does **not** do: discover new languages. The Language list of
the options dialog is fixed to English / French / German on this build (names
from `Main`'s `Language_<Stem>:txt`), `Language "Spanish"` in `Options.txt`
falls back to English, and a `gui/Spanish.common.ovl` is never opened. The
gate is one function, `IsSupportedLanguage` (`0x1005bf5c7` in the Mac
executable: four string compares, "Japanese" being the fourth), called by the
loader and by the options dialog, which otherwise lists every
`gui/*.common.ovl` under the `Language_<Stem>:txt` label of `Main`.
`rct3_saves/tools/mac/patch_language_list.py` makes it return true (3 bytes,
ad-hoc re-signed): verified, "Español" then appears in the list and the
interface switches live to a `gui/Spanish` pair written by
`ovl_text_pack.py`. Without the patch, a new language replaces one of the
three slots — e.g. ship Spanish as `gui/French.common.ovl`.

## 19. Terrain types (`ter`)

The land tools' surfaces are `ter` symbols in two pairs: `terrain/RCT3/Terrain_RCT3`
(`Terrain_00`…`25` and `Cliff_00`…`05`) and `terrain/CT/Terrain_CT` (`Terrain_26`…`31`,
Soaked/Wild, `addon` 1). Each points to one `tex` of the same pair (`Terrain_NN`,
`TerrainCliffN`), a 256×256 picture that `--types texture` decodes like any other (§4):
all 38 do. The struct is 60 bytes, relocate-only (layout verified against the game's
consumer in rerct3/research/re/loader-structs.md):

| Off | Field | Notes |
|---|---|---|
| +0x00 | u32 version | 1 |
| +0x04 | u32 | 0 |
| +0x08 | u32 addon | 0 vanilla, 1 Soaked/Wild |
| +0x0C | u32 number | index in its list (grounds 0–31, cliffs 0–5) |
| +0x10 | u32 type | 0 ground, 1 cliff, 2 ground that blends with its neighbours |
| +0x14 | `tex*` | resolved by symbol |
| +0x18 | `char*` description | a `txt` key: `Land_Paint_Base0Help`… |
| +0x1C | `char*` icon | a `gsi` name: `Land_Paint_Base0`… |
| +0x20 | u32 colour_simple | BGRA, the untextured-terrain option's colour; cliffs `FFFF007F` |
| +0x24 | u32 colour_map | BGRA, the map's colour |
| +0x28 | f32 inv_width | U = world X × inv_width: **0.1 = a texture covers 10 m** (2.5 tiles); 0.25 = 4 m, one tile (CT paving and bordered concrete) |
| +0x2C | f32 inv_height | same along Z |
| +0x30… | 3 × f32 | read by nothing in the game |

The order of the `Terrain_NN` symbols is **not** the order of the interface keys:
19–22 are `Snow0`…`3`, 23–25 `Concrete0`…`2`; read `description` rather than counting.
The six rock grounds (13–18) take the six cliffs' icons and textures.

`--types terrain` writes the pair's records to one `<ovl>.terrain.json`.


## 20. People (`Characters/`): parts, part groups, heads (`--types people`)

Guests, staff, VIPs and entertainers are assembled from parts. Files under
`Characters/<group>/` (`AF` adult female, `AM` adult male, `TF`/`TM` teens,
`CF`/`CM` children, plus one folder per staff type and VIP):

| File | Holds |
|---|---|
| `<id>_Body_Stub`, `_Legs_Stub`, `_Head_Stub`, `_SwimSuit…_Stub` | the `ppg` part group |
| `<id>_Body_Main`, `_Legs_Main`, `_SwimSuit…_Main` | `prt` parts + their `mms` meshes + the clothing `tex` |
| `<id>_Head_Main` | `phd` head, `ssk` detailed heads (2 LODs × 6 expressions), `mms` low-detail head |
| `<id>_Skin` | the `tex` used by skin parts and the head (skin tone + hair) |
| `<id>_Bikini`, `_OnePiece`, `_SwimSuit`, `_Inflatable` | swimsuit / rubber ring `tex` |
| `Shared/` | attachments (hats, held items) |

### 20.1 Colours: 16 variants per texture

Every guest texture is a 4 × 4 grid of 64 px cells (256 × 256; rings 512 × 512).
A part's UVs cover cell 0, the **top-left cell** of the upright texture (checked
by rendering: AF06's cell 0 puts the green T-shirt's logo upright on the chest).
At draw time the game adds `((p & 3) × 0.25, ((p >> 2) & 3) × −0.25)` to the UVs
(`PeoplePart_TransformAndCopyBatch`, rerct3 `peeps.md`), `p` being one of the
person's palette bytes (§20.2) — so `p` reads the grid left to right, top to
bottom. A `_Skin` texture cell bundles skin tone, hair colour and the bare-skin
parts of that head's outfit; skin tone and hair are not separate choices.

### 20.2 `prt` — PeoplePart (20 B)

```
+0x00 u32   1 (never read)
+0x04 u32   0 on disk: runtime slot (the loader stores a cache handle there)
+0x08 tex*  texture
+0x0C mms*  mesh
+0x10 u32   palette: 0 skin, 1 upper clothes, 2 lower clothes,
                     3 upper swimsuit, 4 lower swimsuit
```

The palette values match the person's save fields `SkinPalette`,
`UpperClothingPalette`, `LowerClothingPalette`, `UpperSwimSuitPalette`,
`LowerSwimSuitPalette` (Person +0xC0…+0xC4). A `Body` part has three meshes,
each its own `prt`: `Body` (clothes), `SkinBody` (bare arms, skin texture) and
`OptionalBody` (an extra piece the person may or may not show).

Three objects of `Characters/CF/CF01/SS_SkinLegs_CF01` use another order
(tex* at +0x00, 0, 0, u32 1, mms* at +0x10, no palette); they duplicate well
formed parts of `CF01_SwimSuitLegs1/2_Main` and are flagged `stray_layout`.

### 20.3 `ppg` — PeoplePartGroup (0x54 B)

```
+0x00 u32     1 (2 on Wild! groups; never read)
+0x04 u32     expansion: 0 base, 1 Soaked!, 2 Wild!
+0x08 u32     1 on most swimsuit groups and the lifeguard (never read)
+0x0C u8      person_type: 0 child, 1 teen, 2 adult, 3 VIP, 4 handyman,
              5 mechanic, 6 security guard, 7 entertainer, 8 vendor,
              9 animal keeper, 10 paramedic, 11 park inspector, 12 lifeguard
+0x0D u8      sub_type: 0 male, 1 female
+0x0E u8      slot: 0 head, 1 body, 2 legs, 3 swimsuit body, 4 swimsuit legs
+0x0F u8      optional_part_count
+0x10 u32     1 on the AF01, AM05, CM05, TM05 heads only (never read)
+0x14 f32*    optional_part_chances[optional_part_count]: at setup
              rand() < chance[i] sets bit i of the optional-part mask
+0x18 f32     weight: weighted random pick among the groups registered for
              one (person_type, sub_type, slot) (`FUN_1006b893a` registers,
              `FUN_1006b7ffe` picks)
+0x1C 3 × LOD record   clothes: high, medium, low
+0x34 phd*    head (heads only)
+0x38 3 × LOD record   swimsuit
+0x50 phd*    swimsuit head (heads only)

LOD record (8 B): u8 count, u8 optional_count, u16 0, prt** (count + optional_count)
```

A body's high LOD lists `Body_L1, SkinBody_L1, AttachmentL, AttachmentR` and
the optional `OptionalBody_L1`. Head groups only fill their low LOD (the `mms`
head + hat attachment); the high and medium heads come from the `phd`.

### 20.4 `phd` — PeopleHead (0xC0 B)

```
+0x00 f32[16]  hat attachment (point 2), applied after the Bip01 Head bone
+0x40 f32[16]  face attachment (point 3: sunglasses)
+0x80 u32 1 (never read), +0x84 u32 0 on disk: runtime cache slot
+0x88 tex*     skin texture
+0x8C sal*     head animations
+0x90 ssk*[6]  LOD 1: neutral, Angry, Happy, Sad, Scared, Sick
+0xA8 ssk*[6]  LOD 2: same order
```

### 20.5 `ssk` — SimpleSkin (28 B)

```
+0x00 u32      bone count
+0x04 char**   bone names ("Bip01 Neck", "Bip01 Head", "Bip01 Ponytail1"…)
+0x08 u32*     vertex count per bone (vertices are in bone order)
+0x0C u32      triangle count
+0x10 u16*     indices (3 per triangle)
+0x14 u32      vertex count
+0x18 f32*     vertices: 8 floats (position, normal, U, V)
```

Rigid skinning, one bone per vertex; positions are in their bone's space.
The pose comes from the head's `sal` / `san` (§21.4): channel 0 is Bip01 Neck,
channel 1 Bip01 Head, and the head's transform is [R|t] of the neck times
[R|t] of the head (the head sits at (0.073, 0, 0) in the neck frame).

### 20.6 Output

`--types people` writes, per pair:

- `<ovl>.people.json`: every `prt`, `ppg`, `phd` (fields above; the three
  constants nobody reads keep their `unk` names), every `mms` (counts and its animations: name, keyframes, length) and
  every `ssk` (bones and vertex runs);
- one `<mesh>.glb` per `mms` (pose `IdleStandard` keyframe 0) and per `ssk`
  (bone space), with the part's texture embedded when it lives in the same
  pair (clothes do; `_Skin` textures are separate OVLs). Positions are mirrored
  on Z like `--types scenery` (§14); UVs address cell 0.


## 21. Data-only types (`--types data`)

Every loader type that is pure game data — no pixels, no sound, no mesh that
another exporter already turns into a standard file — has a declarative
layout in `extractors/src/DataExtractor_*.cpp` (engine:
`extractors/include/ovl/extract/Layout.hpp`). `--types data` writes one
`<ovl>.data.json` per pair, `{"<tag>": [{"symbol": …, fields…}, …]}`: every
field of every struct, pointers followed (strings, arrays, sub-structs),
symbol references as `"name:tag"` (a reference with an empty name, `":gsi"`,
is a real null reference in the game data), keyframes and vertices as number
arrays. Over the whole install: 719 files, 215 MB, 13 s.

### 21.1 How a layout is proven

`ovl_layout_check <Assets> [tag…]` decodes every object of every registered
tag and reports, per tag, objects with issues and the fields still named
`unk*`. An object passes when

- every byte of every struct instance (top level and every sub-struct
  reached) is claimed by exactly one field;
- every relocated pointer slot and every symbol-reference slot inside it is
  consumed by a field that follows it — no pointer is read as a number;
- every field declared zero is 0.

All 27 tags pass on every object of the Mac Complete Edition (12 819
objects). `unk_*` names are left only on values for which no reader was
found in the Mac game (`decompile/ghidra/decompiled.c`); each is listed in
its section with its values. Field names come from the game's consumers
first, libOVLng (`include/*.h`, `Manager*.cpp`) and rerct3 second, value
statistics last (`ovl_survey <Assets> <tag…>` prints, per struct word, how
often it is a pointer, a reference, zero, a float, and its common values).

The Mac build is 64-bit but keeps the 32-bit on-disk structs: each pointer
slot is a 32-bit handle passed through `FUN_1001f2550`, so the offsets its
consumers read are file offsets.

### 21.2 Paths, queues, pools, changing rooms, enclosures

Layouts: `extractors/src/DataExtractor_Paths.cpp`. All seven loaders are no-op relocators
(rerct3 loader-structs.md / animals.md; Mac TypeHandler ctors e.g. `FUN_100654cba` "PoolType"/"pty"),
so the on-disk struct is what the consumer reads. Mac consumers resolve every 32-bit relocated slot
with `FUN_1001f2550`; offsets below are on-disk offsets as read by those consumers.

#### Shared: `Research` (12 B, array behind `research`)

| Off | Type | Field | Notes |
|---|---|---|---|
| +0x00 | ref `rcg` | category | |
| +0x04 | i32 | runtime_slot | −1 on disk; overwritten at load by `FUN_1005729ce` (category registration) |
| +0x08 | f32 | min_progress | `FUN_100572a52`: available when `min_progress <= category+0x15C` |

Same record as libOVLng `Research {ResearchCategory*, unk1, unk2}` — other tags with
`ResearchCategoryCount/ResearchCategory*` (stalls, attractions…) should use it too.

#### `ptd` — PathType (10 objects, 10 pairs)

336 B; 604 B (`PathType2`) when `flags & 0x01000000` (2 objects: `0x01000001`, `0x01000003`).

| Off | Type | Field | Notes |
|---|---|---|---|
| +0x000 | hex32 | flags | 0x1 always, 0x2 underwater, 0x01000000 extended |
| +0x004 | char* | internal_name | |
| +0x008 | ref `txt` | name | |
| +0x00C | ref `gsi` | icon | |
| +0x010 / +0x014 | char* | texture_a / texture_b | `tex` names in `<name>_Texture` |
| +0x018 | char*[19][4] | flat … slope_mid | 19 shape groups (path.h order), 4 variants each; JSON key per group |
| +0x148 | u32 | research_count | |
| +0x14C | Research* | research | |
| ext +0x150 | zero | | libOVLng `unk1`, 0 |
| ext +0x154 | u32 | unk_154 | libOVLng `unk2`, 1 in both objects; not read by the path consumer |
| ext +0x158 | char*[16][4] | flat_fc … slope_mid_tc | closed-end (tunnel) variants |
| ext +0x258 | char* | paving | |

Evidence: path.h; loader-structs.md "PathType (ptd) & QueueType (qtd) — VERIFIED"; Mac path-type resolver
(`"Missing scenery pieces for path type "`, decompiled.c ~1067640): 19×4 pieces from word 6, 16×4 from
word 0x56 and paving at word 0x96 only when `flags >= 0x01000000`, research at +0x148/+0x14C.

#### `qtd` — QueueType (5 objects, 5 pairs)

52 B: `+0x00 internal_name char*`, `+0x04 name txt`, `+0x08 icon gsi`, `+0x0C texture` (`ftx` ref,
`Ground<name>`), `+0x10..+0x28 straight, turn_l, turn_r, slope_up, slope_down, slope_straight_1,
slope_straight_2` (char*), `+0x2C research_count`, `+0x30 research` (Research*). Source path.h / QueueSet1.

#### `pty` — PoolType (1 object, 1 pair: `Pool/Default/Default_Stub`)

600 B (block remaining 14056 → next `sid` at 13456). Desc pointer cached at PoolTypeDatabaseEntry+0x168.

| Off | Type | Field | Notes |
|---|---|---|---|
| +0x000 | char* | internal_name | "Default" |
| +0x004 | ref `txt` | name | |
| +0x008 | ref `gsi` | icon | |
| +0x00C | char*[89] | pieces | scenery `sid` names (Base, Corner3Top…, Jacuzzi, Railing); `FUN_100374e50` loop `< 0x59`, "Missing scenery pieces for pool type " |
| +0x170 | u32 | research_count | `FUN_1005729ce(entry+0x70, +0x170, +0x174)` |
| +0x174 | Research* | research | |
| +0x178 / +0x17C | i32 | type0_cost_base / _per_depth | `FUN_10096f6a8` (cost): SegmentType 0 → `base + per_depth × Depth` |
| +0x180 / +0x184 | i32 | type0_refund_base / _per_depth | `FUN_10096f90a` (refund), same formula |
| +0x188 / +0x18C | i32 | type1to4_cost_base / _per_depth | SegmentType 1..4 |
| +0x190 / +0x194 | i32 | type1to4_refund_base / _per_depth | |
| +0x198 / +0x19C | i32 | type5to9_cost / _refund | SegmentType 5..9, flat |
| +0x1A0 / +0x1A4 | i32 | type4_cost_extra / type4_refund_extra | added for SegmentType 4 |
| +0x1A8 / +0x1AC | i32 | type2_cost_per_height / refund | × SpecialHeight for SegmentType 2 |
| +0x1B0 / +0x1B4 | i32 | type3_cost_per_height / refund | × 3·(SpecialHeight/3) for SegmentType 3 |
| +0x1B8 | f32 | unk_1B8 | 0.1; no reader found |
| +0x1BC | f32 | water_surface_offset | 0.2; added to SurfaceHeight × unit in segment geometry |
| +0x1C0 | f32 | unk_1C0 | 1.0; no reader found |
| +0x1C4 | f32 | edge_inset | 0.9; per-side inset in segment geometry |
| +0x1C8 | ref `spl`[6][6] | swim_splines | per tile shape {corner3, edge, corner1, corner2, end, full} × {straight, 5 curve variants}; grouping from the spline names |

SegmentType / Depth / SpecialHeight are PoolSegment persistent fields (+0x15C / +0x174 / +0x160).
The segment-type enum names are not recovered, hence `typeN_*`.

#### `chg` — ChangingRoom (4 objects, 1 pair: `Style/Vanilla/CapeTyphoon/Style`)

12 B `{+0x00 Attraction_S* attraction, +0x04 scenery sid, +0x08 spline spl}` (stall.h / ManagerCHG),
Attraction_S (64 B) right behind it (76 B per room). Attraction type 0x212.

Attraction block (same as the scenery group's): `+0x00 type`, `+0x04 name txt`, `+0x08 description txt`,
`+0x0C icon gsi`, `+0x10 cost i32`, `+0x14 refund i32`, `+0x18/+0x1C zero`, `+0x20 base_upkeep`
(4960), `+0x24 spline`, `+0x28 path_count`, `+0x2C paths` (spl ref array), `+0x30 flags`,
`+0x34 max_height` (−1), `+0x38 addon_pack`, `+0x3C unk_3C` (libOVLng `unk12`, 0 here),
Wild only `+0x40 ui_deactivation`.

#### `vwg` — ViewingGallery (1 object, 1 pair: `Enclosures/Fences/Fences`)

4 B `{Attraction_W* attraction}` + Attraction_W (68 B, type 0x314, addon 2) behind it.

#### `enc` — EnclosureType (3 objects, 3 pairs: Wood / Chain / Electric `_Stub`)

92 B (block 2276 → next `sid` at 2184).

| Off | Type | Field | Notes |
|---|---|---|---|
| +0x00 | char* | internal_name | |
| +0x04 | ref `txt` | name | |
| +0x08 | ref `gsi` | icon | |
| +0x0C..+0x2C | char* ×9 | full, end, corner, edge, corner_inner, gate, view_left, view_mid, view_right | scenery `sid` names; resolve loop `< 9`, "Missing scenery pieces for Enclosure type " |
| +0x30 / +0x34 | i32 | fence_cost / fence_refund | 500/−400, 1000/−800, 2000/−1700; `n·+0x30 + m·+0x34` (~1251049) |
| +0x38 / +0x3C | i32 | gallery_cost / gallery_refund | 4000/−3500 …; × count in ViewingGalleryInstance (~1507011) |
| +0x40..+0x4C | ref `spl` ×4 | spline_right_top, spline_right_bottom, spline_left_top, spline_left_bottom | Wood has null bottoms |
| +0x50 | f32 | strength | 0 / 2 / 3; consumers pick the weakest fence with `strength >= required` |
| +0x54 | f32 | animal_offset | 0.2 / 0.14 / 0.5; added to an animal's stand position next to the fence |
| +0x58 | f32 | height | 1 / 2.2 / 4; animal escape test compares it to a threshold |

Corrects loader-structs.md (88 B, 8 pieces): 9 pieces, costs start at +0x30.

#### `asd` — AnimalShelterDesc (6 objects, 1 pair: `Enclosures/Shelters/Shelters`)

84 B + its eat/drink/sleep spline arrays right behind it (116 or 120 B per shelter); the proxy tile
lists sit in another block. Desc cached at AnimalShelter+0xB8 (`FUN_100a5be04`).

| Off | Type | Field | Notes |
|---|---|---|---|
| +0x00 | ref `sid` | shelter | |
| +0x04 / +0x08 / +0x0C | i32 | hut_x / hut_z / hut_rotation | placement args of the hut (`+0x10`) |
| +0x10 | ref `sid` | hut | WardensShed |
| +0x14 / +0x18 | f32 / i32 | food_trough_offset / food_trough_slot | Trough vtable+0x40 init, trough 0 |
| +0x1C / +0x20 | f32 / i32 | drink_trough_offset / drink_trough_slot | trough 1 |
| +0x24 / +0x28 | u32 / spl ref* | eat_spline_count / eat_splines | `FUN_100a153ac` spline kind 2 |
| +0x2C / +0x30 | | drink_spline_count / drink_splines | kind 3 |
| +0x34 / +0x38 | | sleep_spline_count / sleep_splines | kind 4 |
| +0x3C | ref `spl` | trough_transition_spline | kind 5 |
| +0x40 | u32 | food_type | `FUN_100475724`: 0 foodmeat, 1 foodhay, 2 foodleaves |
| +0x44 / +0x48 | u32 / {i32 x, i32 z}* | proxy_tile_count / proxy_tiles | one `invisibleproxy` sid placed per tile around the shelter |
| +0x4C | ref `spl` | male_spline | kind 7 |
| +0x50 | ref `spl` | female_spline | kind 8 |

The exact meaning of the trough offset/slot pair (distance along / edge index?) and of hut_rotation
is inferred from the call shapes, not traced into the callee.


### 21.3 Carried items, research, particles, wild animals, entertainers

Layouts are in `extractors/src/DataExtractor_Items.cpp` (`register_items`). `ovl_layout_check` reports 0 issues for all
7 tags (362 objects). Every loader here is a relocate-only no-op, so the struct on disk is the struct the consumer
reads. "Mac FUN_x" means `decompile/ghidra/decompiled.c`. All pointers in the Mac binary are 32-bit OVL handles,
resolved with `FUN_1001f2550`.

#### `cid` — CarriedItem (179 objects, 4 pairs)

156 B (0x9C). Structs sit back to back. Each one is followed by its own `extras` array (4 × `extra_count` B), so the
stride is 156 + 4·n. Source: libOVLng `carrieditems.h` / `ManagerCID.cpp`. The price fields were named from the data.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | unk_00 | 1 in every object. libOVLng: "always 1". No reader found |
| 0x04 | u32 | addon_pack | 0 vanilla, 1 Soaked!, 2 Wild! |
| 0x08 | u32 | unk_08 | 0. 2 for the Donut and Fries size families (MoreCarriedItems). No reader found |
| 0x0C | Ref txt | name | |
| 0x10 | Ref txt | plural_name | |
| 0x14 | Ref gsi | icon | 40×40, `:gsi` = none |
| 0x18 | Ref shs | shape | null (0) for hidden and billboard items |
| 0x1C | Ref shs | shape_alt | always equal to `shape` |
| 0x20 | f32 | lod_distance | 0 hidden, 40 shape items, 80 umbrellas, 200 billboards, 500 open umbrella |
| 0x24 | Ref cid | used_version | empty wrapper / open umbrella, `:cid` = none |
| 0x28 | hex | flags | 1 food, 2 drink, 4 used, 8 map, 0x10 voucher, 0x20 open umbrella, 0x40 umbrella, 0x80 balloon, 0x200 souvenir, 0x1000 kiddie, 0x2000 camera, 0x8000 camcorder, 0x10000 sun block, 0x20000 inflatable, 0x80000 swimsuit |
| 0x2C | i32 | age_class | -1. Soaked! wearables: 3 kids, 12 teens, 48 adults |
| 0x30 | u32 | carry_type | 1 not visible, 2 hand, 4 body, 6 two hands, 8 head, 16 sunglasses, 36 swimsuit |
| 0x34 | f32 | hunger | |
| 0x38 | f32 | thirst | |
| 0x3C | f32 | light_protection_factor | sun blockers 4 / 15 / 50 |
| 0x40 | u32 | cost | stall purchase cost, in 1/100 currency. Smallest of the four price words (libOVLng unk17) |
| 0x44 | u32 | value | price guests accept in normal weather (unk18) |
| 0x48 | u32 | value_wet | Umbrella 450 vs 250, coffee and hot chocolate > value, lemonade and ice cream < value (unk19) |
| 0x4C | u32 | value_hot | lemonade and ice cream > value, coffee < value (unk20) |
| 0x50 | f32 | litter_rate | 0. 0.01 for litter items (empty cups, wrappers) |
| 0x54 | i32 | litter_kind | -1. 1 wrapper / stick, 2 box / bottle |
| 0x58 | u32 | default_colour | flexi colour index: 0, 7 for Balloon |
| 0x5C | u32 | extra_count | |
| 0x60 | → Ref[n] ced | extras | condiments, written right after the struct |
| 0x64..0x6C | zero | | libOVLng unk26..28. 0 everywhere |
| 0x70 | u32 | shaped | 1, 0 for billboard items |
| 0x74 | Ref ftx | billboard_texture | null unless billboard |
| 0x78 | f32 | billboard_scale_x | |
| 0x7C | f32 | billboard_scale_y | |
| 0x80 | u32 | unk_80 | 0. 16 inflatables + swimsuits, 32 sunglasses (libOVLng unk33). No reader found |
| 0x84..0x90 | char* ×4 | male_morph_body, male_morph_legs, female_morph_body, female_morph_legs | Soaked! wearables, "" otherwise |
| 0x94 | u32 | unk_94 | 0. 1 for SwimSuitV1 (libOVLng unk38) |
| 0x98 | char* | texture_option | "Inflatable", "DefaultFlexiColours", "SwimSuitV1/2", "" |

Evidence for the price words: across 179 items, the four words keep the same order of size, rising with item size
(Small … XL). The two weather words move in opposite directions for hot drinks and for cold drinks or ice cream, and
for the umbrella. The consumer was not traced: `CarriedItem` is read in too many peep functions to single one out.

#### `ced` — CarriedItemExtra (41 objects, 1 pair)

28 B. Source: libOVLng `struct CarriedItemExtra`.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | zero | | |
| 0x04 | Ref txt | name | |
| 0x08 | Ref gsi | icon | 20×20 |
| 0x0C | u32 | unk_0C | 0, 4 for Mustard only |
| 0x10 | f32 | hunger | 0 … 0.3 |
| 0x14 | f32 | thirst | -0.3 … 0.3 |
| 0x18 | f32 | unk_18 | 0, 0.3 for Parmesan, Chilli, ChilliSauce, Marshmallow (libOVLng guesses nausea) |

#### `rcg` — ResearchCategory (58 objects, 1 pair)

40 B (`ResearchCategoryDesc`). Consumers: Mac FUN_10053c522 creates the ResearchCategoryInstance. It copies
+0x0C → CurrentLevel and +0x10 → DevelopCost, and sets IsHidden = (+0 == 2). FUN_10053c61c hands the count and pointer at
+0x14 and +0x18 to FUN_1005729ce, and writes the instance pointer back to +0x1C. FUN_100572a52 unlocks a category
when each dependency's CurrentLevel ≥ the dependency's +8 float. The name at +4 is used for the duplicate-name check
(rerct3 inventions.md).

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | initial_state | 2 = hidden. 0 / 1 visible |
| 0x04 | char* | name | system name |
| 0x08 | Ref txt | display_name | |
| 0x0C | f32 | initial_level | percent researched at start (0, 10, 20, 25, 50) |
| 0x10 | f32 | develop_cost | 10 or 1 |
| 0x14 | u32 | dependency_count | 0..2 |
| 0x18 | → ResearchDependency[n] | dependencies | |
| 0x1C | zero | | runtime: ResearchCategoryInstance |
| 0x20, 0x24 | zero | | |

ResearchDependency, 12 B: +0 `category` Ref rcg; +4 `runtime_handle` i32, -1 on disk, overwritten by
FUN_100890de6; +8 `required_level` f32, percent.

#### `psi` — ParticleSkin (41 objects, 1 pair)

28 B. Sources: rerct3 particles.md, OVLExtractor-2 `SFStructs.h` ParticleSkin / SpriteCoords.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | sprite_count | 1..16 |
| 0x04 | → (→SpriteCoords)[n] | sprites | an array of pointers, each to {f32 left, top, right, bottom} (atlas UV) |
| 0x08 | Ref tex | texture | |
| 0x0C | u32 | type | 0 normal, 1 special colour handling |
| 0x10 | u32 | mode | 0 animated, 1 static |
| 0x14 | f32 | animation_speed | 0.125 … 2.0 |
| 0x18 | zero | | |

#### `wai` — WildAnimalItem (7 objects, 1 pair)

92 B. Source: libOVLng `wildanimalitem.h`. The consumer (WildAnimalItemManager, Mac FUN_1007877c8 …) was not traced
down to field reads.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | zero | | |
| 0x04, 0x08, 0x0C | u32 | unk_04, unk_08, unk_0C | 2 in every object |
| 0x10 | char* | short_name | |
| 0x14 | Ref shs | shape | |
| 0x18 | f32 | lod_distance | 200 |
| 0x1C | i32 | cost | |
| 0x20 | i32 | refund | positive = money back |
| 0x24 | f32 | wear_rate | reciprocal durability: 0.2 food, 0.1 ball / box, 0.05 dummy / tyre, 0.033 rock |
| 0x28 | u32 | unk_28 | 0 ball / rock, 1 box / dummy, 2 iced meat / mango / tyre |
| 0x2C..0x34 | f32 ×3 | size_x, size_y, size_z | |
| 0x38 | Ref gsi | icon | |
| 0x3C | Ref txt | name | |
| 0x40..0x4C | f32 ×4 | unk_40..unk_4C | 0.7 … 1.5, mostly 1 |
| 0x50 | hex | animal_flags | 1 plant eaters, 2 meat eaters, 4 big animals (libOVLng) |
| 0x54 | Ref txt | description | |
| 0x58 | f32 | weight | 0.2 … 1.0 (libOVLng: "guess") |

#### `was` — WildAnimalSpecies (22 objects, 2 pairs)

There is a 64 B head. Then four 0x58 B variants, each followed by its `sound_count` × 44 B sound table (stride
88 + 44·n). The genetics (24 B) and stats (36 B) blocks sit elsewhere in the pair. The head labels come from
OVLExtractor-2 Form1.h. Stats come from rerct3 animals.md (WildAnimalSpeciesStats_FormatStatValueByIndex,
byte-exact). Genetics use the index order of Mac FUN_100aa23bc / FUN_100aa24fc (WildAnimalGeneticStat_* keys over
the animal's runtime copy at +0x178). Variant sizes come from Mac FUN_1002fb8b2, which builds a box of ±size around
the animal. Sounds come from Mac FUN_1002fcc72 (picks by `event` and `looping`) and FUN_1002fc092 (plays an
"AnimalSound").

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | zero | | |
| 0x04 | char* | name | "lion" |
| 0x08 | Ref txt | display_name | |
| 0x0C | Ref txt | description | |
| 0x10 | Ref gsi | icon | |
| 0x14 | char* | data_path | "WildAnimals\lion\lion_data" |
| 0x18..0x24 | → Variant ×4 | adult_male, adult_female, baby_male, baby_female | |
| 0x28 | → Genetics | genetics | |
| 0x2C | → Stats | stats | |
| 0x30 | char* | shelter | AnimalShelterDesc name ("bigcatshelter"). Mac FUN_10047c3a0 |
| 0x34 | u32 | primate | 1 only for chimp, gorilla, mandrill and orangutan. Passed as a bool by Mac FUN_1004a963e |
| 0x38 | f32 | escape_height | Mac FUN_1005532ae fires an event when the enclosure's float at +0x50 is below this value |
| 0x3C | Ref txt | naming_name | Name_WildAnimalSpecies_*_ForAnimalNaming |

Variant (0x58 B):

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | Ref mdl | model | |
| 0x04 | Ref wad | animations | |
| 0x08, 0x0C, 0x10 | f32 | size_x, size_y, size_z | full extents. Also summed for the visual LOD (Mac FUN_1002fc894) |
| 0x14 | u32 | unk_14 | 33000 … 60000, cubs above adults |
| 0x18 | f32 | unk_18 | 0.55 … 0.9 |
| 0x1C | Ref txt | description | |
| 0x20, 0x24 | f32 | unk_20, unk_24 | adults about 1, cubs about 0.5 |
| 0x28, 0x2C | u32 | unk_28, unk_2C | adults 1/1 … 1/3, cubs 0/5 |
| 0x30 | Ref gsi | icon | |
| 0x34, 0x38 | f32 | unk_34, unk_38 | always equal. Adults above cubs (lion 3.0 / 1.47) |
| 0x3C..0x48 | f32 ×4 | unk_3C..unk_48 | mostly 0. Bears 0.61 ×2 |
| 0x4C | u32 | unk_4C | 0..3 |
| 0x50 | u32 | sound_count | 14..19 |
| 0x54 | → AnimalSound[n] | sounds | |

AnimalSound (44 B): +0 `sound` Ref snd; +4 `event` (animation / action id: 9 eat, 12 drink, 15 sleep …);
+8 `start_time` f32 (stored on the one-shot when > 0); +0x0C/+0x10 `pitch_min/max` (random factor, scales rate and
length); +0x14/+0x18 `volume_min/max`; +0x1C/+0x20 `distance_min/max` (< 0 = engine default); +0x24 `unk_24`
(0 everywhere, passed to the sound-instance constructor); +0x28 `looping` (1 looping object, 0 one-shot).

Genetics (24 B): `hunger`, `peep_enjoyment`, `social`, `water_body_requirement`, `adolescence_period`, `active` (u32).
Stats (36 B): `prey_count`, `prey` (→ char*[n] species names), `breeding_happiness_req`, `breeding_space_req`,
`vertical_step_reach` f32, `diet_type` (0 meat, 1 hay, 2 foliage), `months_between_births`, `babies_pre_pregnancy_av`,
`gestation_period`.

The variant `unk_*` words have no reader: the only code that holds a variant pointer is WildAnimalVisual (+0xD8).
It reads +0x00, +0x04, +0x08..+0x10, +0x50 and +0x54 only. The AI side never selects a variant through the species
pointer (Mac FUN_1005539aa returns it; its 39 callers read +0x2C, +0x30, +0x34, +0x38 and +0x3C only).

#### `ent` — Entertainer = EntertainerDesc (14 objects, 14 pairs)

256 B. There are 7 Characters/Entertainers/Entertainer%d and 7 Characters/VIPStats/VIP%d. In the VIP pairs the
struct does not start the block: the thought table (12 B × n) and its txt refs come first, and +0xA8 points to the
block start. In the 8 objects with no VIP data, the words +0xA4..+0xFC are 0, apart from +0xAC. Consumers (Mac):
parts FUN_1002d4398 / FUN_100a37e60; speed factors FUN_1002d5fbc / FUN_100a693b6
(`speed = base × factors[table[anim_state]]`); debug name FUN_1005df742; height FUN_1003be553, which returns it per
person type and adds it to the position for the eye / camera height; thoughts FUN_1006d035c; animation masks
FUN_1002d5c2c; +0xB8/+0xBC FUN_100994be0; +0xC0..+0xFC FUN_1009949ac. Guests reach the desc through person +0x168
(3 VIP, 7 entertainer) and +0x169 (index).

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | zero | | |
| 0x04 | u32 | speed_factor_count | 15 |
| 0x08 | → f32[n] | speed_factors | per motion class, multiplies the base walk speed |
| 0x0C | u32 | part_count | 5 |
| 0x10 | → char*[n] | parts | people parts: head, body, legs, swim body, swim legs |
| 0x14 | char* | debug_name | "Panda", "Joe Sluggerball" |
| 0x18 | f32 | height | 1.44 … 2.18 |
| 0x1C | Ref txt | display_name | |
| 0x20 | Ref gsi | costume_icon | `:gsi` for VIPs |
| 0x24 | Ref gsi ×16 | lower_colours | `<Name>Lower_UniformColour_N` |
| 0x64 | Ref gsi ×16 | upper_colours | `<Name>Upper_UniformColour_N` |
| 0xA4 | u32 | thought_count | |
| 0xA8 | → Thought[n] | thoughts | VIPs only |
| 0xAC | u32 | anim_count | bit length of each mask (329, 348) |
| 0xB0 | u32 | anim_mask_count | |
| 0xB4 | → AnimMask[n] | anim_masks | |
| 0xB8, 0xBC | i32 | pose_time_0, pose_time_1 | FUN_100994be0(0 / 1): value / constant, compared with the animation timer |
| 0xC0 | {f32 z, f32 y} ×8 | pose_offsets | FUN_1009949ac(pose 2..9) returns (0, y, z), the local offset of a pose. Non-VIPs use the built-in tables |

Thought (12 B): +0 `thought` (id the guest code asks for), +4 `text_count`, +8 → txt refs[n] (one picked as
`index % n`). AnimMask (16 B): +0 `flag_word` (which of the guest's three animation flag words gets the bits),
+4 `flags_if_set`, +8 `flags_out_of_range` (used when the animation id ≥ `anim_count`), +0xC → u32 bitmask of
⌈anim_count / 32⌉ words, one bit per animation id. The JSON gives the mask as hex words.


### 21.4 Animations and wild-animal models

Layouts in `extractors/src/DataExtractor_Anims.cpp` (`register_anims`). `ovl_layout_check` → 0 issues on
all five tags. Evidence: Mac decompile (`decompile/ghidra/decompiled.c`) + whole-corpus statistics.

#### `san` — SimpleAnim (3 010 objects, 14 pairs)

People head animations (`Characters/*/Shared/Head_*`). Loader is a no-op relocator
(`FUN_10040b036` registers "san"; Load = `return`). 16 B structs back to back.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | key_count | 1–30 |
| 0x04 | u32 | channel_count | = `bone_count` of the owning `sal` (2, 3 or 4) |
| 0x08 | f32* | times | `key_count` key times in s; first key is not at 0, last key may repeat (equal times, 1 157 cases) |
| 0x0C | rec* | channels | `key_count × channel_count` value records, key-major: record(k, c) = values + (k·channels + c)·24 |

Value record (24 B):

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | f32[3] | translation | bone position in the parent (channel 0: model space) |
| 0x0C | i16[3] | x_axis | unit vector × 32767: column 0 of the bone rotation (bone local X in the parent frame) |
| 0x12 | i16[3] | y_axis | column 1; z = x × y |

Not tangents (rerct3 lead was wrong): over all 70 789 records both vectors are unit length (max deviation
2.4e-5) and orthogonal (max |dot| 3.7e-5). Column, not row, interpretation: `Bip01 Head` (channel 1)
sits at `(0.073, 0, 0)` in the neck frame and neck x_axis ≈ (0, 1, 0), so the head lands 7 cm above the
neck, as it must. JSON: `channels[c] = {translation[k][3], x_axis[k][3], y_axis[k][3]}` (decoded floats,
scale 1/32767 applied).

**Channel → bone mapping.** Channel *c* is `sal.bone_names[c]`: always `Bip01 Neck` (parent −1),
`Bip01 Head` (parent 0), then optionally `Bip01 Ponytail1`, `Bip01 Ponytail11` (4-channel heads AF/PF/TF/VF)
or `Bip01 Glasses` (JS). Head placement for §20: `M_head = [R(neck)|t(neck)] · [R(head)|t(head)]`,
R = (x_axis, y_axis, x_axis × y_axis) as columns.

#### `sal` — SimpleAnimList (27 objects, 14 pairs)

40 B. Referenced by `phd+0x8C`. Lists every animation of one head (one `L1` and one `SS_` swimsuit list per head).

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | bone_count | 2–4 |
| 0x04 | char** | bone_names | channel names of every `san` of the list |
| 0x08 | i32* | bone_parents | −1 = root, else index into bone_names |
| 0x0C | u32[4] | — | 0 everywhere |
| 0x1C | u32 | anim_count | 0x3B–0xC3 |
| 0x20 | char** | anim_names | "Walk", "HoldHandKidBoth", … (the people animation names) |
| 0x24 | san&[n] | anims | symbol refs, same order as anim_names |

#### `modelanim` — ModelAnim (1 125 objects, 19 pairs)

Wild-animal clips (`WildAnimals/*_anims`). 88 B + 1 extra-data chunk. Loader Load = `FUN_10026d3aa`
(Mac): it reads the extra chunk as NUL-terminated names and stores, for each name, a string-table id
into the u32 slots of lists +0x08 (count +0x18), +0x0C (+0x1C), +0x10 (+0x20) and pairs in +0x14
(+0x24, two names each); unload `FUN_10026d69e` releases them. The rerct3 lead had the counts wrong
(+0x18/+0x1C are the list counts, not "boneCount/totalBoneCount" of a hierarchy).

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | f32 | duration | = (frame_count − 1) / 30 (30 fps) |
| 0x04 | u32 | frame_count | 13–116 |
| 0x08 | u32* | translation_bones | name-id slots (0 on disk); JSON: names from the extra chunk |
| 0x0C | u32* | rotation_bones | idem |
| 0x10 | u32* | list3 | idem; always empty (null, count 0) |
| 0x14 | u32* | name_pairs | idem, 2 ids per entry; always empty |
| 0x18 | u32 | translation_bone_count | 7–10 (Bip01, Spine, Spine1, clavicles, thighs, …) |
| 0x1C | u32 | rotation_bone_count | 23–34: the whole skeleton |
| 0x20 | u32 | list3_count | 0 |
| 0x24 | u32 | name_pair_count | 0 |
| 0x28 | f32* | translations | frame-major: frame f, bone b at +(f·n_t + b)·12: xyz |
| 0x2C | f32* | rotations | frame-major: +(f·n_r + b)·16: quaternion x, y, z, w (all unit length) |
| 0x30 | u32[10] | — | 0 everywhere |

Extra chunk: `translation_bone_count + rotation_bone_count` names, translation bones first (verified
on all 1 125 objects). Data sizes check exactly (e.g. `MaleGorillaWalkSick`: 30 × 7 × 12 = 2 520 B
to the block end). JSON: `translations[b] = {bone, keys[f][3]}`, `rotations[b] = {bone, keys[f][4]}`.
The float keys are not compressed (the 11/10/11 packed quaternions of the rerct3 runtime track object are
a runtime form, not the on-disk one).

#### `wad` — WildAnimalAnimData (41 objects, 19 pairs)

Variable size 0x38 + 4·anim_count (31 anims → 180 B; one `Elephant:wad` has 8 → 88 B). No-op loader.
Consumers (Mac): `FUN_1002faff0` (species setup, reached through the `{mdl, wad, …}` record at
animal+0xD8), locomotion `FUN_1002ff6dc`, `FUN_100333a1c` (walk/run blend),
`FUN_1002fbf10` (anim clock).

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | head_look_neck_chain | passed with "Bip01 Neck" to the head look-at controller `FUN_10025a478`: number of bones above the neck (parent chain) that share the turn (0, 2, 3) |
| 0x04 | u32 | head_look_head_chain | idem with "Bip01 Head", bones below it (1–3) |
| 0x08 | u32 | anim_count | 31 (8) |
| 0x0C | ptr | anims_offset | always points at +0x38 of the struct itself (JSON: 0x38) |
| 0x10 | f32* | anim_durations | per anim; the anim clock wraps on it (`floorf(t / d)`), = modelanim duration |
| 0x14 | f32* | anim_speeds | per anim, m/s: position += dir · speed · dt (`FUN_1002ff6dc`); [0] walk, [1] walk sick, [2] run; 0 for in-place anims |
| 0x18 | f32 | unk_18 | 0.12–1.74, grows with body size; no reader found |
| 0x1C | u32 | unk_1C | 0, 1 or 2 (2: apes, bears, pandas; 1: mandrills); no reader found |
| 0x20 | f32 | unk_20 | 0.16–2.82, body length-like; no reader found |
| 0x24 | f32 | unk_24 | always k/30 (0.4–0.9 s): a time in frames/30; no reader found |
| 0x28 | f32 | run_blend_in_speed | when switching to anim 2 (run): blend = (v − speed[cur]) / (speed[run] − speed[cur]); < 0 disables. Overwritten at runtime by `FUN_1006c1814` |
| 0x2C | f32 | run_blend_out_speed | same when leaving run |
| 0x30 | f32 | unk_30 | 0 (37×), −0.3 … 0.9; no reader found |
| 0x34 | f32 | unk_34 | −1 (33×) or a positive value (0.78–1.91); no reader found |
| 0x38 | modelanim&[n] | anims | inline symbol refs, fixed order: Walk, WalkSick, Run, (null), Stand, StandSick, Idle1, Idle2, EatIn/Loop/Out, DrinkIn/Loop/Out, SleepIn/Loop/Out, Scratch, InteractALoop, InteractB, HitObject, ClickReaction, Sit, InteractAIn/Out, TurnL20/45/90, TurnR20/45/90 (indices 0x19–0x1E are the turns, read by `FUN_1006c1814`) |

#### `mdl` — Model (50 objects, 22 pairs)

Wild-animal skinned model (`WildAnimals/*_data`). 80 B struct + 2 extra-data chunks: the model image
and the bone names. Active loader (fgdk3 `ModelRes/Code/Runtime/Model.cpp`): Mac `FUN_100255a02`
(relocates the image in place), `FUN_1002560f6` (per-LOD mesh setup), `FUN_10020803c` (vertex size from
the format word), unload `FUN_1002557a8`.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u16 | — | 0 |
| 0x02 | u16 | flags | 1 on disk (bit 0 asserted by `FUN_100256420`); bits 2/4/8/0x10 = runtime load stages |
| 0x04 | u8 | lod_count | 5 or 6 |
| 0x05 | u8 | resident_lod_count | = lod_count everywhere; LODs [0, lod_count − resident) would be streamed from a third chunk |
| 0x06 | u16 | material_count | 1–8 |
| 0x08 | f32[4] | bounding_sphere | centre xyz, radius (= bbox centre / half diagonal) |
| 0x18 | f32[3] | bbox_min | |
| 0x24 | f32[3] | bbox_max | |
| 0x30 | — | model (Custom) | +0x30..+0x4F runtime handles (bone/material tables, LODs, buffers, +0x4C optional count), 0 on disk; the field decodes the two chunks |

Model chunk (all offsets in the chunk; the loader aligns to 16 where noted):

```
+0   u32 bone_count, u32 n1, u32 n2, u32 n3   (n1..n3 = 0 everywhere: extra name lists)
+10  bone[bone_count] (0x60): f32 position[4] (w = 1), f32 rotation quat[4] (xyzw),
                              f32 inverse_bind_matrix[16] (row-major, translation in row 3)
     u16 parent (0xFFFF root), u16 next (= i+1, 0xFFFF for the last) per bone
     (u32[n2])                                           -> align 16
     u32 size[material_count], then the material names (C strings of those sizes) -> align 16
     LOD record[lod_count] (0x14): u16 material_record_count (= material_count), u16 mesh_count,
                                   u32 bone_count, ptr records, ptr meshes, u32 has_bone_mask (0)
     u32 lod_ext[lod_count] (0; non-zero -> one more 0x14 record) -> align 16
     per LOD: (bone mask, unused) mesh[mesh_count] (0x20) -> align 16, then per mesh
              vertices, u16 indices -> align 4, then material_record[n] (0x10) + their range lists
```

Mesh record (0x20): `u32 format` (0x1305 in all 321 meshes), `u32 index_count` (triangle list),
`u16 index_sets` (1; index bytes = sets × count × 2), `u16 vertex_count`, `u16 unk_0C` (0 everywhere),
`u16 unk_0E` (0xFFFF everywhere; libOVLng `ModelExtraMeshHeader` also leaves both unexplained),
4 runtime pointers (0). Vertex format 0x1305 = 44 B: `f32 position[3]`, `f32 normal[3]`,
`u8 bone[4]`, `u8 weight[4]` (sum 255 on every vertex, bones < bone_count), `u32 colour` (FFFFFFFF),
`f32 uv[2]` — size matches `FUN_10020803c` (bits 1: xyz, 4: normal, 0x100: 4 weights, 0x200: colour,
0x1000: one uv set). Material record (0x10): `u16 material`, `u16 n_ranges`, `u16 n_ranges_b`, `u16 0`,
2 runtime ptrs; then `n_ranges` × `{u16 first_mesh, u16 mesh_count}` and `n_ranges_b` × same
(`ranges_b` empty everywhere). The walk ends exactly at the chunk end on all 50 models.

Name chunk: `bone_count` NUL-terminated bone names ("Bip01", "ACamChase", "Bip01 Pelvis", …), same
names as the `modelanim` rotation bones. (Needed the PairView fix: v5 subversion-1 files start their
extra-data stream with a 0 word.)

JSON: `model = {bones[{name, parent, next, position, rotation, inverse_bind_matrix}], materials[names],
lods[{bone_count, meshes[{format, vertex_count, index_count, unk_0C, unk_0E, positions, normals,
bone_indices, bone_weights, colours, uvs, indices}], materials[{material, ranges, ranges_b}]}]}`.


### 21.5 Scenery items, stalls, special attractions

Layouts: `extractors/src/DataExtractor_Scenery.cpp` (`register_scenery`). `ovl_layout_check`:
sid 5 995 / 5 995 ok, sta 89 / 89, sat 6 / 6, 0 issues. All pointers on disk stay 32-bit
in the Mac x86_64 build (FUN_1001f2550 turns a 32-bit handle into a pointer), so the
offsets the Mac code reads are file offsets.

#### `sid` — SceneryItem (5 995 objects, 162 pairs)

The struct is 0xD4 (structure_version 0), 0xE4 (1, Soaked: `_Sext`) or 0xEC (2, Wild: `_Wext`),
chosen by the u16 at +0xA (the game itself tests `u32(+8) > 0xFFFF` / `> 0x1FFFF`).
In the Mac game the descriptor sits at SceneryItem+0xF8.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | — | 0 everywhere |
| 0x04 | hex | flags | placement flags. 0x400000: sounds start without their first script (FUN_1007caaa6). Most common: 0x14E1, 0x16E1, 0x1041 (supports), 0x10E1 |
| 0x08 | u16 | position_type | 0 full tile (5 439), 3 wall, 4 quarter, 1/2 path edge, 5 half, 6 path centre, 7 corner, 8 path-edge join |
| 0x0A | u16 | structure_version | 0 / 1 / 2 (2 050 / 1 276 / 2 669) |
| 0x0C | u32 | height | 0 (4 736) or the item height in height units: equals size_y and the largest square max_height_block on all 1 259 non-zero (supports 1m→1 … 8m→8, Steepslope 11, Windmill 17). libOVLng "unk1". No game reader found |
| 0x10 / 0x14 | u32 | squares_x / squares_z | footprint |
| 0x18 | ptr | squares | → squares_x·squares_z × square (20 B), row-major |
| 0x1C..0x24 | f32×3 | position_x/y/z | bounding box min |
| 0x28..0x30 | f32×3 | size_x/y/z | bounding box size (tiles of 4 m, height units) |
| 0x34 | u32 | track_struct_count | |
| 0x38 | ptr | track_structs | → count × track_struct (88 B); 2 903 sids |
| 0x3C | i32 | cost | build price in cents (200 = $2 on 3 770 items) |
| 0x40 | i32 | removal_cost | cents, negative = refund (200, −1000, −400…) |
| 0x44 | u32 | unk_44 | 5 rides / track / stalls / most scenery (4 714), 0 walls / paths / supports / pool pieces (1 252), 4 on the 29 raised paths. Importer "unk2". No reader found |
| 0x48 | u32 | type | scenery class (0 tree, 2 shrub, 3 flowers … 18 ride track, 27 stall; checked against the classification condition FUN_1009f04b8) |
| 0x4C | char* | supports | support style (`ts1`, `scaffolding`, `defaultpool`, ""…) |
| 0x50 / 0x54 | u32 / ref[] | svd_count / svds | `:svd` refs (2–4 for trees / LOD variants) |
| 0x58 | ref | icon | `:gsi` (1 568) |
| 0x5C | ref | group_icon | `:gsi` |
| 0x60 | ref | group_name | `:txt` |
| 0x64 | char* | ovl_path | |
| 0x68 / 0x6C | u32 / ptr | param_count / params | → {char* type, char* params} (LitterBin, Sign, RideEvent…) |
| 0x70 / 0x74 | u32 / ptr | sound_count / sounds | → sound (16 B); 114 sids, mostly ride pieces |
| 0x78 | ref | name | `:txt` |
| 0x7C, 0x80 | u32 | — | 0 everywhere |
| 0x84 | i32 | break_time_min | breakdown timer, see below |
| 0x88 | i32 | break_time_range | |
| 0x8C..0x94 | u32×3 | default_colour_1..3 | FlexiColour indices 0..31 of the three remap channels (0 = unset on ~5 370) |
| 0x98..0xB4 | 32 B | — | 0 everywhere |
| 0xB8 / 0xBC | u32 / char*[] | anr_name_count / anr_names | individual animations (2 flat rides) |
| 0xC0 | u32 | — | 0 everywhere |
| 0xC4 | u32 | anr_age_alternatives | |
| 0xC8 | u32 | anr_animation_chunks | |
| 0xCC | u32 | anr_alternate_run_animations | |
| 0xD0 | u32 | anr_cycles_per_circuit | (all four read by the flat-ride code, FUN_100421c94: `+0xC4 < 2`, `+0xC8 * +0xC4`) |
| *v ≥ 1* | | | |
| 0xD4 | ptr | extra_sounds | → sound_count × extra_sound (12 B) |
| 0xD8 | u32 | — | 0 everywhere |
| 0xDC | u32 | addon_pack | 0 vanilla, 1 Soaked, 2 Wild (read by FUN_1007ccf72) |
| 0xE0 | u32 | generic_addon | 0 / 1 / 2 (2 on GenericStall, DonutStall…; read by FUN_1007ccf72) |
| *v = 2* | | | |
| 0xE4 | f32 | unk_E4 | −1 on 2 666, 0.5 Acacia, 2 Baobab, 3.4 Banyan (Wild trees). Importer "unkf". No reader found |
| 0xE8 | i32 | billboard_aspect | −1 none, 0..6 = 8:1 … 1:8 (read when version ≥ 2, else −1) |

Breakdown timer (FUN_1007cf59e, SceneryItem BREAKFLAGS @+0xBC / BREAKTIME @+0xC0): when either
field is non-zero the item arms BREAKTIME = min + rand()·range game-time units (2880 = 1 day,
§terrain-fog), counts it down and sends a break message at 0. Stalls (type 27) ignore the
fields and use 6000 + rand()·8000. Stalls carry 1440 / 2880, CashMachine 0 / 2880.

square (20 B), one per footprint cell:

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | hex | flags | per-cell collision / support flags (bit 0 tested by the support pass FUN_1007cf778, bit 1 "support column needed" per rerct3). Common 0xFFB, 0, 3, 0xFF9 |
| 0x04 | i32 | min_height_block | lowest blocked height unit (−2..) |
| 0x08 | i32 | max_height_block | highest |
| 0x0C | ptr | block_bits | → ⌈(max−min)/32⌉ u32 bit words, one bit per height unit (ManagerSID) |
| 0x10 | u32 | support_type | support orientation (1 vertical on 21 842, 0, 2..7 diagonal / mirrored; rerct3 RebuildCellVisuals) |

track_struct (88 B), support / girder connection (libOVLng SceneryItemTrack). Three points:

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 / 0x04 | i32 | start_x / start_z | cell (0..11) |
| 0x08 / 0x0C | u32 | start_sub_x / start_sub_z | 0..2 in the 3×3 sub-grid |
| 0x10 / 0x14 | f32 | start_offset_x / start_offset_z | ≈ 0.5 within the sub-cell |
| 0x18 | f32 | start_height | |
| 0x1C / 0x20 | i32 | end_x / end_z | −1..10 |
| 0x24 / 0x28 | u32 | end_sub_x / end_sub_z | 0..2 |
| 0x2C | f32 | end_height | |
| 0x30 | u32 | angled | 0 everywhere |
| 0x34 / 0x38 | i32 | control_x / control_z | 0 everywhere (rotated by FUN_1007d304c) |
| 0x3C / 0x40 | u32 | control_sub_x / control_sub_z | 0 |
| 0x44 / 0x48 | f32 | control_offset_x / _z | 0.5 everywhere |
| 0x4C | f32 | control_height | 0 |
| 0x50 | hex | flags | 0..2 (bit 0 inverted, bit 1 reversed, rerct3) |
| 0x54 | u32 | group | 1..9; entries with the same group share a girder (FUN_1007d341c) |

The game registers a girder only when start and end cells differ (FUN_1007d1c3c compares
+0x00/+0x1C and +0x04/+0x20) and rotates each cell pair with FUN_1007ca90a
(FUN_1007d2fe6 start, FUN_1007d2f6a end, FUN_1007d304c control).

sound (16 B): `u32 sound_count; ref[] sounds (:snd); u32 script_count; ptr scripts → script_count
script pointers` (null = no script for that animation). A script is a list of
`{f32 time; u32 command; [f32 value; f32 end_time] if command 3 or 4}` ended by command 0
(ManagerSID; parsed by FUN_1007cad34): 1 start a random sound of the list, 2 stop, 3 volume
fade to `value` by `end_time`, 4 pitch fade. Over the game: 2 387 starts, 362 stops, 1 448
volume, 1 537 pitch, 1 221 ends.

extra_sound (12 B, v ≥ 1): `f32 min_distance` (< 0 → 5.0), `f32 max_distance` (< 0 → 100.0),
`u32 category` (0 / 2: index of the sound category list / volume channel the RunningSound joins,
FUN_10040169c). Most are (−1, −1, 0); fountains (5, 40, 2).

#### `sta` — Stall (89 objects, 34 pairs)

Two layouts, told apart by `u32(+0) == 0xFFFFFFFF` (FUN_100618b98; 54 SW, 35 V). The game then
finds the sid at tail+0 when the attraction type's byte 1 is 0 / 1, else at SW +0x28.

Stall_V (0x5C): Attraction_V (0x00..0x37, below) then

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x38 | ref | sid | `:sid` |
| 0x3C | u32 | item_count | 0 on kiosks (tested at tail+4) |
| 0x40 | ptr | items | → item_count × {ref item (`:cid`); i32 price (cents)} |
| 0x44..0x58 | u32×6 | appeal_female_child / _teen / _adult, appeal_male_child / _teen / _adult | 0..9 |

Stall_SW (0x2C): `0x00 sw_marker (0xFFFFFFFF), 0x04 item_count, 0x08 items, 0x0C..0x20 the six
appeals, 0x24 attraction → Attraction_S / _W, 0x28 sid`.

Appeal: FUN_10054a81e(stall, PersonType, SubType) returns tail+0xC/+0x10/+0x14 for a female
(SubType 1) child / teen / adult (PersonType 0 / 1 / 2, Person+0x168/+0x169, names from
FUN_1005df742: "Child ", "Teenage ", "Adult ", "Male", "Female") and +0x18/+0x1C/+0x20 for a male,
5 otherwise; FUN_10054a874 feeds it into powf(). Defaults 6 7 5 / 7 8 9; pirate toys 4 3 1 / 9 3 1;
T-shirts and kiosks 5 everywhere; pretzels 4 6 8 / 4 6 9.

Attraction (shared with the flat / tracked rides: §16.1). Vanilla 0x38 inline; behind the SW
pointer it is Attraction_S (0x40) when type byte 1 == 2 and Attraction_W (0x44) when == 3.

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | hex | type | low byte: 7 food, 8 drink, 9 misc, 0x0E info, 0x0B toilet, 0x0C ATM, 0x0D first aid; byte 1: 0 vanilla, 2 Soaked (S), 3 Wild (W) |
| 0x04 / 0x08 | ref | name / description | `:txt` |
| 0x0C | ref | icon | `:gsi` (all `:gsi`, empty) |
| 0x10 | i32 | cost | cents: 10000 on 77, 30000, 1000 |
| 0x14 | i32 | refund | cents, negative: −7500, −30000, −750 |
| 0x18, 0x1C | u32 | — | 0 everywhere |
| 0x20 | i32 | base_upkeep | 1500 stalls, 4960 sat |
| 0x24 | ref | spline | `:spl` (empty) |
| 0x28 / 0x2C | u32 / ref[] | path_count / paths | 0 on every stall / sat |
| 0x30 | hex | flags | 0 |
| 0x34 | i32 | max_height | 12 |
| *S, W* | | | |
| 0x38 | u32 | addon_pack | 0 / 1 / 2 |
| 0x3C | u32 | generic_addon | 2 on Donut / FrenchFry / Generic stalls (same as their sid +0xE0), else 0 |
| *W* | | | |
| 0x40 | hex | ui_deactivation | 0 |

#### `sat` — SpecialAttraction (6 objects, 4 pairs)

V (0x3C) = Attraction_V + `0x38 sid`. SW (0x0C) = `0x00 sw_marker (2), 0x04 attraction →
Attraction_S/W, 0x08 sid`; the game tests `*data != 2` (FUN_1006ec382, FUN_1006ec352 returns
`*(+4)` or `data − 0x38`). The three vanilla utilities (Toilet, CashMachine, Mcentre) ship both:
V in their own pair, SW (type 0x30B..0x30D, W attraction) in Style/Vanilla/Style.


### 21.6 People parts, heads, skins; manifold meshes

Layouts in `extractors/src/DataExtractor_People.cpp` (`--types data`). Evidence is
the Mac build (`decompile/ghidra/decompiled.c`). The Mac binary is 64-bit but keeps
the 32-bit on-disk structs: every pointer slot is a 32-bit handle turned into a
pointer by `FUN_1001f2550`, so the offsets its consumers read are the on-disk
offsets. Loaders: `prt` `FUN_100635c78`/`FUN_100635e5a`, `phd`
`FUN_100626cce`/`FUN_100626dd8`; `ppg`, `ssk`, `mam` have empty loaders
(relocate only).

#### `prt` — PeoplePart (791 objects, 259 pairs)

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | `unk_00` | 1 in all 788 regular objects; no reader in the loader or the draw / pick / attachment paths. |
| 0x04 | u32 | (zero) | Runtime: the loader allocates a 0x161 B cache (byte 0 = mesh usable, +1.. global-bone → mms-bone map, +0x15D 4 attachment-point bone indices) and stores its handle here. |
| 0x08 | ref `tex` | `texture` | Bound before drawing the part (`FUN_1006a71a2` +8); batch key in `FUN_1006a2ef8`. 85 null (attachment dummies). |
| 0x0C | ref `mms` | `mesh` | Loader reads the mesh's bone names ("attachmentdummyhand/head/lhand/rhand" → attachment points 0/2/1/0); renderer its vertex / index counts. |
| 0x10 | u32 | `palette` | Index into the person's 5 palette bytes (SkinPalette … LowerSwimSuitPalette): `FUN_1006359d8` (PeoplePart_TransformAndCopyBatch) reads `palette[prt+0x10]` and offsets UVs by ((p&3)·0.25, (p>>2&3)·−0.25). |

Variant `prt_stray` (3 objects, `Characters/CF/CF01/SS_SkinLegs_CF01`): `texture`
ref at +0x00, zero +0x04/+0x08, `unk_0C` = 1, `mesh` ref at +0x10. The pair is a
stray duplicate: the same three symbols exist well-formed in
`CF01_SwimSuitLegs1_Main` / `CF01_SwimSuitLegs2_Main` (the ones the CF01 swimsuit
`ppg` reference). Chosen when +0x00 is a symbol reference.

#### `ppg` — PeoplePartGroup (157 objects, 157 pairs)

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | `unk_00` | 1; 2 on the Wild! groups (AK01, Smythe, Tiger). No reader found. |
| 0x04 | u32 | `expansion` | 0 base, 1 Soaked! (BobW, Sharky), 2 Wild! (AK01, Smythe, Tiger). Same pair of words as CarriedItem +0/+4 (libOVLng `carrieditems.h`: "unk1 always 1", "addonpack"). Swimsuits and the lifeguard (Soaked! content) still say 0; no reader found. |
| 0x08 | u32 | `unk_08` | 0/1: 1 on most guest / VIP swimsuit groups (not AM01_SwimSuitLegs2, BobW, Smythe) and on all three LG01 (lifeguard) groups. No reader found. |
| 0x0C | u8 | `person_type` | Matches Person `PersonType` (+0x168): 0 child, 1 teen, 2 adult, 3 VIP, 4 handyman, 5 mechanic, 6 security guard, 7 entertainer, 8 vendor, 9 animal keeper, 10 paramedic, 11 park inspector (names: debug switch in `FUN_1005df742`), 12 lifeguard (LG01). |
| 0x0D | u8 | `sub_type` | Matches Person `SubType` (+0x169): 0 male, 1 female ("Male"/"Female" in the same switch; AF/CF/TF/VF = 1). |
| 0x0E | u8 | `slot` | 0 head, 1 body, 2 legs, 3 swimsuit body, 4 swimsuit legs (the 5 Person part-group references; `FUN_1006b7ffe` is called with slot 0..4; draw reads the person's optional-part mask word `[slot]`). BobW_SwimSuitLegs says 2. |
| 0x0F | u8 | `optional_part_count` | Loop bound for optional parts in draw / pick / attachment / validation. |
| 0x10 | u32 | `unk_10` | 1 on AF01, AM05, CM05, TM05 heads only, else 0. No reader found. |
| 0x14 | f32* | `optional_part_chances` | `optional_part_count` floats: at setup (`FUN_1005df742`) bit i of the slot's optional mask is set when `rand() < chance[i]`. |
| 0x18 | f32 | `weight` | Selection weight: `FUN_1006b893a` copies type / sub_type / slot / +0x18 into the part-group registry, `FUN_1006b7ffe` picks a group per (type, sub_type, slot) by weighted random. 100, 25 on rarer heads (AF05, AM04, CM07, TM04). Not a LOD distance (person LOD thresholds are 20/50/300 in `FUN_1006a274c`). |
| 0x1C | 3 × lod | `lods` | Person LOD 0, 1, 2 (+0x8E selects). |
| 0x34 | ref `phd` | `head` | Detailed head, drawn at LOD 0–1 only (`FUN_1006a6774`: skipped when LOD > 1). |
| 0x38 | 3 × lod | `swim_lods` | Used instead of `lods` when the person's UseSwimSuit byte is set. |
| 0x50 | ref `phd` | `swim_head` | |

LOD record (8 B):

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u8 | `fixed_count` | Parts always drawn. |
| 0x01 | u8 | `optional_count` | Parts drawn when their mask bit is set; validation reports "%d Optional Part(s) Missing". |
| 0x02 | u16 | (zero) | |
| 0x04 | prt** | `parts` | `fixed_count + optional_count` refs; a null ref is an unresolved part (HM01_Head). |

#### `phd` — PeopleHead (87 objects, 44 pairs)

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | f32[16] | `hat_attachment` | Row-major matrix of attachment point 2 ("attachmentdummyhead"), multiplied by the head bone's matrix in `FUN_1006a6b5c`. Translation x 0.27–0.52 along the head bone (top of the hair; AF05's tall hair 0.52). |
| 0x40 | f32[16] | `face_attachment` | Same for attachment point 3 (no mms bone maps to it): x ≈ 0.16, z ≈ 0.15 — eye level, in front of the face (sunglasses). |
| 0x80 | u32 | `unk_80` | 1 in all objects; no reader found. |
| 0x84 | u32 | (zero) | Runtime: loader allocates a 0x160 B cache (word 0 = index of "Bip01 Head" in the sal, then a global-bone → sal-bone map) and stores its handle here. |
| 0x88 | ref `tex` | `texture` | Bound before drawing the ssk (`FUN_1006a71a2`, `FUN_1006a2ef8`). |
| 0x8C | ref `sal` | `animation` | Head bone animations: loader looks up "Bip01 Head" in its bone names; pose `FUN_100626e16`. |
| 0x90 | 2 × 6 ref `ssk` | `lods` | `lods[person LOD (+0x8E)][expression (+0xA8)]`, each {`neutral`, `angry`, `happy`, `sad`, `scared`, `sick`} (order from the symbol suffixes; 3 heads repeat the neutral skin). |

#### `ssk` — SimpleSkin (1 014 objects, 44 pairs)

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | `bone_count` | Outer loop of `FUN_1003fc026` (SimpleSkin_TransformVertices). |
| 0x04 | char** | `bone_names` | "Bip01 Neck", "Bip01 Head", "Bip01 Ponytail1"… Not read by the Mac code paths found (bone i uses matrix i of the head's sal pose). |
| 0x08 | u32* | `bone_vertex_counts` | Vertices of bone i follow those of bone i−1. |
| 0x0C | u32 | `triangle_count` | |
| 0x10 | u16* | `indices` | 3 per triangle, copied as-is to the index buffer. |
| 0x14 | u32 | `vertex_count` | |
| 0x18 | f32* | `vertices` | 8 floats per vertex: position (× bone matrix + translation), normal (× bone matrix, w = 0), U, V (+ the palette cell offset). Flat array in the JSON. |

Rigid skinning, positions in bone space (§20.5).

#### `mam` — ManifoldMesh (35 objects, 35 pairs)

Proxy meshes of the `Path/UnderWater/*` scenery, referenced by their `svd`
(`SceneryItemVisual_Sext::proxy_ref`, libOVLng `sceneryvisual.h`).

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | f32[3] | `bbox_min` | +0x0C zero (libOVLng writes 0). |
| 0x10 | f32[3] | `bbox_max` | +0x1C zero. |
| 0x20 | u32 | `vertex_count` | |
| 0x24 | u32 | `triangle_count` | |
| 0x28 | vertex* | `vertices` | 16 B per vertex: x, y, z, then a word that is 0 everywhere (checked). The JSON holds x, y, z flat. |
| 0x2C | u16* | `indices` | 3 per triangle (libOVLng pads the array to a multiple of 8 shorts; the pad is outside the field). |

Consumer `FUN_1004f8d48` (via `FUN_1004f85ec`): classifies each vertex as above /
below a height (y), then walks the triangles and builds the crossing edges — the
mesh's cross-section at a horizontal plane, i.e. where the water surface cuts the
underwater path piece.

#### Corrections to §20 (applied to `--types people`)

- `ppg` +0x18 is a selection weight, not a LOD distance (`lod_distance` in
  `<ovl>.people.json`). `flags[4]` = person_type, sub_type, slot,
  optional_part_count. +0x10 is not "1 on heads" (4 heads only). +0x14 points to
  the optional-part chances (not written by PeopleExtractor).
- `phd` matrix 0 / matrix 1 = hat / face attachment transforms relative to the
  "Bip01 Head" bone; +0x84 is a runtime slot.
- `prt` +0x04 is a runtime slot (always 0 on disk).
- `ssk`: the old "lodCount / lodOffsets / boneWeightIndices" hypothesis
  (rerct3 loader-structs.md) is refuted; the layout above matches the consumer.
