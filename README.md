# OVLExtractor-3

**Cross-platform extractor for RollerCoaster Tycoon 3 `.ovl` files.**

A clean-room C++17 rewrite of [OVLExtractor-2](https://github.com/gamemodderz/OVLExtractor-2)
(C++/CLI .NET WinForms, Windows-only) targeting **Windows, Linux, and macOS** —
and going further: not just dumping the OVL structure but actually
**extracting resources** (sounds → `.wav`, textures → `.tga`, static meshes
→ `.obj` + `.mtl` + `.tga`) that can be opened in standard tools.

## Why a v3?

`OVLExtractor-2` is a great inspector for the RCT3 OVL binary format, but it has
two practical limitations:

1. **Windows-only**: built on C++/CLI + WinForms + .NET Framework 4.5, it cannot
   compile or run on Linux or macOS.
2. **Inspector, not extractor**: it produces a textual hex dump useful for
   format reverse-engineering, but does not output usable resource files.

This project addresses both:

- 100% portable C++17 + CMake — builds on AppleClang, GCC 11+, Clang 14+, and
  MSVC 2022.
- Real resource extraction with file formats anyone can open (`.wav`, `.tga`,
  `.obj` + `.mtl`, `.json` sidecars).
- Same parsing fidelity as the legacy reader, verified on **14 952 real OVL
  files** from a complete RCT3 installation (Main + all expansions).

## Status

- ✅ **Parser core**: all OVL versions 1, 4, 5 handled (v6 deferred — format
  incomplete in the legacy reader too).
- ✅ **CLI** (`ovlextract`): `--dump`, `--list-loaders`, `--types
  sound/texture/atlas/model`, `--build-index`, `--texture-index`,
  `--auto-textures`, recursive directory input.
- ✅ **Sound extractor** (`snd` loader → standard PCM WAV, with a `smpl`
  loop chunk for the 58 / 334 ambient-SFX entries that are looping).
- ✅ **Texture extractor** (`ftx` FlexiTexture, all format codes 3–9):
  indexed8 + BGRA palette → TGA, with raw `.ovltex` + `.json` sidecar.
- 🚧 **Texture extractor** (`tex` DXT compressed): 110 / 121 single-tex
  OVLs decode to TGA via the trailing-data DXT1 layout (≈ 17 % of all 665
  `tex` symbols). DXT3/5 + multi-`tex` OVL v5 layouts still open
  (see `docs/RCT3_OVL_FORMAT.md` §4).
- ✅ **Static mesh extractor** (`shs` → `.obj` + `.mtl` + `.tga`): full
  multi-material sub-mesh decoding, 99 % success on a 40-OVL random sample.
  Cross-OVL texture binding resolves via the global symbol index built by
  `--build-index`; `--auto-textures` pulls referenced `.tga` files into the
  model's output directory in one pass.
- 🚧 **Animated mesh extractor** (`mms` → `.obj`): topology + UVs decode
  correctly; vertex positions still broken across all 17 candidate decoders.
- 🚧 **Atlas extractor** (`gsi` → cropped `.tga`): works for the rare
  `ftx`-backed atlas; blocked on `tex` decode for the typical case.
- ✅ **Qt 6 GUI** (`ovlextract_gui`): file picker, tree of resources grouped
  by category (textures / models / sounds / atlas / other), texture preview,
  embedded OpenGL viewer for shs meshes, "open externally" playback for snd
  items, and bulk "Extract all to folder…". Off by default — pass
  `-DOVL_BUILD_GUI=ON`.

For the full file-format reverse-engineering notes, see
[docs/RCT3_OVL_FORMAT.md](docs/RCT3_OVL_FORMAT.md). For extractor
implementations, CLI flags and the Blender import helper, see
[docs/EXTRACTORS.md](docs/EXTRACTORS.md).

## Build

Requires CMake ≥ 3.21 and a C++17 compiler.

```bash
git clone <this-repo>
cd OVLExtractor-3
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Build options

| Option | Default | Effect |
|---|---|---|
| `OVL_BUILD_CLI` | `ON`  | Build the `ovlextract` CLI binary |
| `OVL_BUILD_TESTS` | `ON`  | Build Catch2 unit tests |
| `OVL_BUILD_GUI` | `OFF` | Build the `ovlextract_gui` Qt 6 desktop app — needs Qt 6.5+ (Widgets / OpenGL / OpenGLWidgets). On macOS with Homebrew: `cmake -B build -DOVL_BUILD_GUI=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"` |
| `OVL_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan in Debug builds |

## Usage

```bash
# Build a global symbol → OVL path index over an install tree (one-shot, ~10s
# for 7.5k pairs). Used to resolve cross-OVL texture references when
# generating MTL files for shs models.
./build/cli/ovlextract --build-index ovl_index.json /path/to/Assets

# Extract a static mesh end-to-end (.obj + .mtl + sibling .tga files pulled
# from whichever OVL declares each referenced texture)
./build/cli/ovlextract --types model \
    --texture-index ovl_index.json --auto-textures \
    --assets-root /path/to/Assets \
    /path/to/Assets/Scenery/Dice/Dice.unique.ovl

# Inspect an OVL: list its loaders and linked files
./build/cli/ovlextract --list-loaders path/to/Main.common.ovl

# Write a structural text dump (.txt) — equivalent to legacy "Dump OVL"
./build/cli/ovlextract --dump --output-dir /tmp/dump path/to/Main.common.ovl

# Extract all sounds (snd) → .wav files
./build/cli/ovlextract --types sound --output-dir /tmp/wavs Sounds.common.ovl

# Extract all textures (ftx) → .tga + .ovltex + .json
./build/cli/ovlextract --types texture --output-dir /tmp/textures Main.common.ovl

# Combine multiple extractors
./build/cli/ovlextract --types sound --types texture --dump \
    --output-dir /tmp/everything --overwrite path/to/anything.common.ovl
```

The `input` argument accepts:
- a path ending in `.common.ovl` or `.unique.ovl` (the other side is auto-resolved)
- a path ending in `.ovl`
- a basename without extension

### Output files

| Extractor | Files produced |
|---|---|
| `--dump` | `OverlayDump_<name>_common.txt` and `_unique.txt` |
| `--types sound` | `<symbol>.wav` per `snd` loader (mono or stereo PCM, plus `smpl` chunk if loop=1) |
| `--types texture` | `<symbol>.ovltex` (raw block), `<symbol>.json` (metadata), and `<symbol>.tga` (decoded indexed8+palette — all format codes 3–9 supported) |
| `--types atlas` | `<symbol>.tga` per `gsi`, cropped from parent texture (most parents are `tex` → blocked until `tex` decode lands) |
| `--types model`   | `<symbol>.obj` + `<symbol>.mtl` per `shs` static mesh, sub-meshes as `g`/`usemtl` groups; with `--texture-index` the `.mtl` gets `map_Kd` lines, with `--auto-textures` the referenced `.tga` files are pulled in too |
| `--build-index`   | A JSON file mapping every linked-file symbol (lowercased) to its defining OVL path, side, and loader tag — used for cross-OVL texture lookups |

### Batch example

Extract every texture from a full RCT3 installation, preserving the directory
structure (the example below uses the macOS Steam install path):

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

On a typical installation this yields ~1 100 decoded TGA textures plus the
raw blocks for other format codes.

## Architecture

```
OVLExtractor-3/
├── core/                       libovlcore — pure C++17 parser
│   ├── include/ovl/
│   │   ├── BinaryReader.hpp    portable replacement for fopen_s + FILE*
│   │   ├── Endianness.hpp      little-endian fixed-width readers
│   │   ├── OvlHeader.hpp       header structs (V1/V4/V5/V6 variants)
│   │   ├── OvlTypes.hpp        POD types (Reference, Loader, Block, …)
│   │   ├── OvlData.hpp         aggregate for one side (common / unique)
│   │   ├── OvlParser.hpp       parsing + lookup API
│   │   ├── SFStructs.hpp       RCT3-specific resource layouts
│   │   └── Error.hpp           OvlError exception type
│   └── src/                    BinaryReader.cpp, OvlParser.cpp
├── extractors/                 libovlextractors — strategy per loader tag
│   ├── include/ovl/extract/
│   │   ├── IResourceExtractor.hpp
│   │   ├── DumpExtractor.hpp
│   │   ├── SoundExtractor.hpp
│   │   ├── TextureExtractor.hpp
│   │   ├── AtlasExtractor.hpp
│   │   ├── ModelExtractor.hpp
│   │   └── TextureIndex.hpp
│   └── src/
├── cli/                        ovlextract — CLI11 single-header
├── gui/                        ovlextract_gui — Qt 6 Widgets desktop app
│   └── src/
│       ├── main.cpp
│       ├── MainWindow.{hpp,cpp}     menu, tree, stacked detail pages
│       ├── OvlInventory.{hpp,cpp}   group linked-files by category
│       ├── TexturePreview.{hpp,cpp} BGRA-TGA decoder + QLabel preview
│       └── MeshViewer.{hpp,cpp}     QOpenGLWidget viewer for .obj
├── scripts/                    blender_import_objs.py (auxiliary tooling)
├── tests/                      Catch2 unit + smoke tests
├── third_party/CLI11/          vendored single-header
├── cmake/                      build configuration helpers
└── docs/                       RCT3_OVL_FORMAT.md, EXTRACTORS.md
```

The split is intentional: `core/` has **zero** UI / runtime dependencies and
can be embedded into other projects (Qt, command-line, web), while
`extractors/` contains the per-resource decoding strategies. The CLI and GUI
are independent executables linking those two libraries — building one does
not require the other.

### Running the GUI

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOVL_BUILD_GUI=ON \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build build --target ovlextract_gui -j

./build/gui/ovlextract_gui                       # empty window, File → Open
./build/gui/ovlextract_gui path/to/Main.common.ovl   # opens that OVL on start
```

The window shows a tree on the left grouped by category. Click a leaf:

- **Texture** (ftx/tex/fts/ftt) → image preview pane (only ftx + DXT1 `tex`
  decode currently; others fall back to a status message).
- **3D Model** (shs) → embedded OpenGL viewer; left-drag to rotate, scroll
  to zoom. Animated meshes (mms) are shown but won't render (positions WIP).
- **Sound** (snd) → Play button opens the extracted .wav in the OS-default
  audio player via QDesktopServices (no QtMultimedia dep needed).
- Anything else shows the symbol / loader metadata only.

`File → Extract all to folder…` runs every extractor in one go, equivalent
to `ovlextract --types all -o <folder> file.ovl`.

## Cross-platform substitutions vs the legacy reader

| Legacy (OVLExtractor-2) | OVLExtractor-3 |
|---|---|
| `fopen_s` + `FILE*` | `std::ifstream` wrapped in `BinaryReader` (RAII) |
| `Windows.h`, `vcclr.h` | removed |
| `unsigned long` (4 bytes on Win32, 8 on Linux) | `std::uint32_t` everywhere |
| `fread(&struct, sizeof, 1, ovl)` | field-by-field `read_u32()` / `read_u16()` |
| `Debug::WriteLine`, `gcnew String` | `std::cerr`, `std::string` |
| `System::Windows::Forms` (WinForms) | (Phase 2: Qt 6) |
| MSVC `#pragma pack` / alignment | banned; never read whole structs raw |
| `.vcxproj` (Visual Studio 2012) | CMake (≥ 3.21) |

## Format notes

The reverse-engineered file format is documented in
[docs/RCT3_OVL_FORMAT.md](docs/RCT3_OVL_FORMAT.md) — container structure,
loader table, FTX / TEX / GSI / SND / MMS / SHS payload layouts, and the
quirks/open items we hit along the way.

How the extractors turn that into `.wav` / `.tga` / `.obj` / `.mtl` (plus
the cross-OVL texture pipeline and the Blender import helper) lives in
[docs/EXTRACTORS.md](docs/EXTRACTORS.md).

## Testing

```bash
ctest --test-dir build --output-on-failure          # all tests
./build/tests/ovl_tests "[endianness]"              # filter by tag
./build/tests/ovl_tests "[real]"                    # tests that touch real OVLs
```

Tests tagged `[real]` are skipped automatically if the RCT3 Steam install
path is not present (CI / clean dev environments).

Smoke test on a real installation:

```bash
ASSETS="$HOME/Library/Application Support/Steam/steamapps/common/RollerCoaster Tycoon 3 Complete Edition/RollerCoaster Tycoon 3 Platinum.app/Contents/Assets"
find "$ASSETS" -name '*.common.ovl' | xargs -I{} ./build/cli/ovlextract --list-loaders {} > /tmp/batch.log 2>&1
echo "Errors: $(grep -c error /tmp/batch.log)"      # expected: 0
```

## Credits & License

- Format research and the original C++/CLI implementation:
  [gamemodderz/OVLExtractor-2](https://github.com/gamemodderz/OVLExtractor-2)
- This cross-platform rewrite: see commit history.

The OVL format is RCT3's proprietary container; this project only reads
files the user already owns through their RCT3 installation. No game assets
are bundled.

License: same as the upstream OVLExtractor-2 (see `LICENSE`).

## Contributing

The biggest open work items, in priority order:

1. Decode `tex` (Texture) loader — wrapper structure is known (see
   [RCT3_OVL_FORMAT.md §4](docs/RCT3_OVL_FORMAT.md#4-tex-wrapper-format-partially-red-not-decoded))
   but the pointer chain to the actual indexed pixel data is not yet
   located. Unlocks ~3 600 atlas sprites.
2. `mms` (animated mesh) vertex position decode — 17 candidate decoders
   tried, none yield coherent geometry. See [§7.4](docs/RCT3_OVL_FORMAT.md#74-why-positions-dont-work).
3. `txs` shader semantics — enrich `.mtl` output with alpha/spec/reflection
   flags per sub-mesh.
4. OVL header version 6 — currently parser warns and continues.
5. Polish the Qt 6 GUI: cross-platform launcher (`brew install qtbase` /
   `apt install qt6-base-dev` / vcpkg notes), DXT3/5 texture preview once
   the decoder lands, optional texture-mapped 3D viewer.
6. CI matrix (GitHub Actions: Linux + macOS + Windows).

PRs welcome. If you have format documentation from the RCT3 modding community,
adding it to [docs/RCT3_OVL_FORMAT.md](docs/RCT3_OVL_FORMAT.md) is a great way to help.
