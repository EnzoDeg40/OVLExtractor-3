# OVLExtractor-3

**Cross-platform extractor for RollerCoaster Tycoon 3 `.ovl` files.**

A clean-room C++17 rewrite of [OVLExtractor-2](https://github.com/gamemodderz/OVLExtractor-2)
(C++/CLI .NET WinForms, Windows-only) targeting **Windows, Linux, and macOS** —
and going further: not just dumping the OVL structure but actually
**extracting resources** (sounds → `.wav`, textures → `.tga` + `.dds`)
that can be opened in standard tools.

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
  `.dds`, `.json` sidecars).
- Same parsing fidelity as the legacy reader, verified on **14 952 real OVL
  files** from a complete RCT3 installation (Main + all expansions).

## Status

- ✅ **Parser core**: all OVL versions 1, 4, 5 handled (v6 deferred — format
  incomplete in the legacy reader too).
- ✅ **CLI** (`ovlextract`): `--dump`, `--list-loaders`, `--types sound/texture`.
- ✅ **Sound extractor** (`snd` loader → standard 16-bit PCM WAV).
- ✅ **Texture extractor** (`ftx` FlexiTexture format-8 indexed8+palette → TGA).
- 🚧 **Texture extractor** (`tex` Texture loader and other ftx format codes):
  raw `.ovltex` dump only — format reverse-engineering pending.
- 🚧 **Qt GUI** (Phase 2 — not started).
- 🚧 **Model extractor** (`svd`/`mdl`/`mms` → `.obj`/`.gltf`): not started.

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
| `OVL_BUILD_GUI` | `OFF` | Reserved for Phase 2 Qt GUI |
| `OVL_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan in Debug builds |

## Usage

```bash
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
| `--types sound` | `<symbol>.wav` per `snd` loader (mono or stereo PCM 16-bit) |
| `--types texture` | `<symbol>.ovltex` (raw block), `<symbol>.json` (metadata), and `<symbol>.tga` if `format_code == 8` |

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
│   │   └── TextureExtractor.hpp
│   └── src/
├── cli/                        ovlextract — CLI11 single-header
├── tests/                      Catch2 unit + smoke tests
├── third_party/CLI11/          vendored single-header
├── cmake/                      build configuration helpers
└── docs/                       format notes
```

The split is intentional: `core/` has **zero** UI / runtime dependencies and
can be embedded into other projects (Qt, command-line, web), while
`extractors/` contains the per-resource decoding strategies.

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

### Sound (`snd`)

At `loaderreference.datapointer` lies an 80-byte header containing:

- `WAVE_FORMAT_PCM` (16 bytes): tag, channels, sample rate, byte rate,
  block-align, bits per sample
- 44 bytes of unknown metadata (volumes, mix params, …)
- `loop` (4 bytes, i32 at offset 60): boolean — 0 = one-shot, 1 = loop
- `channel1` / `channel1_size` / `channel2` / `channel2_size` — internal
  offsets to the raw PCM payload(s)

For stereo, the two channels are stored separately and we interleave them to
`LRLRLR…` to produce a standard stereo WAV.

When `loop == 1`, a standard `smpl` chunk is appended to the WAV with a
single forward loop covering the entire sample. Audacity, SoundForge, and
most DAWs read this automatically. Empirically 58/334 sounds in
`Sounds.common.ovl` are looping (ambient SFX: water, hums, lava, gears).

### FlexiTexture (`ftx`)

The 76-byte header at `loaderreference.datapointer` contains:

- offset 0: format code (`6`, `7`, `8`, …)
- offset 4: width
- offset 8: height
- offset 60: pointer to pixel data (in a different chunk)
- offset 64 onward (for `format == 8`): 256-entry RGBA palette (1024 bytes)

For `format == 8` (indexed 8-bit), pixels are 1 byte per pixel pointing into
the palette. Index 0 is treated as chroma-key transparent (common convention
in early-2000s palette textures).

Other format codes (7, 6, 5, 4, 3, 9) are written as raw `.ovltex` for further
reverse-engineering.

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

1. Decode `tex` (Texture) loader — header layout differs from `ftx`.
2. Decode `ftx` format codes other than 8 (DXT? RGB565? Indexed4?).
3. Model extractor (`svd`, `mdl`, `mms`) → `.obj` or `.gltf`.
4. Qt 6 GUI reproducing the legacy WinForms UX.
5. CI matrix (GitHub Actions: Linux + macOS + Windows).

PRs welcome. If you have format documentation from the RCT3 modding community,
adding it to `docs/OVL_FORMAT.md` is a great way to help.
