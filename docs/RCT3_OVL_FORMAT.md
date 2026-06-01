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
| `tex`  | Texture (DXT-compressed)         | DXT1 + DXT3 + DXT5 single-tex decoded (§4); multi-tex v5 open |
| `btbl` | BmpTbl — array of `FlicHeader` + texture data | Structure known via rct3dump (§4.1); multi-tex anchor |
| `flic` | Flic — inline texture, or an index into a `btbl` | Structure known via rct3dump (§4.1) |
| `txs`  | Texture shader / blend style     | 40 named styles known (§12); not yet applied to output |
| `fts`  | TextureSet?                      | Treated as ftx (untested)               |
| `ftt`  | TextureType?                     | Treated as ftx (untested)               |
| `gsi`  | Graphic Sprite Info (atlas rect) | Fully understood (§5)                   |
| `psi`  | Particle Sprite Info             | Pre-resolved only, not extracted        |
| `snd`  | Sound (.wav)                     | Fully decoded (§6)                      |
| `sid`  | SceneryItemData (placement/size/flags) | Struct known via rct3dump (§13); not extracted |
| `mms`  | Morphable Mesh                   | Topology + UVs OK, positions broken (§7)|
| `shs`  | Static Shape (rigid mesh)        | Fully decoded (§8)                      |
| `bsh`  | Bone Shape (skinned mesh)        | Struct known via rct3dump (§13); not extracted |
| `ban`  | Bone Animation (keyframes)       | Struct known via rct3dump (§13); not extracted |
| `svd`  | SceneryItemVisual (LOD + mesh refs) | Struct known via rct3dump (§13); not extracted |
| `was`, `asd`, `vwg`, `ent`, `mdl`, `ptd`, `qtd`, `ter`, `sta`, `trr` | Various game data | Structs partly known via rct3dump (§13); not extracted |

What "fully decoded" buys you, per loader, is documented in
[EXTRACTORS.md](EXTRACTORS.md).


## 3. FTX texture format

Every `ftx` linked file points at a 76-byte header followed by a palette and
an unrelated pixel-data block (see §3.2 for the split). All format codes
(3, 4, 5, 6, 7, 8, 9) share the **same on-disk layout** — indexed8 + BGRA
palette — so a single decode path works across all of them.

### 3.1 Header layout (at `loaderreference.datapointer`)

```
+0x00 u32  format       — size class: width = height = 2^format
+0x04 u32  width        — redundant, equals 2^format (except format=8)
+0x08 u32  height       — redundant, equals 2^format (except format=8)
+0x0C u32  unk_a        — 0
+0x10 u32  unk_b        — 0
+0x14 u32  unk_c        — 7 commonly; meaning unclear
+0x18 u32  mipmap_count — 1 for format=8; garbage for others (see §10)
+0x1C u32  metadata_off1
+0x20 u32  unk_e        — 1
+0x24 u32  metadata_off2
+0x28 u32  zero
+0x2C u32  format_repeat   — same as offset 0
+0x30 u32  width_repeat
+0x34 u32  height_repeat
+0x38 u32  unk_c_repeat
+0x3C u32  pixel_internal_offset  ← key field, see §3.2
+0x40 ...  256-entry × 4-byte BGRA palette  (1024 bytes)
```

### 3.2 Pixel data lives elsewhere

The 76-byte header + 1024-byte palette occupy the data block at the
loader's `datapointer`. The actual pixel **indices** (1 byte per pixel)
are in a **separate chunk**, reached via the `pixel_internal_offset` field
(passed through `parser.offset_to_position()`).

So decode = read palette from header block @ +0x40, follow
`pixel_internal_offset` for `width × height` 1-byte indices, then
`bgra[i] = palette[index]`. The 4th palette byte is **not** a per-entry
alpha — across the palettes we sampled (Dice, gigacoaster, …) it sits at 0
or small values for the whole 256-entry table, so we emit alpha = 255
unconditionally and let the caller paint transparency from the material
shader (see §3.5).

