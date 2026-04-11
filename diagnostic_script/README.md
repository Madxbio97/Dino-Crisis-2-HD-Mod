# Dino Crisis 2 — HD Mask Generator

A toolset for automatically generating high-resolution mask textures from upscaled backgrounds in Dino Crisis 2 (Classic Rebirth patch).

## Problem

Dino Crisis 2 uses pre-rendered backgrounds (320×240) with overlay masks (512×512) that create depth by covering the player character. When backgrounds are replaced with AI-upscaled HD versions, the original low-resolution masks cause visible seams and pixelation. Simply upscaling the masks doesn't work because their pixels must exactly match the corresponding background pixels.

## Solution

This toolset intercepts the game's rendering pipeline to record which mask texture belongs to which background and how each mask tile maps to screen coordinates. A Python script then uses this mapping to cut HD mask textures directly from the upscaled backgrounds, ensuring pixel-perfect alignment with zero seams.

## Requirements

- Dino Crisis 2 (PC) with [Classic Rebirth](https://github.com/AceVentura/ClassicRebirth) patch (ddraw.dll → D3D9 wrapper)
- MSVC x86 (Visual Studio Developer Command Prompt) for building the plugin
- Python 3.8+ with Pillow (`pip install Pillow`)
- **Game resolution must be set to 1280×960** in Classic Rebirth settings when collecting data

## Components

| File | Description |
|------|-------------|
| `dc2_diag.c` | ASI plugin — hooks D3D9 draw calls and logs background→mask tile mappings to `dc2_pairs.tsv` |
| `make_hd_masks.py` | Python script — reads the TSV and generates HD masks by cutting from upscaled backgrounds |
| `mask_overrides.tsv` | Optional manual corrections for misidentified pairs |

## Step 1: Build the Plugin

Open an **x86 Developer Command Prompt** and run:

```
cl /LD /O2 dc2_diag.c /link /OUT:dc2_diag.asi kernel32.lib user32.lib
```

Place `dc2_diag.asi` in the game directory alongside the executable.

## Step 2: Collect Data

1. **Set game resolution to 1280×960** (this is critical — the coordinate mapping depends on a consistent resolution).
2. Launch the game with the plugin loaded.
3. Play through all areas you want to generate HD masks for. Each room is recorded automatically when you enter it.
4. Press **Insert** at any time to flush collected data to `dc2_pairs.tsv`.
5. Data is also auto-flushed every 60 seconds.

The plugin generates:
- `dc2_pairs.tsv` — tab-separated file with background→mask tile mappings
- `dc2_diag.log` — debug log with frame/pair/cache statistics

## Step 3: Prepare Textures

Make sure you have:
- **Original texture dumps** in `dump/textures/` (e.g., `BA06487B_320x240.webp` for backgrounds, `F9F692E7_512x512.webp` for masks)
- **Upscaled HD backgrounds** in `hires/textures/` (e.g., `BA06487B_320x240.webp` at 1280×960 resolution)

The HD background filenames must match the original dump filenames (hash + original resolution).

## Step 4: Generate HD Masks

```bash
# Generate all detected masks
python make_hd_masks.py

# Test a single background
python make_hd_masks.py --test BA06487B

# Custom paths
python make_hd_masks.py --tsv dc2_pairs.tsv --dump dump/textures --hires hires/textures --out hires/textures
```

Output HD masks are saved to `hires/textures/` with the original filename (e.g., `F9F692E7_512x512.webp`), ready for the texture replacement plugin to load.

## Manual Overrides

In rare cases (usually during room transitions), the plugin may associate a mask with the wrong background. Create or edit `mask_overrides.tsv` to correct these:

```
# Format: bg_hash	mask_hash
526B109C	413FEA13
A11E80A0	4DEA6744
```

The Python script loads overrides automatically and applies them on top of auto-detected pairs.

## How It Works

1. **Plugin** hooks `SetTexture` and `DrawPrimitiveUP` in the D3D9 pipeline.
2. Backgrounds are identified as 320×240 textures drawn fullscreen with UV coordinates spanning 0–1.
3. Masks are identified as 512×512 textures drawn as multiple tile quads on top of the background.
4. Each tile's UV region (which part of the mask texture) and destination rectangle (where on screen) are recorded.
5. **Python script** uses these coordinates to map each mask pixel to its corresponding position in the HD background, then copies the HD pixels to create a seamless HD mask.

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `0 pairs` in log | Make sure you're in a game room (not menu/cutscene). Walk around to trigger mask rendering. |
| Wrong mask assigned to background | Add the correct mapping to `mask_overrides.tsv`. |
| Seams between mask tiles | Ensure all data was collected at the same resolution (1280×960). Re-collect if resolution was changed. |
| Plugin doesn't load | Verify the ASI loader is working. Check that `dc2_diag.asi` is in the game directory. |
| Slow performance | The plugin hashes textures on every draw call. This is expected during data collection — remove the plugin for normal gameplay. |

## Credits

Designed for use with the [Classic Rebirth](https://github.com/AceVentura/ClassicRebirth) DirectDraw→D3D9 wrapper for Dino Crisis 2.
