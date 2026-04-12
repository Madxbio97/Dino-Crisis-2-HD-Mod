"""
Dino Crisis 2 — HD Mask Generator v2
Uses UV/dst coordinate mappings from TSV to cut HD masks from upscaled backgrounds.
No pixel matching needed — coordinates tell us exactly where each mask tile comes from.

Usage:
  python make_hd_masks.py [--dump dump/textures] [--hires hires/textures] [--tsv dc2_pairs.tsv]
  python make_hd_masks.py --test BA06487B

Requirements: pip install Pillow
"""

import os
import argparse
from collections import defaultdict
from PIL import Image
import numpy as np

def load_texture(folder, hash_str):
    for f in os.listdir(folder):
        if f.upper().startswith(hash_str.upper()):
            return Image.open(os.path.join(folder, f)).convert("RGBA"), f
    return None, None

def parse_tsv(tsv_path):
    rows = []
    with open(tsv_path, 'r') as f:
        f.readline()  # header
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) < 13:
                continue
            rows.append({
                'bg': parts[0], 'overlay': parts[1],
                'ow': int(parts[2]), 'oh': int(parts[3]), 'order': int(parts[4]),
                'dx0': float(parts[5]), 'dy0': float(parts[6]),
                'dx1': float(parts[7]), 'dy1': float(parts[8]),
                'u0': float(parts[9]), 'v0': float(parts[10]),
                'u1': float(parts[11]), 'v1': float(parts[12])
            })
    return rows

def find_mask_pairs(rows):
    """Find 512x512 overlays unique to one background."""
    overlay_bgs = defaultdict(set)
    for r in rows:
        overlay_bgs[r['overlay']].add(r['bg'])

    masks = {}
    for r in rows:
        ov = r['overlay']
        if r['ow'] == 512 and r['oh'] == 512 and len(overlay_bgs[ov]) == 1:
            bg = r['bg']
            if bg not in masks:
                masks[bg] = ov
    return masks

def load_overrides(path):
    """Load manual bg->mask overrides from TSV file.
    Format: bg_hash<TAB>mask_hash (one pair per line, # = comment)"""
    overrides = {}
    if not os.path.exists(path):
        return overrides
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) >= 2:
                overrides[parts[0].upper()] = parts[1].upper()
    return overrides

def get_tiles_for_pair(rows, bg_hash, mask_hash, mask_only=False):
    """Get all draw call tiles. If mask_only, match by overlay hash regardless of bg."""
    tiles = []
    for r in rows:
        if r['overlay'] == mask_hash:
            if mask_only or r['bg'] == bg_hash:
                tiles.append(r)
    return tiles

def detect_screen_size(all_rows):
    """Detect game screen resolution from ALL dst coordinates in TSV."""
    max_x = max(r['dx1'] for r in all_rows)
    max_y = max(r['dy1'] for r in all_rows)
    # Round to nearest multiple of 320/240
    screen_w = round(max_x / 320) * 320
    screen_h = round(max_y / 240) * 240
    if screen_w < 320: screen_w = 320
    if screen_h < 240: screen_h = 240
    return screen_w, screen_h