An earlier version of the extractor hard-coded `index 0 → alpha = 0`
(chroma-key heuristic). It worked for textures with a keyed-out background
(Carcass, foliage cutouts) but broke any model where index 0 is a real
color used in the mesh interior — most notably the Dice cube, where index
0 paints the dot/edge fill of an `SIOpaque` material.

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

### 3.5 Alpha is a material property, not a texture property

Whether a pixel should be transparent is decided by the **txs shader** bound
to the sub-mesh that samples the texture, not by the texture data. Sub-meshes
with `SIAlphaMask*` / `SIAlphaBlend*` txs need transparent areas; sub-meshes
with `SIOpaque*` don't. Since one texture can be sampled by multiple
sub-meshes (with different txs), baking alpha into the `.tga` would corrupt
the opaque cases. Restoring alpha specifically for the alpha-masked subset
is a follow-up that needs `txs` semantic decode (open item, §11).

**Lead from rct3dump (unconfirmed on disk):** the reference's in-memory
`FlexiTextureStruct` carries *three* separate data pointers —
`palette`, `texture` (the 1-byte indices), and a distinct **`alpha`** plane —
and its decode is literally `dest[i] = palette[texture[i]] | (alpha[i] << 24)`
*when `alpha != 0`*. That suggests some ftx entries store a real per-pixel
alpha plane next to the index plane, rather than relying solely on the txs
material. We have **not** yet located such a plane in the 76-byte on-disk
header (the reference struct is the post-load in-RAM form, where pointers are
fixed up; on disk they're internal offsets), but the `metadata_off1/2/3`
fields at +0x1C/+0x24/+0x3C are unexplained candidates worth probing on an
ftx known to need alpha (chain, foliage). See §11.

### 3.6 The "false positive" with A8

During reverse engineering we briefly believed non-format-8 textures were
A8 (grayscale alpha masks). They looked coherent as grayscale because
RCT3 palettes are frequently near-monotonic and the indices themselves
form recognizable shapes. The real format is palette indexed8 — using the
palette yields full color. (BambooSign should be green, not grey.)

### 3.7 The 5 stubs

In the 1594 ftx entries found in the full game, **5 fail to decode** —
all with a wildly invalid `pixel_internal_offset` (e.g. `1065353216` =
the IEEE-754 bit pattern for float `1.0`, clearly garbage). These are
placeholder entries that reference real textures stored elsewhere. Not
a bug, just empty slots.


## 4. TEX texture format — DXT1 decode

`tex` is the second texture container in RCT3 (alongside `ftx`). Where
`ftx` stores 8-bit indexed pixels in the same OVL block as its header,
`tex` puts a small wrapper on one side of the OVL pair and the actual
DXT-compressed pixel data in a **trailing section after all parsed OVL
structures** on the other side.

### 4.1 Where the pieces live

For each tex linkedfile (always on the side with parsed loaders for it,
typically `unique`), the OVL pair always contains, on the OPPOSITE side
(typically `common`):

- a `btbl` (BmpTbl) loader  — a count + an array of `FlicHeader`s (see §4.1.1)
- a `flic` (Flic) loader    — the per-texture data anchor
- raw DXT-compressed pixel data + mipmaps, sitting in the file AFTER
  everything the OVL parser enumerates (chunks, relocations, strings,
  symbol tables …). The file size is much larger than `dataend`.

The 76-byte wrapper at `tex`'s `loaderreference.datapointer` (on unique)
encodes mipmap LOD descriptors and pointers we never had to follow
once we located the trailing section directly.

#### 4.1.1 The `tex → flic → btbl` chain (from rct3dump)

rct3dump makes the multi-texture relationship explicit, and it's the key to
the still-open multi-tex v5 case:

- **`tex`** (`TextureStruct`) is a thin wrapper that points at a `flic`.
- **`flic`** (`FlicStruct`) either decodes its own inline pixel data, **or**
  (loader version ≠ 2) holds an **index into a `btbl` array** — i.e. several
  `flic`/`tex` symbols all source their pixels from one shared bitmap table.
- **`btbl`** (`BmpTbl = {u32 unk, u32 count}`) is followed by `count`
  `FlicHeader`s and then `count` texture payloads read back-to-back. **This is
  the per-texture table** that our parser currently sees as
  `unknownafterfileblocks`; matching each `flic` slot to its `btbl` index is
  what multi-tex extraction needs (§4.6).

So a single-tex OVL is just the degenerate `count == 1` case, which is why the
trailing-section scan (§4.3) works for it without modelling the chain.

**Empirical correction (this codebase).** A signature census of the full
install does **not** find the grouped `count × FlicHeader` array that
rct3dump's sequential reads imply. Of every texture-bearing `.ovl`, **117 hold
exactly one header**, and the *only* file with more than one is
**`Main.common.ovl`** — whose **3 DXT3 GUI atlases** appear as three *separate*
single-style trailing headers (§4.2) scattered across the file, not a
contiguous table. So rct3dump's `btbl` array is its **in-RAM** view (assembled
by the loader at runtime), not the on-disk layout. Practically, "multi-tex"
across the whole game = Main alone, so the extractor just emits Main's distinct
headers directly (§4.6) rather than modelling the chain. The only unresolved
part is the **symbol → atlas-page** mapping needed to drive `gsi` slicing.

