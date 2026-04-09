# Dino Crisis 2 — DX9 Texture Dumper & Replacer

An ASI plugin for dumping and replacing **all** textures in Dino Crisis 2 (Sourcenext) with the Classic Rebirth patch.

Intercepts DX9 calls `SetTexture` and `EndScene` via detour hooks at the `d3d9.dll` level, ensuring compatibility with any version of Classic Rebirth.

## Features

- **Dump** all game textures in WebP format (backgrounds, masks, sprites, 3D textures, UI)
- **Replace** textures from `hires\textures\` — arbitrary resolution supported
- **Auto-detect room changes** — two-level cache with quick hashing prevents texture bleed from previous scenes
- **Correct alpha** — backgrounds (320×240) are saved as fully opaque, masks and sprites preserve transparency

## Requirements

- **Dino Crisis 2** (Sourcenext, Japanese edition)
- **Classic Rebirth** (`ddraw.dll`) — DX9 wrapper for the game
- **ASI Loader** (`dinput.dll`) — e.g. Ultimate ASI Loader
- **libwebp.dll** — WebP encoding/decoding library (from TeamX mod or downloaded separately)

## Installation

1. Make sure Classic Rebirth (`ddraw.dll`) is installed and the game runs
2. Place the following files in the same folder as `Dino2.exe`:
   - `dino2hd_ext.asi`
   - `dinput.dll` (ASI Loader)
   - `libwebp.dll`
3. Launch the game

## File Structure

```
Dino2.exe
ddraw.dll              ← Classic Rebirth
dinput.dll             ← ASI Loader
dino2hd_ext.asi        ← this plugin
libwebp.dll            ← WebP codec
dino2hd_ext.log        ← log (created automatically)
dump/
  textures/
    A3F7B210_320x240.webp   ← dumped textures
    ...
hires/
  textures/
    A3F7B210_320x240.webp   ← replacements (same filename)
    ...
```

## Usage

### Dumping Textures

1. Launch the game with the plugin installed
2. Load a save, walk around the locations
3. Textures are automatically saved to `dump\textures\`
4. Filename format: `<HASH>_<W>x<H>.webp`, where HASH is the FNV-1a hash of the texture content

### Replacing Textures

1. Copy the desired file from `dump\textures\` to `hires\textures\` (filename must match exactly)
2. Edit the file in an image editor (GIMP, Photoshop, etc.)
3. Save as WebP (lossless recommended)
4. Restart the game — the replacement will be applied automatically

> **Replacement size can differ from the original.** The plugin creates a new DX9 texture of the required size. For example, an original 320×240 background can be replaced with a 1280×960 image.

### Configuration

Modes are set in the source code (requires recompilation):

```c
static int g_dump    = 1;  // 1 = dump textures
static int g_replace = 1;  // 1 = replace from hires\textures\
```

## Building from Source

### MSVC (Visual Studio)

Open **x86 Native Tools Command Prompt**:

```cmd
cl /LD /O2 dino2hd_ext.c /link /OUT:dino2hd_ext.asi kernel32.lib user32.lib
```

### MinGW (MSYS2)

In **MSYS2 MINGW32** terminal:

```bash
pacman -S mingw-w64-i686-gcc   # once
i686-w64-mingw32-gcc -shared -O2 -o dino2hd_ext.asi dino2hd_ext.c -lkernel32 -luser32
```

> **Important:** 32-bit (x86) only. The game is 32-bit.

## Texture Types

| Size | Type | Alpha in dump |
|------|------|---------------|
| 320×240 | Pre-rendered background | Opaque (alpha = 255) |
| 256×256, 128×128, ... | 3D textures, masks | Original from pixel data |
| Other | Sprites, UI | Original from pixel data |

## How It Works

1. On load, the ASI creates a temporary DX9 device to obtain the addresses of `EndScene` and `SetTexture` functions inside `d3d9.dll`
2. Installs **detour hooks** (overwriting the first bytes of each function with a `JMP`) — this intercepts calls through any device, including the one created by Classic Rebirth
3. On each `SetTexture` call:
   - A **quick hash** (3 pixel rows) is computed to detect content changes
   - If content has changed — a **full hash** is computed and a replacement is looked up
   - Results are cached for 30 frames
4. Replacements are stored in a separate `hash → DX9 texture` cache and reused across frames

## Troubleshooting

- **Game won't start:** verify that `dinput.dll` is an ASI Loader, not the original DirectInput file
- **Log is empty:** ASI Loader is not loading the plugin. Make sure the file has the `.asi` extension
- **`frames=0` in log:** Classic Rebirth did not create a DX9 device. Verify that `ddraw.dll` is Classic Rebirth
- **Textures not dumping:** check that `libwebp.dll` is present in the game folder
- **Replacement not applied:** the filename in `hires\textures\` must **exactly** match the one in `dump\textures\`

## License

Free to use. Created for the Dino Crisis 2 modding community.

## Credits

- **TeamX** — original HD Mod for backgrounds and masks, libwebp
- **Classic Rebirth** — DX9 wrapper for the Sourcenext version
