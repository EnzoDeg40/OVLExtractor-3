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
| `psi`  | Particle Sprite Info             | Pre-resolved only, not extracted        |
| `snd`  | Sound (.wav)                     | Fully decoded (§6)                      |
| `sid`  | SceneryItemData (placement/size/flags) | Footprint / svd / cost subset decoded for track pieces (§15.3) |
| `spl`  | Spline (Bézier rail / profile)   | Fully decoded (§15.1)                   |
| `tks`  | TrackSection (one track piece)   | Fully decoded incl. Soaked/Wild ext (§15.2); connection rules §15.4 |
| `trr`  | TrackedRide                      | Decoded: pieces, trains, track set path, speeds, groups (§15.5) |
| `mms`  | Morphable Mesh                   | Topology + UVs OK, positions broken (§7)|
| `shs`  | Static Shape (rigid mesh)        | Fully decoded (§8)                      |
| `bsh`  | Bone Shape (skinned mesh)        | Rest-pose mesh decoded (§8.6); skeletal animation open |
| `ban`  | Bone Animation (keyframes)       | Tracks decoded → JSON sidecar (§8.7); skinned render open |
| `svd`  | SceneryItemVisual (LOD + mesh refs) | Struct known via rct3dump (§13); not extracted |
| `was`, `asd`, `vwg`, `ent`, `mdl`, `ptd`, `qtd`, `ter`, `sta` | Various game data | Structs partly known via rct3dump (§13); not extracted |

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
played at `fps`. The extractor decodes frame 0 only.


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

Pages come out exactly as stored, and the stored orientation is **not
uniform**: the GUI icon sheets (`GUI2Main`, `GUI2LargeIcons*`, `FrontEndIcons`,
`FrontEndParts`, scenery icons) are top-down, while the splash logos
(`gui/Banner`), the loading screen, the career-map images of
`gui/FrontScreen`, `FrontEndLogo` and every **font page** are stored
bottom-up — the game draws those elements with a flipped V. Nothing in the
wrapper, the txs style, the format or the mip count tells the two apart (both
happen with the same `GUIIcon:txs`), so it is an authoring convention: the
extractor leaves pages and `gsi` sprites as stored, and only the font
extractor flips (§17), where the convention is certain.

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
in the page's stored orientation (§4.3), so a sprite of a bottom-up page
(career-map images) is itself bottom-up.


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


## 7. MMS — morphable mesh (positions BROKEN)

The `mms` loader holds morphable meshes for animals, characters, and ride
cars. Indices and UVs decode correctly; **vertex positions do not**.

### 7.1 MMS header (40 bytes at datapointer)

```
+0  u32  vertex_count
+4  u32  index_count                — count of u16 indices, /3 = triangle count
+8  u32  algo_unknown_1             — "lower for less vertices, can't be 0,
                                       vertices leak if wrong" (legacy comment).
                                       Probably controls position decode but
                                       its exact role is unknown.
+12 u32  algo_unknown_2             — usually == unknown_1
+16 u32  type_flag                  — usually 0, 1 on some attachments
+20 u32  morph_count                — number of morph animations (≥1 even for
                                       "static" meshes)
+24 u32  vertex_uv_offset           — points to base vertex+UV buffer (12B/vtx)
+28 u32  index_offset               — points to u16 triangle list
+32 u32  unknown_2                  — seen 0
+36 u32  morph_data_offset          — points to morph descriptors (64B each)
```

### 7.2 Base vertex/UV buffer (12 bytes per vertex)

```
+0 u16  unknown_a       — possibly bone or morph index
+2 u16  unknown_b
+4 float  U             — texture U in [0, 1]
+8 float  V             — texture V in [0, 1]
```

UVs work — they extract cleanly and match expected ranges.

### 7.3 Morph descriptor (64 bytes per morph)

```
+0   32 bytes unknown
+32  u32 name_ptr           — pointer to morph name string (e.g. "1Swim")
+36  u32 times_count        — number of keyframes
+40  u32 times_offset       — pointer to keyframe timings list
+44  u32 positions_offset   — pointer to per-keyframe vertex positions ← KEY
+48  u32 attachment_offset
+52  12 bytes unknown
```

Morph 0 of any mesh is typically the "base" or "rest" animation — what
we'd want for a static OBJ export. Each morph holds `times_count` keyframes.

### 7.4 Why positions don't work

The `MorhpMeshVertex` struct in `OVLExtractor-2/SFStructs.h:37` declares
3 × uint8 per vertex (so 3 bytes per vertex per keyframe). `ModelExtractor`
currently tries **17 different decoder candidates** on each MMS, writing
one diagnostic OBJ per candidate next to the default (`<name>.<decoder>.obj`):