#### 4.1.2 Two on-disk pixel layouts

rct3dump reads texture pixels through **two different code paths** depending on
the loader, and RCT3 OVLs use both:

1. **Contiguous mips** (`ReadTextures`, used after a `btbl`): a `FlicHeader`
   then every mip level concatenated, each level sized purely from
   `format`/`width`/`height` (no per-level header). This is the layout our
   single-tex DXT1/3/5 decoder handles (§4.2).
2. **Per-mip headers** (`ReadTexture`, used by `flic` loader version 2): each
   level is preceded by a `FlicMipHeader { u32 MWidth, MHeight, Pitch, Blocks }`
   and copied **pitch-aware** (`size = Pitch × Blocks`, row stride = `Pitch`).
   The level loop stops when `MWidth/MHeight/Pitch/Blocks` hit zero. We do not
   yet emit this layout; it likely explains any tex OVL whose trailing payload
   doesn't size-match a plain contiguous DXT chain.

### 4.2 Trailing-section header (48 bytes per texture)

```
+0x00 .. +0x0F  u32×4   relocation offsets (echo of parsed table)
+0x10           u32     0x18 constant (block-3 size hint?)
+0x14 .. +0x1B  zero
+0x1C           u32     format_code   ← D3D format (NOT a constant!) — see §4.4
+0x20           u32     width
+0x24           u32     height
+0x28           u32     mipmap_count
+0x2C           u32     total compressed pixel-data size in bytes
+0x30           bytes   first mip (largest), then each next level, all
                        DXT-compressed back-to-back.
```

**Correction (rct3dump):** the `0x12` at `+0x1C` is **not** a constant — it's
the texture's D3D format code, and `+0x1C .. +0x28` is exactly rct3dump's
`FlicHeader { u32 Format, Width, Height, Mipcount }`. We only ever saw `0x12`
because those were all DXT1; DXT3 textures carry `0x13` and DXT5 `0x14` in the
same slot (§4.4). Reading this field turns format detection from a size
heuristic into a direct lookup, and is what unlocked DXT3/DXT5 decode.

Verified empirically: sum of per-level compressed sizes (DXT1 = 4 bpp,
4×4 blocks padded to 8 bytes minimum) exactly matches the size field
(e.g. 256×256 mip=9 → 32768 + 8192 + 2048 + 512 + 128 + 32 + 8 + 8 + 8
= 43704 = `0xAAB8`, matching `+0x2C` byte-for-byte on every
single-tex character/clothing texture sampled).

### 4.3 Locating the header

`dataend` (what `OvlParser` stops at) lies a variable margin before the
trailing section — when an OVL side has no symbol resolves the parser
exits earlier than when it does, by 20+ bytes. We therefore scan
forward from `dataend` for the `{format, width, height, mip, size}`
signature at `+0x1C .. +0x2F`, requiring:

- `format_code ∈ {0x12, 0x13, 0x14}` (DXT1/DXT3/DXT5),
- `width` and `height` both powers of two in `[16, 4096]` (rectangular
  allowed — we no longer require `w == h`),
