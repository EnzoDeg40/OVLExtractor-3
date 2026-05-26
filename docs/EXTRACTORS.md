# Extractors & Tooling

How `OVLExtractor-3` turns the raw bytes documented in
[RCT3_OVL_FORMAT.md](RCT3_OVL_FORMAT.md) into files you can open in standard
tools (Audacity, Blender, GIMP, Preview, browsers …). For the file-format
spec itself, see the sibling document.


## 1. Architecture overview

```
core/                    libovlcore — pure C++17 parser
  └─ OvlParser           reads .common.ovl + .unique.ovl into two OvlData,
                         resolves internal offsets → file positions, hands
                         out linked-file / loader / symbol-resolve tables.

extractors/              libovlextractors — strategy per loader tag
  ├─ DumpExtractor       textual structural dump (debug)
  ├─ SoundExtractor      snd → .wav (PCM, optional `smpl` loop chunk)
  ├─ TextureExtractor    ftx/tex/fts/ftt → .tga + .ovltex + .json
  ├─ AtlasExtractor      gsi → cropped sprite .tga
  ├─ ModelExtractor      shs → .obj + .mtl   (mms diagnostic only)
  └─ TextureIndex        symbol → OVL JSON map + lookup helper

cli/                     ovlextract single binary, CLI11-based
```

All extractors implement `IResourceExtractor`:

```cpp
class IResourceExtractor {
    virtual std::string_view name() const = 0;
    virtual ExtractResult extract(const OvlParser&, const ExtractContext&) = 0;
};

struct ExtractContext {
    std::filesystem::path                  output_dir;
    bool                                   overwrite = false;
    std::function<void(std::string_view)>  log;
    const TextureIndex*                    texture_index = nullptr;  // §6
    bool                                   auto_extract_textures = false;
};
```

The split is intentional: `core/` has **zero** UI / runtime dependencies and
can be embedded into other projects (Qt, command-line, web); `extractors/`
contains the per-resource decoding strategies.


## 2. SoundExtractor — `snd` → `.wav`

Per §6 of the format doc, each `snd` has an 80-byte header + 1-2 raw PCM
channel blobs at internal offsets.

What the extractor does:

1. Read the SidSoundHeader at `datapointer`.
2. Sanity check (`fmt == 1`, `channels ∈ {1, 2}`, `1000 ≤ samplerate ≤ 192000`).
3. Read `channel1_size` bytes from `channel1` (and same for channel2 if
   stereo). Interleave to `LRLRLR…` when stereo.
4. Write a minimal RIFF WAVE — `fmt ` (16 B) + `data` (raw PCM).
5. If `loop == 1`, append a `smpl` chunk per the WAV sampler-data spec:
   one forward-loop entry, start=0, end=`total_samples - 1`, infinite play
   count. Audacity / SoundForge / most DAWs round-trip it automatically.

Output: `<symbol>.wav` per `snd` linkedfile, mono or stereo PCM 16-bit (or
8-bit if the source is 8-bit).

CLI:
```bash
ovlextract --types sound --output-dir /tmp/wavs Sounds.common.ovl
```

Real-world numbers (`Sounds.{common,unique}.ovl`): 334 sounds total, 58
of them looping (ambient SFX: water, hums, lava, gears, hydraulics).


## 3. TextureExtractor — `ftx/tex/fts/ftt` → `.tga` + `.ovltex` + `.json`

Per §3 of the format doc, all FTX format codes (3–9) share the same
indexed8 + BGRA palette layout, so a single decode path handles them all.

What the extractor does for each linked file with tag `ftx`, `tex`, `fts`,
or `ftt`:

1. Locate the data block, read its full size (header + palette + small
   metadata) and dump it verbatim as `<symbol>.ovltex` — kept around so
   future RE on `tex` can be done offline without re-running the extractor.
