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

## 6. Next steps

1. Scenery chain in game-load order: `ftx` + `txs` → `shs` → `svd` → `sid`
   (+ `gsi` icon, `txt` name) — the minimal custom scenery object. Verify in
   the game, then round-trip: extract → glTF → re-create → load.
2. Then per need: `bsh`/`ban` (animated scenery), `sta`, `anr`, track types.
   Use the matching `Manager*.cpp` of libOVLng as the field reference.
3. v5.1 in the builder if a type needs it (22 game pairs use it).