- `1 ≤ mip ≤ 16`, and
- `size` **exactly** equal to the mip-chain size computed from the format's
  block size (8 B for DXT1, 16 B for DXT3/5).

The format-code constraint (3 valid values out of 2³²) combined with the exact
size match makes the signature far stricter than the old size-only heuristic —
mms vertex/index buffers and prt/snd payloads don't trip it.

### 4.4 Format detection

The format is read **directly** from `format_code` at `+0x1C` (§4.2). RCT3
reuses the Direct3D format enumeration; rct3dump's `RCT3DFormatToD3DFormat`
gives the full table (only the DXT codes occur in `tex` trailing sections in
practice, but the rest is documented here for completeness):

| code   | D3D format     | code   | D3D format    | code   | D3D format |
|--------|----------------|--------|---------------|--------|------------|
| `0x01` | R8G8B8         | `0x09` | X4R4G4B4      | `0x12` | **DXT1 (BC1)** |
| `0x02` | A8R8G8B8       | `0x0A` | A4R4G4B4      | `0x13` | **DXT3 (BC2)** |
| `0x03` | X8R8G8B8       | `0x0B` | L8            | `0x14` | **DXT5 (BC3)** |
| `0x04` | R5G6B5         | `0x0C` | A8L8          | `0x15` | R3G3B2     |
| `0x05` | X1R5G5B5       | `0x0E` | V8U8          | `0x16` | A8         |
| `0x07` | P8 (palette)   | `0x10` | UYVY          | `0x100`–`0x103` | depth (D16/D32/D15S1/D24S8) |
| `0x08` | A1R5G5B5       | `0x11` | YUY2          |        |            |

What we decode today:

| `format_code` | Format     | bytes / 4×4 block | Status            |
|---------------|------------|-------------------|-------------------|
| `0x12`        | DXT1 (BC1) | 8  (4 bpp)        | **Decoded → TGA** |
| `0x13`        | DXT3 (BC2) | 16 (8 bpp)        | **Decoded → TGA** (explicit 4-bit alpha) |
| `0x14`        | DXT5 (BC3) | 16 (8 bpp)        | **Decoded → TGA** (interpolated alpha) |

All three decode to a 32-bit BGRA TGA at the largest mip level. Per-format
alpha handling:

- **DXT1** — opaque (`A = 255`) unless a block uses the `c0 ≤ c1`
  "punch-through" mode, where colour index 3 is fully transparent.
- **DXT3** — the leading 8 bytes of each 16-byte block hold 16 explicit 4-bit
  alpha values (scaled ×17 to 0–255); the trailing 8 bytes are a DXT1-style
  colour block but **always** in 4-colour mode (no punch-through).
- **DXT5** — the leading 8 bytes hold two 8-bit alpha endpoints + sixteen 3-bit
  indices into an 8-entry interpolated alpha ramp; colour block as DXT3.

The `+0x2C` size field is now used only as a **cross-check**: `data_size` must
equal the mip-chain size computed from `format_code` + `width` + `height` +
`mipmap_count`, which also keeps the trailing-section scan (§4.3) from
false-positiving.

**Prevalence in stock RCT3** (raw signature census across all 14 952 `.ovl`
files, both sides): **110 DXT1** headers, **10 DXT3** headers (8 files), and
**zero DXT5** anywhere in the shipping game. So the BC3/DXT5 path is implemented
to spec for completeness and custom content, but never fires on stock assets
(untested on real data). DXT3 is rare; its single-tex instances (e.g.
`Mackeral` — a 32×32 cutout, verified: 642 transparent + 382 opaque texels)
extract correctly, while most DXT3 sits inside multi-tex icon atlases that are
still gated (§4.6).

### 4.5 Coverage today

Raw signature census across the complete RCT3 Complete Edition install (every
`.ovl`, both sides) finds **120 distinct DXT texture headers**: 110 DXT1,
10 DXT3, 0 DXT5. The gated extractor (single-tex only, §4.6) decodes:

