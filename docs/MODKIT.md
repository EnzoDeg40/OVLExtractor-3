# Making RCT3 mods with `rct3mod`

`rct3mod` turns a folder with a `mod.toml` and a few pictures into game files
and installs them. No OVL knowledge needed: each piece of content starts from
something the game already has (the *base*) and changes what you describe —
name, texture, icon, price. Meshes, supports, sounds and the rest come from
the base.

RCT3 has no code-mod loader: the game builds its menus by scanning
`Assets/Path/*`, `Assets/Style/**/Style.common.ovl`… A mod is therefore new
files in new folders; the game's own files are never touched.

## Quick start

```bash
export RCT3_ASSETS=".../RollerCoaster Tycoon 3 Platinum.app/Contents/Assets"

rct3mod new my-paths          # my-paths/mod.toml, a starter manifest
cp ~/Pictures/bricks.jpg my-paths/texture.jpg
$EDITOR my-paths/mod.toml     # texture = "texture.jpg", name, title…
rct3mod build my-paths        # -> my-paths/build/Assets/ (check it builds)
rct3mod install my-paths      # copies into $RCT3_ASSETS, restart the game
rct3mod list
rct3mod uninstall my-paths    # removes exactly what install copied
```

`--game <Assets>` replaces `$RCT3_ASSETS` on any command.
`rct3mod build --preview <dir>` also writes the textures it made as TGA.

## mod.toml

```toml
[mod]
id = "enzo"                  # letters, digits, - and _; names the install
name = "Enzo's paths"
version = "1.0.0"
author = "Enzo"

[[path]]                     # a path type (repeat the table for more)
name = "Enzo"                # letters, digits, _ : folder Path/Enzo/ and symbol names
title = "Enzo"               # shown in the game (any UTF-8), default: name
base = "Asphalt"             # a folder of Assets/Path/ to start from
texture = "photo.jpg"        # TGA, PNG, JPEG or BMP, relative to mod.toml
crop = [480, 950, 2640]      # square [x, y, size] of the picture (default: centred)
icon = "face.png"            # menu icon picture (default: texture)
icon_crop = [560, 1200, 1750]

[[scenery]]                  # a scenery item (repeat for more)
name = "RedDice"
title = "Dé rouge"
visual = "Style/Vanilla/Scenery/Dice/Dice.common.ovl"  # game pair with the svd
svd = "Dice"
style = "Style/Vanilla/Style.common.ovl"              # game pair with the sid
sid = "Dice"
folder = "Style/Custom/enzo/RedDice"  # default: Style/Custom/<mod id>/<name>
theme = 0                    # 0 Vanilla, 1 Wild West, 2 Spooky, 3 Space,
                             # 4 Adventure, 5 Atlantis, 6 Island Paradise,
                             # 7 Jungle, 8 Prehistoric
tint = [220, 30, 30]         # recolour the item's textures (optional)
cost = 40                    # price (optional)
```

Use `ovlextract` or the extraction map (`site/`) to find bases: path types
are the folders of `Assets/Path/`; for scenery, the `svd` / `sid` names and
their pairs are listed per item.

## What each content type makes

| Table | Writes | Details |
|---|---|---|
| `[[path]]` | `Path/<name>/`: the 24 pairs of the base renamed, plus `<name>Icons` | the picture on the ground pages and on the 3D pieces' sheet, `title` in the menus, the icon in the path menu — `docs/LIBOVLNG_WRITER.md` §6.1 |
| `[[scenery]]` | `<folder>/<name>` (visual) + `<folder>/Style` (item) | one item per folder; same object, new name, tint and price — §6 |

## Install rules

- `install` builds first, then copies `build/Assets/*` into the game and
  records each file in `Assets/.rct3mod/<id>.files`.
- A file that already exists and was not installed by this mod stops the
  install: the game's files and other mods' files are never overwritten.
- Installing a mod again replaces its previous version.
- `uninstall` removes the recorded files and the folders they leave empty.
- The game reads its files at start-up: restart it after `install`.

## For C++ users

The same thing as a library (`modkit/`, target `rct3modkit`):

```cpp
#include "rct3mod/Mod.hpp"

rct3mod::PathType p;
p.name = "Brick"; p.title = "Brick path"; p.texture = "bricks.png";
rct3mod::build_path_type(p, game_assets, out_assets);

auto mod = rct3mod::load_mod("my-paths");
rct3mod::install_mod(mod, game_assets);
```

The builders sit on `ovlcore` (`core/`): `PairView` to read a pair,
`clone_pair` + `CloneHooks` to re-encode one with renamed symbols and patched
data, `SceneryCodec` / `TextOvl` / `ImageCodec` for the object types and the
pixels. A new content type is one more builder in `modkit/src/` plus its
table in `Mod.cpp`.

## Layout of the project

| Folder | Target | Role |
|---|---|---|
| `core/` | `ovlcore` | the OVL format: read, write, clone, codecs |
| `extractors/` | `ovlextractors` | OVL → files (textures, glTF, sounds, JSON) |
| `modkit/` | `rct3modkit`, `rct3mod` | content builders, mod.toml, install |
| `cli/` | `ovlextract`, `ovlwrite`, checks | tools on top of the libraries |