2. Parse the FTX-style header (format, width, height, mipmap, pixel_off).
3. Sanity check dimensions (`0 < w,h ≤ 8192`); skip otherwise.
4. Read the 256-entry BGRA palette at `+0x40` of the header block.
5. Follow `pixel_internal_offset` to read `width × height` 1-byte indices.
6. Map each index through the palette to BGRA, with **alpha = 255** for all
   pixels (the 4th palette byte isn't a per-entry alpha; transparency is a
   per-sub-mesh `txs` shader concern, see §3.5 of the format doc).
7. Write `<symbol>.tga` (uncompressed, 32-bit, top-down origin).
8. Write `<symbol>.json` sidecar with parsed header fields.

Output per linked file:
- `<symbol>.tga`     — viewable in any tool
- `<symbol>.ovltex`  — raw bytes (for offline RE)
- `<symbol>.json`    — `{symbol, loader_tag, format_code, width, height,
                        mipmap_count, pixel_internal_offset, raw_block_size}`

CLI:
```bash
ovlextract --types texture --output-dir /tmp/textures Main.common.ovl
```

There's also a programmatic single-symbol entry point used by the auto-
texture pipeline (§6.2):

```cpp
bool TextureExtractor::extract_symbol(const OvlParser&,
                                      const std::string& symbol_lc,
                                      const ExtractContext&);
```

It case-insensitively matches the requested symbol against every
linkedfile and extracts just that one.

### 3.1 `tex` limitation

Despite being accepted by the tag filter, `tex`-wrapper texture decode is
**not implemented** — the wrapper's pointer chain to actual pixel data is
not yet RE'd. Running with `--types texture` on a `tex`-heavy OVL produces
the `.ovltex` raw dump + `.json` sidecar but no `.tga`. See §4 of the
format doc for the wrapper layout.


## 4. AtlasExtractor — `gsi` + parent `ftx`/`tex` → cropped `.tga`

Per §5 of the format doc, each `gsi` describes a rectangle within a parent
texture (referenced symbolically through the symbol-resolves table).

What the extractor does:

1. **Pass 1**: walk both sides of the OVL pair; for every texture loader
   (`ftx`/`tex`/`fts`/`ftt`), call `decode_ftx_at` to produce a
   `DecodedTexture { width, height, BGRA pixels }`. Tag `tex` is currently
   skipped (returns false), see §3.1 above. Store decoded textures in a
   map keyed by symbol.