| Format | Headers | Decoded | Notes |
|--------|--------:|--------:|-------|
| DXT1   | 110     | 110     | all single-tex, symbol-named |
| DXT3   | 10      | 9       | 6 single-tex + Main's 3 atlas pages; `chimp_data` (1) deferred |
| DXT5   | 0       | —       | absent from stock RCT3 |

The 6 single-tex DXT3 textures are `Mackeral` (32², the validation case) plus
five 256²/512² atlases — `PathIcons`, `ShopsIcons`, `EnclosureIcons`,
`PoolIcons`, and `WildAnimals` — each a single big DXT3 image sliced by `gsi`
rects, so decoding them makes `gsi` atlas-sprite extraction (§5) viable for
those packs. The only multi-header OVL is `Main` (3 DXT3 GUI atlases), now
emitted as index-named atlas pages (§4.6); its per-`gsi`→page routing stays
open. The lone holdout is `chimp_data` (2 `tex` symbols but only one findable
header), deferred as an edge case.

### 4.6 Open work on `tex`

- **DXT3/DXT5 decode** — ✅ **done.** `format_code` at `+0x1C` selects the
  decoder (§4.4). **DXT3** (explicit 4-bit alpha) is verified on real assets —
  6 single-tex textures now decode, including the `EnclosureIcons` / `PathIcons`
  / `ShopsIcons` / `PoolIcons` atlases and `Mackeral`. **DXT5** (interpolated
  alpha) is implemented to the BC3 spec but **does not occur anywhere in stock
  RCT3** (0 of 14 952 files), so it's unverified on real data — it's there for
  custom content.
- **Multi-tex layout** — install-wide this is **only `Main.common.ovl`** (3 DXT3
  GUI atlases; §4.1.1 explains why rct3dump's `btbl` array isn't the on-disk
  shape). The per-symbol path *defers* multi-tex OVLs — the trailing scan is
  symbol-blind, so decoding per `tex` symbol would emit one near-duplicate per
  symbol (Main has 86). Instead a separate pass (`extract_multitex_atlases`)
  decodes the **distinct** trailing headers once and writes
  `<stem>__atlas<N>_<W>x<H>.tga`. Result: Main yields its 4 `ftx` textures + 3
  atlas pages (before, it produced 88 `.tga`, only 4 distinct). What remains is
  the **symbol → atlas-page mapping**: rct3dump builds it via the in-RAM
  `tex → flic → btbl` chain, but that linkage isn't surfaced by our parser, so
  we can't yet say which `gsi` rect belongs to which page.
- **Per-mip `FlicMipHeader` layout** (§4.1.2) — needed if any `flic` v2 texture
  turns out not to size-match a contiguous chain. Not yet observed in failing
  cases, but documented so it isn't re-discovered from scratch.
- **`gsi` atlas extraction** — for single-tex atlas packs (`PathIcons`,
  `ShopsIcons`, `EnclosureIcons`, `PoolIcons` …) the parent texture now decodes,
  so their `gsi` rects are croppable today (`AtlasExtractor` already has the
  slicing code). The big shared `Main` sheet (~1900 `gsi`) needs the
  symbol → atlas-page mapping above before its sprites can be routed correctly.

OVLExtractor-2 does not decode `tex` either — its handler is a stub
that emits a `<tex format='18'>` XML element referencing a `.png`
that's never written. Cobra-tools has no RCT3 `tex` support.

**Atlas splitting status:** `tex` is now decoded, so the per-pack atlases
(`PathIcons`, `ShopsIcons`, `EnclosureIcons`, `PoolIcons`, … — each a single
`tex` image) decode and their `gsi` rects can be cropped. The only atlas that
still can't be split is `Main`'s shared GUI sheet set (3 pages, ~1900 `gsi`),
because routing a `gsi` rect to the correct page needs the symbol → atlas-page
mapping that isn't surfaced yet (§4.6). `ftx`-backed GSI worked all along.