| Decoder name       | Width | Interpretation                                |
|--------------------|-------|-----------------------------------------------|
| `int8_127_s3`      | 3 B   | signed int8 / 127 (default output)            |
| `uint8_255_s3`     | 3 B   | unsigned int8 / 255 − 0.5                     |
| `int8_127_s4`      | 4 B   | int8/127, 4-byte stride (1 padding)           |
| `int16_s6`         | 6 B   | int16 / 32767                                 |
| `float16_s6`       | 6 B   | IEEE 754 half-precision                       |
| `float32_s12`      | 12 B  | full float32                                  |
| `packed10_snorm`   | 4 B   | 3 × 10-bit SNORM in a u32                     |
| `sm8_s3`           | 3 B   | sign-magnitude int8                           |
| `sm8_s4`           | 4 B   | sign-magnitude int8 + 1 padding               |
| `bias128_s3`       | 3 B   | `(u - 128) / 127`                             |
| `bias128_s4`       | 4 B   | bias128 + 1 padding                           |
| `int8_xzy`         | 3 B   | int8 with axis swap XZY (Z-up → Y-up)         |
| `int8_flipy`       | 3 B   | int8 with Y axis negated                      |
| `delta_int8`       | 3 B   | running sum of int8/127 deltas                |
| `base_int8_a4`     | 12 B  | first 3 bytes of UV record as int8/127        |
| `base_int16_xy`    | 12 B  | first 4 bytes of UV record as int16 XY        |

None produce coherent geometry across the sample set. Adjacent vertices DO
share leading bytes on disk (consistent with a smooth mesh having close
vertices), but the resulting triangles look scrambled in 3D viewers.

The legacy OVLExtractor-2 explicitly left position decoding commented out
(`Form1.h:2222-2235`) — they couldn't figure it out either. Cobra-tools
has no RCT3 mesh support.

Untested hypotheses:
- **Per-keyframe scale/bias header** before the positions — the
  `Algorithm Unknown 1/2` header fields might contain a scale factor
- **Z-order swizzled** indices into a separate position table
- **Compressed delta from a base pose** stored elsewhere (per-loader or
  in a parent SID/SVD block)

### 7.5 What ModelExtractor outputs for MMS

- Valid OBJ structure
- Correct topology (faces reference correct vertex indices)
- Correct UVs (`vt` lines match in-game UV mapping)
- **Incorrect vertex positions** (cosmetically wrong, mesh appears shattered)
- 17 sibling OBJs (one per decoder candidate) for A/B comparison

In bulk batch runs, `side_loop` in `ModelExtractor.cpp` skips MMS entirely
to avoid flooding the output with noise variants — only `shs`/`bsh` are
exported during full sweeps. To inspect a single MMS, set **`OVL_MMS=1`** (the
mms branch is gated behind that env var) and extract the specific OVL; it emits
`MMS_HDR` / `MORPH_DESC` / `VBUF` / `POS_BYTES` diagnostics to stderr.

### 7.6 New evidence — `mms` is quantized to a bias+scale (not solved)

`rct3dump` does **not** cover `mms`. Running the `OVL_MMS=1` path on `Mackeral`
(6 v) and `Duck` (LODs of 63/49/20 v) surfaced concrete new findings:

- **The morph descriptor carries a per-axis bias + scale.** Its first 32
  "unknown" bytes are two float3s: `+0..+8` ≈ a min/offset (Mackeral
  (−0.073, −0.060, −0.358); Duck (−0.107, −0.141, −0.251)) and `+16..+24` tiny
  positives (~1e-4…3e-4) ≈ a scale. This **confirms the §7.4 "per-keyframe
  scale/bias" hypothesis** and **refutes the legacy "raw 3×uint8 position"**:
  positions are quantized, dequantized as `pos ≈ bias + raw·scale`.
- **But naïve uint8/uint16 dequant doesn't give consistent geometry.**
  `scale_z·65535 ≈ 16` over-ranges Z for a fish that's ~4.6 across; uint8
  under-ranges everything. So the raw width/alignment likely varies, the
  quantization isn't simple linear-full-range, or `positions_off` has a
  sub-header before the data.
- **`stride_id` (header +8, legacy "AU1") is not a byte stride** — it varies per
  mesh (Mackeral 6 = vc; Duck LODs 47/38/15 ≈ 0.75·vc), more like a count of
  unique/shared entries.
- **The base buffer's `a`/`b` u16 fields look like a position remap** — they
  descend with vertex index (Mackeral 5,4,3; Duck-L1 a=b=46,45,44; Duck-L2
  a≠b: 17/20, 16/19), consistent with §7.4's "swizzled indices into a separate
  position table" (vertex *i* may read `position[a_i]`).

