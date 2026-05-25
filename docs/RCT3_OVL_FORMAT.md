# RCT3 OVL Format — Reverse-Engineering Notes

Findings accumulated while building the cross-platform OVL extractor
(`OVLExtractor-3`). This is **not** a complete spec — only the bits we needed
to extract textures and that were either undocumented, wrong in the legacy
OVLExtractor-2 source, or absent from cobra-tools.

Reference sources we cross-checked:
- `OVLExtractor-2` (legacy .NET reader by Belgabor et al., partial)
- `cobra-tools-master` (Frontier games — JWE, Planet Zoo, Planet Coaster;
  has **no** RCT3 support for `ftx`/`tex`/`gsi`)
- Empirical hex inspection of `.common.ovl` files from the Steam release


## 1. High-level structure

An RCT3 asset comes as a **pair** of files :
- `<name>.common.ovl` — shared data (textures, palettes, geometry)
- `<name>.unique.ovl` — per-instance data (gsi sprite refs, model instances…)

The parser reads both into `data_[0]` (common) and `data_[1]` (unique).
Cross-side references are normal: a `gsi` on the unique side typically
points at a `ftx`/`tex` texture on the common side.

Each side has:
- A header (magic `0x4b524746` = `"FGRK"`, versions 1/4/5/6)
- 9 chunks of payload data
- A loaders table (which loader handles each data block)
- A linkedfiles table (the actual data entries, each tied to a loader + symbol)
- A symbol-resolves table (relocation slots, see §5)
- A relocations table (pointer-fixup list)


## 2. Loaders we touched

| Tag    | Meaning                          | Status                          |
|--------|----------------------------------|---------------------------------|
| `ftx`  | FlexiTexture (palette indexed8)  | Fully decoded (this doc)        |
| `tex`  | Texture (atlas wrapper)          | Partially RE'd, **not decoded** |
| `fts`  | TextureSet?                      | Treated as ftx (untested)       |
| `ftt`  | TextureType?                     | Treated as ftx (untested)       |
| `gsi`  | Graphic Sprite Info (atlas rect) | Fully understood (this doc)     |
| `psi`  | Particle Sprite Info             | Pre-resolved only, not extracted|
| `snd`  | Sound (.wav)                     | Extracted via `SoundExtractor`  |
| `sid`  | Sound sub-record inside `svd`/`phd` | Pre-resolved only, not a top-level loader |
| `mdl`  | Model (3D geometry)              | Not yet attempted               |
| `mms`  | Morphable Mesh                   | Not yet attempted               |
| `svd`, `was`, `asd`, `vwg`, `ent` | Various game data | Listed by parser, not extracted |


## 3. FTX texture format (the one that works)

Every `ftx` linked file points at a 76-byte header followed by a palette
and an unrelated pixel-data block (see §3.2 for the split).

### 3.1 Header layout (at `loaderreference.datapointer`)

```
+0x00 u32  format       — size class: width = height = 2^format
+0x04 u32  width        — redundant, equals 2^format (except format=8)
+0x08 u32  height       — redundant, equals 2^format (except format=8)
+0x0C u32  unk_a        — 0
+0x10 u32  unk_b        — 0
+0x14 u32  unk_c        — 7 commonly; meaning unclear
+0x18 u32  mipmap_count — 1 for format=8; garbage for others (see §3.3)
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
`bgra[i] = palette[index]`.

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

### 3.5 The "false positive" with A8

During reverse engineering we briefly believed non-format-8 textures
were A8 (grayscale alpha masks). They looked coherent as grayscale
because RCT3 palettes are frequently near-monotonic and the indices
themselves form recognizable shapes. The real format is palette
indexed8 — using the palette yields full color. (BambooSign should be
green, not grey.)

### 3.6 The 5 stubs

In the 1594 ftx entries found in the full game, **5 fail to decode** —
all with a wildly invalid `pixel_internal_offset` (e.g. `1065353216` =
the IEEE-754 bit pattern for float `1.0`, clearly garbage). These are
placeholder entries that reference real textures stored elsewhere. Not
a bug, just empty slots.


## 4. TEX wrapper format (partially RE'd, NOT decoded)

`tex` loaders are 76-byte wrappers used for atlases — every gsi in
the game references a `tex` texture, never a plain ftx. The wrapper's
binary structure is :

```
+0x00 to +0x1C : 8 × u32, all 0x00070007 — likely (h_log2=7, w_log2=7)
                 repeated 8 times (mipmap LOD descriptor?)