**NOT a blocker for static meshes:** zero `shs` files in the random-sample
survey reference a `:tex`. Cracking `tex` is unnecessary for textured
3D models (see §8.5).


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
to avoid flooding the output with noise variants — only `shs` is exported
during full sweeps. To inspect a single MMS, call the extractor with the
specific OVL or manually re-enable the `mms` branch.


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
conventions.

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
  `SIOpaqueSpecular50Reflection`). RE on `txs` is open — for now we ignore it.

Survey of 76 shs across a 40-OVL random sample:

- **75/76 (99%)** reference at least one `:ftx`
- **Zero** reference a `:tex` — atlas wrapper textures are only used by `gsi`,
  not by 3D meshes. Cracking `tex` is **not** required for textured models.
- Sub-mesh count distribution: 29× single, 19× double, 20× triple, 7× quad

The texture often does not live in the same OVL as the SHS — coaster pieces
in `tracks/coasters/Track6/45medslopechain_data.unique.ovl` reference
`gigacoaster:ftx` which lives in `tracks/coasters/Track6/Track6_Textures.common.ovl`.
The global symbol index (§9) resolves these cross-OVL references.


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

- **`mipmap_count` is garbage for non-format-8 ftx headers.** Header
  field at offset 0x18 reads values like 511821, 263360, etc. Either
  the layout differs for non-format-8 or this field was repurposed.
  We hardcode `1` for those.
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
| `tex` symbol→atlas-page mapping | Single-tex DXT1/3/5 is decoded and `Main`'s 3 atlas pages now extract index-named (§4.6); the remaining piece is routing each `gsi` rect to the right `Main` page, which needs the in-RAM `tex → flic → btbl` linkage surfaced by the parser. Affects only `Main`'s ~1900 GUI sprites; single-tex atlas packs already slice. Not required for shs models — none reference tex. |
| `mms` position decode | Unlocks readable 3D meshes for animated objects (animals, characters, ride cars). |
| `txs` shader semantics | Refines `.mtl` output to encode alpha mask, reflection, specular per sub-mesh based on the `txs` symbol. The 40 styles + their D3D blend/alpha-test/alpha-ref values are now tabulated in §12 — enough to drive both `.mtl` flags and ftx alpha re-masking. |
| `ftx` per-pixel alpha plane | rct3dump's `FlexiTextureStruct` has a separate `alpha` plane (§3.5). If present on disk it would let opaque + alpha-masked sub-meshes share one ftx correctly without txs guesswork. |
| `bsh` / `ban` skinned meshes + animation | Structs fully laid out in §13 (vertex has a bone index; `ban` holds translate/rotate keyframes). Would unlock animated character / animal export. |
| OVL header v6 | Currently parser warns and continues; some Wild! / Soaked! OVLs may be affected. |
| The 5 stub ftx textures | Cosmetic — could be filtered out at extract time. |
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
+0x18  u32   color  (D3DCOLOR)         +0x18  u32   Bone     ← bone index
+0x1C  float u, v                      +0x1C  u32   unk
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
  u32 TotalVertexCount, TotalIndexCount  u32   BoneNumber
  u32 MeshCount2, MeshCount
  BoneShape2** sh                       BoneShape2: like StaticShape2 but
  u32 BoneCount                           Vertexes are VERTEX2, Triangles u16
  BoneStruct* Bones
  D3DMATRIX* BonePositions1  ← bind pose (per bone)
  D3DMATRIX* BonePositions2  (≈ identical to 1 in practice)

BoneAnim (ban)         BoneAnimBone               txyz (keyframe)
  u32 BoneCount          char* Name                 float Time
  BoneAnimBone* Bones    u32 TranslateCount         float X, Y, Z
  float TotalTime        txyz* Translate
                         u32 RotateCount
                         txyz* Rotate
```

Skinning recipe (from `DoShapes`): each vertex's position/normal is multiplied
by `BonePositions1[vertex.Bone]`. Animation adds, per keyframe, the bone's
`Translate` to the matrix's `_41/_42/_43`, and converts the `Rotate` keyframe
(an axis-angle **rotation vector**, magnitude = angle) to a rotation matrix via
Rodrigues' formula (`rotmath`, `rct3tex.cpp:2807`). `Bones[i].BoneNumber ==
0xFFFFFFFF` marks an unused bone.

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
