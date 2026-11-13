# libOVLng — what the community OVL *writer* gives us

Source: the RCT3 Importer (Jonwil → Belgabor), GPLv2, cloned read-only at
`../rct3-importer` (GitHub fork `chances/rct3-importer`). The writer library
is `RCT3 Importer/src/libOVLng` (≈15 k lines, "based on libOVL by Jonathan
Wilson"); struct definitions live in `RCT3 Importer/include/*.h`.
OVLExtractor-3 is GPLv2 too, so porting code with attribution is allowed.

Goal of this note: decide how OVLExtractor-3 grows from "extract" to
"extract **and re-create**".

## 1. Symbol hash — solved

`SymbolStruct2.hash` / `SymbolRefStruct2.hash` = **djb2 over the lower-cased
`name:tag`** (`cSymbol::calcSymbolNameHash`, `LodSymRefManager.cpp:124`):

```
h = 5381; for c in lower(name + ":" + tag): h = h*33 + c   (mod 2^32)
```

Checked on the Mac build: 12 390 / 12 390 `name:tag` strings found in
`ContentPacks/*` and `gui/*.common.ovl` have their djb2 present in the file.
This lifts the "new symbols are refused" limit of `scripts/ovl_text_pack.py`
(§18.1 of RCT3_OVL_FORMAT.md).

## 2. Container writing algorithm (`cOvl::Save`, `libOVLng.cpp`)

Chunk numbers below are libOVLng's, **0-based**; RCT3_OVL_FORMAT.md §18.1
counts from 1 (its "chunk 1" = chunk 0 here, "chunk 3" = chunk 2).

1. **String table** → common, chunk 0, one block (all symbol names).
2. **Assign** space for symbols, loaders and symbol references → chunk 2 of
   each file (common / unique).
3. Each type manager **Make**s its data blocks (`GetBlock(chunk, size)`), and
   registers every pointer field it writes with the relocation manager, and
   every cross-object link as a *symbol reference*.
4. Loader type numbers = managers in **alphabetical tag order**.
5. Symbols are **sorted** (v5): pointer symbols by loader number then hash;
   data symbols after them, by tag then hash. (v4: hash only; v1: case-
   insensitive name.)
6. **MakeOffsets**: blocks are laid out common chunks 0..8 then unique chunks
   0..8; a block's `reloffset` is the running sum — one address space shared
   by both files, so unique can point into common.
7. **Relocations**: each registered slot's in-memory pointer is rewritten to
   its `reloffset`; the slot's own `reloffset` goes into the fixup set of the
   file that holds it (written sorted, as a count + u32 list).
8. **Write** each file: header (`FGRK`, reserved 2, version, `16`, `0` if v5),
   references, loader headers, v5 per-loader `{index, count}`, per chunk
   `{count, count, sizes…}`, v5 extra-data table (`4 + n*14 + 4`, n,
   records, 0) or `{4, 0}`, `0`, block bytes, fixups, loader extra chunks,
   trailing `0` (v5).

`FilesAndLines` tails are never written — consistent with our finding that the
game ignores them.

### Why not just compile it

The code builds in memory with **real 32-bit pointers** stored in
`unsigned long` and converts them at save time
(`RelocationManager.cpp`). On macOS/x86-64 `unsigned long` and pointers are
8 bytes, so the structs and the relocation pass break. It also pulls in
boost, wx and a few `windows.h` bits. **Port the algorithm, not the code**:
blocks as byte buffers, pointers as `{block, offset}` handles, relocations as
a list of slots resolved at layout time.

## 3. Type coverage: libOVLng vs. OVLExtractor-3

| Tag | Name | We decode | libOVLng writes |
|---|---|---|---|
| txt | Text | ✅ (+ template writer) | ✅ |
| ftx, tex, flic, btbl, gsi | textures / icons | ✅ | ✅ |
| snd | Sound | ✅ | ✅ |
| shs, bsh, ban | meshes, skin, anim | ✅ | ✅ |
| svd, sid | scenery visual / item | ✅ | ✅ |
| spl, tks, trr, rit, ric | tracked rides | ✅ | ✅ |
| anr | AnimatedRide (flat ride) | ✅ | ✅ |
| sta, sat | Stall, SpecialAttraction | partial | ✅ |
| ter | TerrainType | ✅ | ✅ |
| txs | shader styles | doc only | ✅ (in ManagerCommon) |
| **ptd, qtd** | path / queue types | ❌ | ✅ |
| **cid, ced** | carried items (+extra) | ❌ | ✅ |
| **chg** | changing room | ❌ | ✅ |
| **mam** | ManifoldMesh | ❌ | ✅ |
| **wai** | WildAnimalItem | ❌ | ✅ |
| mms, fct, prt/ppg/phd/ssk | morph mesh, fonts, people | ✅ | ❌ |

So: almost everything we decode, libOVLng can also *write* — its managers are
a field-by-field spec of what each struct must contain for the game to
accept it. The reverse gaps (people, fonts, mms) are ours alone.

## 4. What the game's own files do (measured, Mac build, 7 476 pairs)

The writer in OVLExtractor-3 follows the game where it differs from
libOVLng. All of it checked by `ovl_roundtrip --rebuild` (§5):

| Item | Game | libOVLng |
|---|---|---|
| String table | common chunk 0 block 0; **no usable order** (insertion order of Frontier's tool) | sorted, unique |
| Table blocks | chunk 2 of each file starts `[symbols, loaders, symbol refs]`; the unique file always has all three (empty or not), the common drops an empty symbol-ref table | same three, in chunk 2 |
| Data blocks | wherever the type puts them (text: common chunk 0, ~16 KB blocks) | one blob per manager |
| Symbol order | v1 case-insensitive name; v4 hash; v5 loader tag (alphabetical) then hash, symbols without a loader last by tag then hash | same |
| v5 loader info | one `{loader type, symbols of that tag in the file}` per loader type, **types in alphabetical tag order** | `{index, count}` in index order |
| Symbol refs | grouped by loader; 49 pairs use an order the game's tool chose (creation order), not reproducible — irrelevant to the game | loader order |
| v5 extra table | `{count, 14-byte records, 0}` or `{0}` | same |
| After relocations | v4: one `0` word; then the extra-data chunks; then the `FilesAndLines` source map (not read by the game — dropped) | + a trailing `0` (v5) |

One oddity: the Mac port renamed `CheatPeepName_Atari:txt` to `…Xtari…` in
`Main.common.ovl` but kept the old hash (0xF0D5033F = djb2 of "Atari").

## 5. Status in OVLExtractor-3

| Piece | Where | Verified |
|---|---|---|
| `OvlImage`: lossless read / write of one file, v1 / v4 / v5.0 / v5.1 | `core/…/OvlImage.hpp` | `ovl_roundtrip <Assets>`: **14 952 / 14 952 files byte for byte** |
| `OvlBuilder`: pair from blocks + pointers, symbols, loaders, symbol refs | `core/…/OvlBuilder.hpp` | `ovl_roundtrip --rebuild <Assets>`: **7 403 / 7 476 pairs byte for byte** with the default rules; +49 when given the original symbol-ref order; 22 v5.1 pairs unsupported by the builder; 2 = the "Xtari" hash |
| `txt` writer | `core/…/TextOvl.hpp`, `ovlwrite text strings.json out` | reproduces `ContentPacks/ContentPack0001` byte for byte (minus tail); French gui rebuilt from scratch with 48 strings changed + 1 new symbol → **loaded by the game, main menu shows the new texts** (2026-11-13) |

`ovl_roundtrip --rebuild` keeps only what a type writer would supply (block
payloads, which fields are pointers, the string order) and lets the builder
regenerate the rest, so a match proves the container rules for every type in
the game — not only `txt`.

## 6. Scenery writer: ftx, shs, svd, sid (verified in the game, 2026-11-13)

`core/…/SceneryCodec.hpp` decodes and encodes the four types of a scenery
item. Each struct is kept as raw bytes with its pointer / symbol-ref fields
zeroed (unknown scalar fields survive untouched), the pointed-to data decoded
beside it; the encoders place data like the game (ftx struct in common
chunk 4, planes in common chunk 0; shs / svd / sid structs in unique chunk 2,
shs geometry in common chunk 4, sid squares in common chunk 2). Struct sizes:
ftx 36, frame 28; shs 0x38, mesh 40; svd 0x34 / 0x38 Soaked / 0x3C Wild, LOD
0x48; sid 0xD4 / 0xE4 (structure_version 1) / 0xEC (2), square 20.

`ovl_scenery_check <Assets>` runs, on every object of the game, (1) a
coverage check — every relocated or symbol-ref slot of the struct is one the
decoder consumed — and (2) decode → encode into a fresh pair → decode → equal:

| type | ok |
|---|---|
| ftx | 1 594 / 1 594 |
| shs | 16 306 / 16 306 |
| svd | 6 332 / 6 332 |
| sid | 5 880 / 5 994 (the 114 others are ride pieces with sounds: not supported yet) |

`ovlwrite scenery-clone` re-encodes a game item under a new name (optionally
re-tinting its textures and changing its price). Verified in the game:
the red **"Dé OVL3 (OVLExtractor-3)"**, cloned from the Vanilla Dice, is listed
under Scenery → Scenery items at its own price, previews and places in a
park. What it took — the rules the game enforces:

1. **Discovery**: the game walks `Style/` recursively and loads only files
   named `Style.common.ovl` (`FUN_100a8cb04`). A pair with any other name is
   never opened.
2. **StyleIndex**: a Style pair must define the data symbol `StyleIndex:int`
   (0 Vanilla, 1 Wild West, 2 Spooky, 3 Space, 4 Adventure, 5 Atlantis,
   6 Island Paradise, 7 Jungle, 8 Prehistoric). Without it the style is not
   registered and its items are not listed. A file that also defines
   `StyleInternalName:txt` creates a theme (with `StyleName`, `StyleIcon`,
   `FenceName`, `MapColourRed/Green/Blue:flt`); without it the items are added
   to the theme of that index (as `Vanilla/CapeTyphoon` and `Vanilla/Safari`
   do). The old Importer wrote only `StyleIndex`.
3. **File references**: a symbol that lives in another OVL (here
   `DiceIcon:gsi` in `Style/SceneryIcons`) only resolves if the pair lists
   that file in its references — relative, without extension, **with
   backslashes** (`..\..\SceneryIcons`). Otherwise the item is silently
   dropped from the menus.
4. **Two pairs**: the sid's `ovl_path` names the pair holding the visual
   (svd, shs, ftx), which the game loads when the item is used. It must not
   be the Style pair itself: selecting the item then stops the game with
   `Resource_SymbolSource::operator= Detected cycle`. So: `Style.common.ovl`
   (sid, name txt, StyleIndex) + `<Name>.common.ovl` (visual), as the game's
   own files are split.

### 6.1 Path types: `ovlwrite path-clone` (verified in the game, 2026-11-13)

```bash
sips -s format tga photo.jpg --out photo.tga
ovlwrite path-clone --assets "$ASSETS" --from Asphalt --name Enzo --title "Enzo" \
    --image photo.tga --crop 480,950,2640
```

writes `Path/Enzo/` (24 pairs): the game then lists **"Enzo : €12"** among
the path types and lays it in a park. What the game does and what the
command relies on:

1. **Discovery**: the game lists the folders of `Path\` and, for each folder
   `X`, loads `Path\X\X_Stub` (ptd + one sid per piece) and
   `Path\X\X_Texture` (the ground pages) — decompile `FUN_1006108bc` and
   its callees. No file of the game has to change; a new folder is enough.
2. **Cloning** (`core/…/PairClone.hpp`, the `ovl_roundtrip --rebuild` code
   moved to the library, with hooks): every string that contains `<From>`
   is renamed — symbols the folder defines, piece names in the ptd, the
   sids' `ovl_path`, tex names, file references (`Asphalt_Scenery` →
   `Enzo_Scenery`). Symbols the folder does not define stay the game's
   (`Icon_PathType_Asphalt:gsi` in `Path/PathIcons`, `BasicPath:rcg`, the
   `SceneryItem_Asphalt_*:txt` piece names, the `pathasphalt` support type),
   except the path's name `Name_PathType_<From>:txt`, written into the stub
   as a new `txt` with `--title`.
3. **Textures** (`core/…/ImageCodec.hpp`):
   - On flat ground the path is drawn with the two **ground pages**
     (`texture_a` / `texture_b` of the ptd: `tex` + inline v2 `flic`, DXT3
     256², 8 mip levels, stored bottom-up). The colours are replaced by the
     picture (page B tinted), the alpha mask of each level (the tile
     shapes) is kept.
   - The 3D pieces (raised paths, slopes, edges) sample the `ftx` sheet
     `Asphaltsheet1` (256², indexed): its walking surface is u 0–0.5,
     v 0.5–1 (up-facing vertices of `Asphalt_Flat`), where the picture goes;
     the quadrant above gets a tinted copy; the sheet is re-quantised to
     256 colours (median cut).
4. The four variants of each piece shape all name the same piece, as in
   the game's paths; the icon is the source path's.

## 7. Next steps

1. From scratch instead of cloned: ftx from an image (quantisation is in
   `ImageCodec` since the path writer),
   shs from glTF / OBJ (the inverse of `--types scenery`), a minimal sid
   built from parameters, an icon (gsi + tex/flic/btbl) of our own.
2. Then per need: `bsh`/`ban` (animated scenery), `sta`, `anr`, track types,
   sid sounds. Use the matching `Manager*.cpp` of libOVLng as the reference.
3. v5.1 in the builder if a type needs it (22 game pairs use it).
