# RCT3 OVL Format — Reverse-Engineering Notes

Findings accumulated while building the cross-platform OVL extractor
(`OVLExtractor-3`). This is **not** a complete spec — only the bits we needed
to extract textures, sounds, and meshes, and that were either undocumented,
wrong in the legacy OVLExtractor-2 source, or absent from cobra-tools.

Reference sources we cross-checked:
- `OVLExtractor-2` (legacy .NET reader by Belgabor et al., partial)
- `cobra-tools-master` (Frontier games — JWE, Planet Zoo, Planet Coaster;
  has **no** RCT3 support for `ftx`/`tex`/`gsi`)
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
| `tex`  | Texture (atlas wrapper)          | Partially RE'd, **not decoded** (§4)    |
| `fts`  | TextureSet?                      | Treated as ftx (untested)               |
| `ftt`  | TextureType?                     | Treated as ftx (untested)               |
| `gsi`  | Graphic Sprite Info (atlas rect) | Fully understood (§5)                   |
| `psi`  | Particle Sprite Info             | Pre-resolved only, not extracted        |
| `snd`  | Sound (.wav)                     | Fully decoded (§6)                      |
| `sid`  | Sound sub-record inside `svd`/`phd` | Pre-resolved only, not a top-level loader |
| `mms`  | Morphable Mesh                   | Topology + UVs OK, positions broken (§7)|
| `shs`  | Static Shape (rigid mesh)        | Fully decoded (§8)                      |
| `svd`, `was`, `asd`, `vwg`, `ent`, `mdl` | Various game data | Listed by parser, not extracted |

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


## 4. TEX wrapper format (partially RE'd, NOT decoded)

`tex` loaders are 76-byte wrappers used for atlases — every `gsi` in
the game references a `tex` texture, never a plain `ftx`. The wrapper's
binary structure is:

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

**Blocker for atlas splitting:** 3679 gsi entries in the game, 783
`tex` textures. All atlases use `tex`, so until `tex` is cracked, atlas
splitting (via `AtlasExtractor`) only succeeds for the rare GSI whose
parent texture is stored as plain `ftx` instead of `tex` — most atlases
yield zero visual output.

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
- **No DXT compression in RCT3.** We spent hours trying DXT1/3/5 + RGB565
  + BGRA8888 variants before discovering everything is indexed8 palette.
  RCT3 ships pre-2004, hardware DXT was an option but Frontier opted
  for palette textures across the board.
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
| `tex` texture format decode | Unlocks 3 679 atlas sprites (cosmetics / GUI). Not required for shs models — none reference tex. |
| `mms` position decode | Unlocks readable 3D meshes for animated objects (animals, characters, ride cars). |
| `txs` shader semantics | Refines `.mtl` output to encode alpha mask, reflection, specular per sub-mesh based on the `txs` symbol. |
| OVL header v6 | Currently parser warns and continues; some Wild! / Soaked! OVLs may be affected. |
| The 5 stub ftx textures | Cosmetic — could be filtered out at extract time. |
| Dice vertical stretch | Minor cosmetic question, not investigated. |