+0x20 u32 = 1       — count?
+0x24 u32 = 8       — bits per pixel? (8 = indexed8)
+0x28 u32 = 16      — ?
+0x2C u32 = 0
+0x30 u32 = 0x00020001
+0x34 u32 → frame array offset (16-byte entries, self-referencing)
+0x38 u32 → sibling tex meta block
+0x3C u32 = 0
+0x40 u32 = 0
+0x44 u32 → self-pointer (this tex's own offset)
+0x48 u32 → +4 into frame array
```

Each "frame array" entry (at the +0x34 target) is 16 bytes:
```
+0  u32   pointer (= self_offset + 4 — purpose unclear)
+4  u32   0
+8  u32   1
+12 float 1.0
```

We could not locate the actual pixel data through these pointers. The
chunk holding the wrapper is too small for 16k bytes of indices. The
real data is probably in a different chunk, reached by an offset we
have not yet identified.

OVLExtractor-2 does not decode `tex` either — its handler is a stub
that emits a `<tex format='18'>` XML element referencing a `.png`
that's never written. Cobra-tools has no RCT3 `tex` support.

**Blocker for atlas splitting :** 3679 gsi entries in the game, 783
tex textures. All atlases use tex, so until tex is cracked, atlas
splitting produces zero visual output.


## 5. GSI — atlas region descriptor (fully understood)

Each `gsi` linkedfile is 16 bytes describing one rectangular region
within a tex texture :

```
+0  u32  texture_ref_slot_a — bytes are 0 on disk (relocation slot)
+4  u32  texture_ref_slot_b — bytes are 0 on disk (relocation slot)
+8  u32  coord_block_offset → internal offset to the rect (16 bytes)
+12 u32  padding (0)
```

At `coord_block_offset` :
```
+0  u32  left    — pixel-space
+4  u32  top
+8  u32  right
+12 u32  bottom
```

The texture reference is **not stored in the gsi data block** —
both slots (+0, +4) are zeroed and get filled by relocations at game
load. To recover the link offline, look up `SymbolResolve.pointer == gsi_off + 4`
in the symbol-resolves table; its `stringpointer` field is the texture
symbol (e.g. `"Extras:tex"`).

The legacy `OVLReader::ReturnDatablocknameFromOffset((startoffset+4),true)`
in `OVLExtractor-2` was looking up a linkedfile whose `datapointer`
matched `gsi+4` — which only worked for textures stored as separate
linkedfiles. For tex-backed atlases that's the wrong table; you need
`symbolresolves` instead.


## 6. Quirks worth remembering

- **`mipmap_count` is garbage for non-format-8 ftx headers.** Header
  field at offset 0x18 reads values like 511821, 263360, etc. Either
  the layout differs for non-format-8 or this field was repurposed.
  We hardcode `1` for those.
- **Animated textures (Dolphin, lavabubble, TVSeq…) used to fail extraction.**
  Symptom: tiny `pixel_internal_offset` (6, 64). Root cause: previous
  attempts read pixels from the wrong block; the palette-decode path
  works fine because both palette and indices are reachable.
- **No DXT compression in RCT3.** We spent hours trying DXT1/3/5 + RGB565
  + BGRA8888 variants before discovering everything is indexed8 palette.
  RCT3 ships pre-2004, hardware DXT was an option but Frontier opted
  for palette textures across the board.
- **Dice texture appears "stretched vertically"** in the extracted TGA.
  Reading dimensions from offset 4 + 8 gives 128 × 128 (matches what's
  in `format_repeat` block at +0x2C). Not investigated further; likely
  a non-square aspect applied at render time.


## 7. MMS — 3D mesh (partially RE'd, positions BROKEN)

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

Morph 0 of any mesh is typically the "base" or "rest" animation — what we'd
want for a static OBJ export. Each morph holds `times_count` keyframes.

### 7.4 Why positions don't work

The `MorhpMeshVertex` struct in `OVLExtractor-2/SFStructs.h:37` declares
3 × uint8 per vertex (so 3 bytes per vertex per keyframe). We tried:

- int8 / 127 (assuming signed normalized) → values in [-1, 1] but geometry
  is incoherent (random-looking polygons in Preview)
- uint8 / 255 (unsigned normalized) → same result
- 4-byte stride (3 position + 1 padding) → also incoherent
- 4-byte stride with shared-prefix observation (adjacent verts identical) →
  still incoherent

Adjacent vertices DO share leading bytes on disk, consistent with a smooth
mesh having close-together vertices. But the indices then produce
triangles whose actual 3D positions are scrambled.

Possible explanations we did not get to test:
- **Packed 10-bit per component** in u32 (DEC10/SNORM10, common in
  GPU vertex compression) — would fit 3× 10-bit + 2-bit padding in 4 bytes
- **Per-keyframe scale/bias header** before the positions — the
  "Algorithm Unknown 1" / "Algorithm Unknown 2" header fields might
  contain a scale factor
- **Z-order swizzled** indices into a separate position table
- **Compressed delta from a base pose** stored elsewhere (per-loader or
  in a parent SID/SVD block)

The legacy OVLExtractor-2 explicitly left position decoding commented out
(`Form1.h:2222-2235`) — they couldn't figure it out either. Cobra-tools
has no RCT3 mesh support.

### 7.5 What ModelExtractor currently outputs

- Valid OBJ structure
- Correct topology (faces reference correct vertex indices)
- Correct UVs (`vt` lines match in-game UV mapping)
- **Incorrect vertex positions** (cosmetically wrong, mesh appears shattered)

To actually use the output, someone would need to crack the position
encoding. Until then `ovlextract -t model` produces files that load but
don't look like anything in particular.


## 8. What's still open

- **tex texture format decode** — would unlock 3679 atlas sprites
- **mms position decode** — would unlock readable 3D meshes
- **Texture/material linking** for mms — once positions work, link to ftx
  via parent svd/phd loaders for textured OBJ exports
- **The 5 stub textures** could be filtered out at extract time
- **Dice vertical stretch** — minor cosmetic question, not investigated