Net: **materially advanced** (the dequant constants are located, two hypotheses
resolved) but **not solved**. Next step: pin `positions_off`'s exact byte span
(to `index_off` / the next morph) to derive the true per-vertex stride, then
test `pos = bias + raw·scale` with the `a`-remap and candidate widths.

**Systematic brute-force (still unsolved).** `OVL_MMS=1` now also dumps the raw
`positions`/`index`/`vbuf` blocks + `bias`/`scale` to `/tmp/mms_dump/`, and a
scorer (`/tmp/mms_brute.py`) was run over the cross product of {u8, i8, u16,
i16, half, f32, packed-10} × {strides 3/4/6/12} × {byte offsets 0–16} ×
{sequential, `a`-remapped}, scoring each decode by the index buffer's triangle
edge coherence (mean edge ÷ bbox diagonal — scale/bias-invariant). **No
per-vertex format produced a coherent mesh** (best ≈ 0.36, indistinguishable
from random scatter). A **delta** (cumulative-sum) decode scored much lower
(i16 ≈ 0.146) — but rendering it confirms that's a *metric artifact* (delta
naturally shortens consecutive-vertex edges even when the topology is wrong; the
duck still renders shattered). Calibration on 300 real `shs`/`bsh` meshes shows
the metric itself is weak (real meshes span 0.10–0.74, median 0.52, because
many are low-poly with large triangles), so edge-coherence alone can't rank
candidates. Conclusion: the position stream is **not** a simple per-vertex
quantization at `positions_off`; it's likely a non-trivial compression (true
delta-from-base with a separate base, a block/range codec keyed by
`stride_id`≈0.75·vc, or positions reached through another indirection). Genuinely
hard — left open with the dump tooling in place for a future attempt.


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
| Page orientation | `tex` pages are stored either top-down or bottom-up with no in-OVL marker (§4.3). Fonts are flipped by their extractor; other pages and `gsi` sprites come out as stored. |
| `RCT1_icons:tex` | Referenced by 20 `gsi` of `Main`, defined by no OVL of the install (an unshipped RCT1 icon sheet). |
| `mms` position decode | Unlocks readable 3D meshes for animated objects (animals, characters, ride cars). Advanced this session: positions are quantized to a bias+scale found in the morph descriptor (§7.6); exact raw width/remap still TBD. |
| `txs` specular/reflection | Alpha is solved (ftx alpha plane §3.5 + txs class). Still approximate: `*Specular*` → glTF roughness 0.5, `*Chrome*` / `*Reflection*` → metallic 0.8. The environment maps they sample (`chrome:ftx`, `two:ftx`) are not in any OVL — presumably built into the engine — so chrome parts export untextured. |
| Recolor | Recolorable ftx export with their blue/magenta/yellow mask ramps (§3.8). Applying player/default colours needs the item's default colours (probably in `sid`). |
| `UseAdTexture:ftx` | Placeholder the generic stall signs bind; the game substitutes the chosen advert at runtime. Exported untextured. |
| `sid` / `sta` gameplay fields | Name, description and sold items are surfaced (§13.4). Prices, footprint, default colours (for recolor) and the `sid` placement data are not decoded yet. |
| Animated ftx | Only frame 0 is decoded (§3.9). |
| OVL header v6 | Currently parser warns and continues; some Wild! / Soaked! OVLs may be affected. |
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
| 31 | constant speed preset (< 0 = none) | | 69–72 | track, track-big, car, car-swing profile `spl` |

Soaked/Wild extension (after the 300 B): `+0x04` section count, `+0x1C/+0x20`
group-definition count / arrays (`{u32 n; {flags, char* group | tks ref}×2 [n]}`,
the auto start/mid/end rules), `+0x3C/+0x40` track-path count / `char*[]`.

E.g. `WoodenRC`: track path `../Track20/Track20`, 85 pieces, trains
`WoodenCoaster, WagonCoaster, 6PeepWoodenRC, …`, station `Wooden`, chain
speed 4 m/s (2.24–6.26), upkeep 1100/300/1000.

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