def generate_hd_mask(mask_img, hd_bg_img, tiles, screen_w, screen_h):
    """
    Generate HD mask using UV/dst coordinate mapping.
    Each tile maps mask UV region -> screen dst -> background position.
    """
    mask_arr = np.array(mask_img)
    hd_bg_arr = np.array(hd_bg_img)
    
    mask_w, mask_h = mask_img.size  # 512, 512
    hd_bg_w, hd_bg_h = hd_bg_img.size
    
    # Scale: original bg (320x240) -> HD bg
    bg_scale_x = hd_bg_w / 320.0  # e.g. 1280/320 = 4
    bg_scale_y = hd_bg_h / 240.0  # e.g. 960/240 = 4
    
    # HD mask size matches HD bg proportionally
    hd_mask_w = int(mask_w * bg_scale_x)
    hd_mask_h = int(mask_h * bg_scale_y)
    hd_mask = np.zeros((hd_mask_h, hd_mask_w, 4), dtype=np.uint8)
    
    # Pre-upscale alpha channel with bicubic interpolation for smooth edges
    alpha_orig = Image.fromarray(mask_arr[:, :, 3])
    alpha_hd = alpha_orig.resize((hd_mask_w, hd_mask_h), Image.BICUBIC)
    alpha_hd_arr = np.array(alpha_hd)
    
    print(f"  Screen: {screen_w}x{screen_h}")
    print(f"  HD bg: {hd_bg_w}x{hd_bg_h}, bg_scale: {bg_scale_x:.1f}x{bg_scale_y:.1f}")
    print(f"  HD mask: {hd_mask_w}x{hd_mask_h}")
    print(f"  Tiles: {len(tiles)}")
    
    # Half-texel padding to close gaps between adjacent tiles
    uv_pad = 1.0 / mask_w  # 1 texel in UV space
    
    for t in tiles:
        # Mask UV region in floating point — expand by 1 texel to cover seams
        u0 = t['u0'] - uv_pad
        v0 = t['v0'] - uv_pad
        u1 = t['u1'] + uv_pad
        v1 = t['v1'] + uv_pad
        
        # Original (unexpanded) UV for screen mapping
        ou0, ov0, ou1, ov1 = t['u0'], t['v0'], t['u1'], t['v1']
        
        # Screen dst region
        dx0, dy0, dx1, dy1 = t['dx0'], t['dy0'], t['dx1'], t['dy1']
        
        if dx1 <= dx0 or dy1 <= dy0 or u1 <= u0 or v1 <= v0:
            continue
        
        # HD mask region for this tile (pull: iterate every HD pixel)
        hd_mx0 = int(u0 * mask_w * bg_scale_x)
        hd_my0 = int(v0 * mask_h * bg_scale_y)
        hd_mx1 = int(u1 * mask_w * bg_scale_x + 0.999)
        hd_my1 = int(v1 * mask_h * bg_scale_y + 0.999)
        
        # Clamp to HD mask bounds
        hd_mx0 = max(0, hd_mx0); hd_mx1 = min(hd_mask_w, hd_mx1)
        hd_my0 = max(0, hd_my0); hd_my1 = min(hd_mask_h, hd_my1)
        
        tile_hd_w = hd_mx1 - hd_mx0
        tile_hd_h = hd_my1 - hd_my0
        if tile_hd_w <= 0 or tile_hd_h <= 0:
            continue
        
        for hy in range(hd_my0, hd_my1):
            for hx in range(hd_mx0, hd_mx1):
                # Check smooth upscaled alpha
                alpha = alpha_hd_arr[hy, hx]
                if alpha == 0:
                    continue
                
                # HD mask pixel -> original mask texel
                orig_mx = hx / bg_scale_x
                orig_my = hy / bg_scale_y
                
                # Original mask texel -> normalized position within tile (0..1)
                # Use original (unexpanded) UV for accurate screen mapping
                t_fx = (orig_mx / mask_w - ou0) / (ou1 - ou0)
                t_fy = (orig_my / mask_h - ov0) / (ov1 - ov0)
                
                # Normalized position -> screen position
                scr_x = dx0 + t_fx * (dx1 - dx0)
                scr_y = dy0 + t_fy * (dy1 - dy0)
                
                # Screen -> HD bg position
                hd_bx = scr_x / screen_w * hd_bg_w
                hd_by = scr_y / screen_h * hd_bg_h
                
                ibx = max(0, min(int(hd_bx), hd_bg_w - 1))
                iby = max(0, min(int(hd_by), hd_bg_h - 1))
                
                hd_mask[hy, hx, :3] = hd_bg_arr[iby, ibx, :3]
                hd_mask[hy, hx, 3] = alpha  # smooth anti-aliased alpha
    
    return Image.fromarray(hd_mask)

def main():
    parser = argparse.ArgumentParser(description="Generate HD masks for Dino Crisis 2")
    parser.add_argument('--tsv', default='dc2_pairs.tsv')
    parser.add_argument('--dump', default='dump/textures')
    parser.add_argument('--hires', default='hires/textures')
    parser.add_argument('--out', default='hires/textures')
    parser.add_argument('--overrides', default='mask_overrides.tsv', help='Manual bg->mask overrides')
    parser.add_argument('--test', type=str, default=None, help='Test single bg hash')
    args = parser.parse_args()

    print("=== DC2 HD Mask Generator v2 ===\n")

    rows = parse_tsv(args.tsv)
    print(f"Loaded {len(rows)} entries from TSV")

    # Detect screen resolution globally from ALL coordinates
    screen_w, screen_h = detect_screen_size(rows)
    print(f"Screen resolution: {screen_w}x{screen_h}")

    mask_pairs = find_mask_pairs(rows)
    
    # Apply manual overrides
    overrides = load_overrides(args.overrides)
    if overrides:
        print(f"Loaded {len(overrides)} manual overrides from {args.overrides}")
        mask_pairs.update(overrides)
    
    print(f"Total {len(mask_pairs)} bg->mask pairs:\n")
    for bg, mask in sorted(mask_pairs.items()):
        tiles = get_tiles_for_pair(rows, bg, mask)
        src = " (override)" if bg in overrides else ""
        print(f"  {bg} -> {mask} ({len(tiles)} tiles){src}")
    print()

    os.makedirs(args.out, exist_ok=True)

    targets = list(mask_pairs.items())
    if args.test:
        m = mask_pairs.get(args.test)
        if not m:
            print(f"BG {args.test} not found!"); return
        targets = [(args.test, m)]

    for bg_hash, mask_hash in targets:
        print(f"\n--- BG={bg_hash} MASK={mask_hash} ---")

        mask_img, mask_file = load_texture(args.dump, mask_hash)
        if not mask_img:
            print(f"  SKIP: mask {mask_hash} not in {args.dump}"); continue
        print(f"  Mask: {mask_file} {mask_img.size}")

        hd_bg_img, hd_bg_file = load_texture(args.hires, bg_hash)
        if not hd_bg_img:
            print(f"  SKIP: HD bg {bg_hash} not in {args.hires}"); continue
        print(f"  HD BG: {hd_bg_file} {hd_bg_img.size}")

        is_override = bg_hash.upper() in overrides
        tiles = get_tiles_for_pair(rows, bg_hash, mask_hash, mask_only=is_override)
        if not tiles:
            print(f"  SKIP: no tiles found"); continue

        hd_mask = generate_hd_mask(mask_img, hd_bg_img, tiles, screen_w, screen_h)

        out_name = f"{mask_hash}_{mask_img.size[0]}x{mask_img.size[1]}.webp"
        out_path = os.path.join(args.out, out_name)
        hd_mask.save(out_path, lossless=True)
        print(f"  Saved: {out_path}")

    print("\n=== Done ===")

if __name__ == '__main__':
    main()