2. **Pass 2**: walk both sides for `gsi` linked files. For each:
   - Read the texture-ref slot via `symbolresolves` (`gsi+4` then `gsi+0`).
   - Read the four-u32 rectangle at the coord-block offset (gsi+8).
   - Look up the parent texture in the map; skip with a log line if absent
     (this is the common case for `tex`-backed atlases).
   - Crop the rectangle (clamped to the texture's bounds) and write
     `<gsi_symbol>.tga`.

`AtlasExtractor`'s `decode_ftx_at` still applies the legacy `index 0 →
alpha = 0` chroma-key while `TextureExtractor`'s `decode_indexed8` no longer
does (see §3 above). This is deliberate for now: sprite atlases tend to use
the index-0-as-background convention, while standalone textures bound to
3D meshes do not. The discrepancy is worth flagging if you start trusting
alpha from atlas output for non-UI purposes.

Output: one `<sprite>.tga` per resolvable `gsi` linked file.

CLI:
```bash
ovlextract --types atlas --output-dir /tmp/sprites Extras.common.ovl
```

**Practical yield is low** until `tex` is decoded. Every atlas in the
shipping game uses `tex`, so the extractor mostly logs `gsi: texture 'X:tex'
not decoded` and produces a handful of sprites from rare `ftx`-backed
atlases.


## 5. ModelExtractor — `shs`/`mms` → `.obj` + `.mtl`

Per §7 and §8 of the format doc. The extractor handles both loader tags
but treats them very differently:

### 5.1 `shs` — full multi-material OBJ

For each `shs` linkedfile:

1. Read `vertex_count` / `index_count` from header `+0x18` / `+0x1C`.
2. Walk the sub-mesh table at header `+0x28`. For each descriptor, pull
   its local `vc` / `ic` / `verts_off` / `idx_off` from offsets
   `+0x18` / `+0x1C` / `+0x20` / `+0x24`.
3. Validate the sum of per-sub-mesh `vc`/`ic` equals the header totals.
4. Collect the SHS's material list from its `SymbolResolve` slice
   (`loadpointer == lf.loaderreference.internal_offset`), paired as
   consecutive `(ftx_symbol, txs_symbol)` — one pair per sub-mesh in order.
5. For each sub-mesh, read `vc` × 36-byte vertices (3f pos + 3f normal +
   u32 sentinel + 2f UV) and `ic` × u32 indices (sub-mesh-local).
6. Emit:
   - `<symbol>.obj` — one concatenated vertex/UV stream, then one `g`/`usemtl`
     group per sub-mesh with a per-sub-mesh index base offset.
   - `<symbol>.mtl` — one `newmtl` per unique sanitized ftx symbol. When a
     texture index is provided and the ftx symbol resolves, the material
     gets a `map_Kd <ftx>.tga` line — otherwise just a stub with `Kd 1 1 1`
     and a `# unresolved` comment so the file remains valid.

UV `v` is flipped on output (`vt v` = `1 - input_v`) to match standard
tooling conventions (Blender etc.).

Survey on a 40-OVL random sample: **75 / 76 (99 %)** of SHS files extract.
The one failure is an empty placeholder (`vc = ic = 0`). Sub-mesh
distribution: 29× single, 19× double, 20× triple, 7× quad.

### 5.2 `mms` — diagnostic only

Because position decode is unsolved (§7.4 of the format doc), MMS handling
is intentionally noisy and **skipped in bulk batches** (`side_loop` only
dispatches on `shs`). When `process_mms` is invoked manually it writes:

- `<symbol>.obj` — default (int8/127 stride 3) positions
- `<symbol>.<decoder>.obj` — one OBJ per candidate decoder (17 variants)
- Diagnostic dumps to stderr: full 40-byte MMS header (`MMS_HDR ...`) and
  first 48 bytes of the position stream (`POS_BYTES ...`)

so you can A/B-compare candidate position encodings against the topology
+ UV-correct mesh in any 3D viewer.

### 5.3 SHS survey mode

Setting environment variable `OVL_SHS_SURVEY=1` switches `process_shs` to
a CSV-on-stdout one-shot:

- Per linkedfile row: `side, ovlbase, symbol, vc, ic, w00..w24, p00..p24`,
  where `wNN` is the hex value of the u32 at `+0xNN*4` of the header and
  `pNN` is `R` if that offset appears in the relocations table else `.`
  (i.e. a quick "is this a pointer slot" map for the 100-byte header).
- A `SMTABLE` line listing the sub-mesh pointers, and `SMDESC` lines for
  the first 48 bytes of each sub-mesh descriptor.

Used during initial RE of the multi-material layout — kept around because
running with `OVL_SHS_SURVEY=1` on a new OVL collection quickly tells you
whether the same header conventions hold.


## 6. Cross-OVL texture resolution

A `shs` mesh and the `:ftx` it references usually live in **different**
OVLs — e.g. coaster pieces in `tracks/coasters/Track6/45medslopechain_data.unique.ovl`
reference `gigacoaster:ftx` from `tracks/coasters/Track6/Track6_Textures.common.ovl`.
To resolve these, the toolchain has a two-step "build then consume"
pattern.

### 6.1 `--build-index` — write the global symbol map

```
ovlextract --build-index <out.json> <Assets/>
```

Walks every `.common.ovl` under the given directory, parses each OVL pair,
and writes a sorted JSON map of every linked-file symbol → defining OVL.
See §9 of the format doc for the JSON layout.

Operational notes:
- The input must be a directory (the CLI errors out otherwise).
- Keys are lowercased (RCT3 symbol resolution is case-insensitive); the
  original case is preserved in the `symbol` field.
- First match wins on collision (commonly-duplicated textures across
  content packs).
- Duplicate count is reported on stdout for diagnostics.
- Full install: ~7 500 OVL pairs → ~53 000 indexed symbols → ~5.5 MB JSON
  in ~10 s cold-cache.

The parser is intentionally hand-rolled (`extractors/src/TextureIndex.cpp`)
to avoid a JSON dependency; it expects one entry per line, the format
emitted by `--build-index`.

### 6.2 `--texture-index` + `--auto-textures` — consume it

```
ovlextract --types model --texture-index idx.json \
    --auto-textures --assets-root <Assets/> \
    <Assets/.../some_data.unique.ovl>
```

`--texture-index` alone: model extraction writes `map_Kd <symbol>.tga` in
each per-shs `.mtl` for materials whose `:ftx` symbol resolves
(case-insensitive lookup); unresolved materials get a `# unresolved` comment
and no `map_Kd` line. The user is then expected to extract textures
separately and place the `.tga` files next to the `.obj`.

`--auto-textures` (requires `--assets-root`): for each resolved symbol, the
index entry is followed to the defining OVL,
`TextureExtractor::extract_symbol()` pulls just that one texture into the
model's output directory. `OvlParser` instances are cached in an
`AutoTextureState` across the whole `extract()` call so a single shared
texture OVL (e.g. `Track6_Textures`) is parsed only once even when many
shs reference it. Already-extracted symbols are tracked across the whole
batch to avoid duplicate disk writes.

End-to-end example (extract one coaster piece, complete with textures):

```bash
ovlextract --types model --texture-index idx.json --auto-textures \
    --assets-root /path/to/Assets \
    /path/to/Assets/tracks/coasters/Track6/45medslopechain_data.unique.ovl
```

Output directory contains `45medslopechain_HI.obj`, `…HI.mtl`, the same
for `_ME`/`_LO`, plus `gigacoaster.tga`, `chain.tga`, `struts.tga`,
`coaster_LO_textures02.tga` — all sourced from four different texture OVLs
and stitched together via the index.

Validation: Blender import + Cycles render shows textures bound to the
correct sub-meshes.


## 7. CLI reference (`ovlextract`)

```
ovlextract [options] <input>
```

`input` accepts:
- A path ending in `.common.ovl` or `.unique.ovl` (the other side is
  auto-resolved)
- A path ending in `.ovl`
- A basename without extension
- A **directory** — triggers recursive batch mode (every `.common.ovl`
  under the tree is processed, mirroring the directory layout in the output)

### Options

| Option | Description |
|---|---|
| `--dump` | Write a structural text dump (`OverlayDump_<name>_<side>.txt`) |
| `--list-loaders` | List loaders + linked files to stdout |
| `--build-index FILE` | Recursively scan a directory and write the global symbol → OVL JSON map to `FILE` |
| `--texture-index FILE` | Load the JSON map from a previous `--build-index` run (enables `map_Kd` emission) |
| `--assets-root DIR` | Root directory the texture index was built against (required with `--auto-textures`) |
| `--auto-textures` | When extracting models with `--texture-index`, also extract each referenced texture from its source OVL |
| `-t,--types TYPE` | Resource type to extract (`sound`, `texture`, `atlas`, `model`, `dump`, `all`). Repeatable. |
| `-o,--output-dir DIR` | Output directory (default: `./extracted/<basename>/` for single file, `./extracted/` for directory input) |
| `--overwrite` | Overwrite existing output files (default skip) |
| `-v,--verbose` | Verbose logging to stderr |
| `-V,--version` | Print version |

### Output file conventions

| Extractor | Files produced (per linked file) |
|---|---|
| `--dump` | `OverlayDump_<name>_common.txt` and `_unique.txt` |
| `--types sound` | `<symbol>.wav` per `snd` loader (mono or stereo PCM, optional `smpl` loop chunk) |
| `--types texture` | `<symbol>.ovltex` (raw block) + `<symbol>.json` (metadata) + `<symbol>.tga` if decode succeeds |
| `--types atlas` | `<symbol>.tga` per resolvable `gsi` (parent must be `ftx`, not `tex`) |
| `--types model` | `<symbol>.obj` + `<symbol>.mtl` per `shs`. With `--auto-textures`, also each referenced `<ftx>.tga` |
| `--build-index` | One JSON file mapping every lowercased symbol → `{symbol, ovl, side, tag}` |

### Worked examples

```bash
# Inspect an OVL: list its loaders and linked files
ovlextract --list-loaders path/to/Main.common.ovl

# Write a structural text dump (.txt) — equivalent to legacy "Dump OVL"
ovlextract --dump --output-dir /tmp/dump path/to/Main.common.ovl

# Extract all sounds → .wav files
ovlextract --types sound --output-dir /tmp/wavs Sounds.common.ovl

# Extract all textures → .tga + .ovltex + .json
ovlextract --types texture --output-dir /tmp/textures Main.common.ovl

# Combine multiple extractors
ovlextract --types sound --types texture --dump \
    --output-dir /tmp/everything --overwrite path/to/anything.common.ovl

# Build the cross-OVL symbol index once for a whole install
ovlextract --build-index ovl_index.json /path/to/Assets

# Extract a static mesh end-to-end (.obj + .mtl + sibling .tga from
# wherever each referenced texture lives)
ovlextract --types model \
    --texture-index ovl_index.json --auto-textures \
    --assets-root /path/to/Assets \
    /path/to/Assets/Scenery/Dice/Dice.unique.ovl
```

### Batch example (preserve directory layout)

```bash
ASSETS="$HOME/Library/Application Support/Steam/steamapps/common/RollerCoaster Tycoon 3 Complete Edition/RollerCoaster Tycoon 3 Platinum.app/Contents/Assets"
cd "$ASSETS"
find . -name '*.common.ovl' | while read f; do
    sub=$(dirname "$f" | sed 's|^./||')
    base=$(basename "$f" .common.ovl)
    out="$HOME/rct3-out/texture/$sub/$base"
    mkdir -p "$out"
    ovlextract --types texture --overwrite -o "$out" "$f"
done
```

Yields ~1 100 decoded TGAs plus the raw blocks for other format codes.

Or use the built-in recursive mode (since `e9e0cb8`) — pass a directory as
`input` and the layout is mirrored automatically:

```bash
ovlextract --types texture -o ~/rct3-out "$ASSETS"
```


## 8. Blender helper script

`scripts/blender_import_objs.py` imports a directory tree of `.obj` files
(produced by `--types model`) into a Blender scene, laying them out in a
square-ish grid so they don't pile up at the origin.

```bash
# Open Blender with everything imported
blender --python scripts/blender_import_objs.py -- out/static_models

# Headless: save to .blend instead of opening the UI
blender -b --python scripts/blender_import_objs.py -- out/static_models out/scene.blend

# Optional filtering
blender --python scripts/blender_import_objs.py -- out/static_models --high-only
blender --python scripts/blender_import_objs.py -- out/static_models --limit 200
```

- `--high-only`: skip multi-LOD `_LO` / `_ME` / `_ULOW` variants, keep only
  the high-detail mesh per asset.
- `--limit N`: pick `N` random files from the filtered set (useful for
  spot-checking a large extraction without loading the whole batch).

Uses the Blender 4.x `bpy.ops.wm.obj_import` operator with a fallback to
the legacy `bpy.ops.import_scene.obj` for older versions. Materials and
textures are picked up automatically because the script imports the
`.obj` (which references the sibling `.mtl` via `mtllib`), provided the
`.tga` files were placed next to the `.obj` (which `--auto-textures`
guarantees).