**Supports** are not part of the piece meshes; the game builds them at
runtime (`TrackPiece_BuildSupports` → `SupportGrid_PlaceColumns` →
`ScenerySupportManager_RebuildCellVisuals` in rerct3's notes). Each piece's
`sid.supports` names its style (`ts1` for Track3/Corkscrew and most steel
sets, `ts2` for Track20/wooden, `ts6` for Track32–34…). The style's pieces
are `sid`s in the central `Supports/Supports.ovl` (257 of them), meshes in
`Supports/<style>/`, by type: **12** middle (`_support_1H/2H/4H/8H`), **13**
top, **14** bottom (footing), **15** bottom-extra, **16** girder
(`_girder_1L/2L/3L`), **17** cap.

**Where columns go: the sid connection entries** (`sid +0x34` count,
`+0x38` → 88-byte `SceneryItemTrack`). They come in groups
(`direction_type` 1, 2, 3…). In a group, the entry whose `end` cell equals
its `start` cell is the point the track rests on: footprint cell `start`,
`start_sub[2..3]` (always 0.5; the game picks the sub-cell itself, see below), attach height `height`
above the piece base (the underside of the track: 0 for a Track3 straight,
0.95 mid-way up a `medslope`). The other entries of the group name
neighbouring cells, some outside the footprint (`(0,-1)`, `(0,1)`): where the
column moves, with a horizontal **girder**, when it cannot go straight down.
A straight has one group, a 2×2 tight curve one, a loop four placed by hand,
none at the top of the loop. Local cell → world tile is the game's
`SceneryItem_GridTransform` (rerct3), sub-positions turn with
`TransformSubGridFloat`.

**Measured in the running game** (Mac build, `rct3_saves` probe,
`scripts/verify/live_supports.py`). Every drawn scene node has a 4×4 world
matrix in memory, last column `(0,0,0,1)`; the probe's `mats` command lists
them all in a box. A real node has the class pointer `0x100d28f50` 0x70
bytes before its matrix and its model 0x58 before; the model's float at
+0x2C is the mesh height (1, 2, 4, 8 for the `ts1` pieces). With
`SupportLOD 50 50` in `Options.txt` the game builds the supports of the whole
park at once, not just the part in view. Two parks were read whole: The Money
Pit (Giga, 91 predicted columns, **62 exact** piece for piece) and Fright
Night (LayDown, 148 predicted, **89 exact**). What those measurements fix:

| Rule | Detail | Evidence |
| --- | --- | --- |
| Sub-position | The column stands on a **3×3 sub-grid** of the tile (centres 1/6, 1/2, 5/6), not at the entry's `start_sub` (always 0.5 there). It is the sub-cell where the **track's underside** is at the attach height: the rail mid-line moved down by the ride's track depth along the banked normal (1.18 on the Giga = rail height above the support point on a flat Straight), then 0.25 towards the lower side. If several sub-cells qualify (flat track), the one the track crosses the longest, then the one nearest the centre. | 77 of 79 single columns of The Money Pit; the 2 misses are both `Straight2tilt90left` |
| Footing | At **ceil(ground)**, not at the ground: ground 0.72 → footing at 1, 3.13 → 4. It sinks into the terrain. | every column measured |
| Top | The top piece starts at **round(attach) − 1** and is **stretched vertically** to end exactly at the attach height (Y scale 0.76 for attach 17.76, 1.37 for 22.37, 1.45 for 11.45). The column touches the track. | ~70 columns, no exception |
| Middles | Between the footing and the top piece, at multiples of their own height, largest first (`RebuildCellVisuals`). | every column measured |
| Blocked tile | A tile holding **any scenery item** (a bush at height 0 is enough) or another piece of track lower down takes no column. | e.g. bushes on (36,50), (36,57), (50,55), (50,57), (49,70), (34,47) |
| Neighbour + girder | The column then moves to a free cell named by another entry of the group, on that tile's sub-cell **nearest the support point**, and ends 0.443 under the attach height. A horizontal girder (the `3L` mesh, 3.66 long) is centred between the two, at attach − 0.22, stretched to the gap + 0.73 (X scale 1.292 for a 4.0 gap, 0.927 for 2.667). No free neighbour: no column. | (50,57) → (51,57), (49,70) → (49,69), (50,55) → (51,55), (36,57) → (37,57) |

**Not understood yet** (the remaining misses):
- **Spacing.** On a run of identical one-tile pieces (a chain lift, a run of
  `Steepslope`) the game keeps **one column out of two**, with no scenery
  involved. rerct3 notes that `TrackPiece_BuildSupports` walks the linked
  pieces (≤ 20) "to avoid double supports"; a greedy rule "skip when the
  previous column is closer than ~4–5.5 units" walked backwards along the
  track explains most of it but not all. Not implemented: those columns are
  still predicted.
- Whole stretches with no support and nothing on the tiles (The Money Pit
  (55..64, 46..50); in Fright Night columns over very low ground).
- `Straight2tilt90left`; the columns predicted under the Fright Night
  `Halfloop` (the game has none).

`rct3_track.py layout --supports` implements every rule of the table; the
park save provides the terrain and the scenery (a `.trk` has neither:
ground 0, no obstacles).

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
