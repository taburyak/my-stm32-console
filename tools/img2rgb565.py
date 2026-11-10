#!/usr/bin/env python3
"""
img2rgb565.py - Convert PNG/BMP/JPG/GIF images into RGB565 assets for the STM32F407 Console.

Supported output formats (--format):
  c      : C header/source array (`const uint16_t <name>[W * H]`) for `sys_draw_sprite` / `sys_lcd_blit`
  raw    : Headerless raw little-endian RGB565 pixel data (`.raw`) for `sys_file_read` into `.ext_bss`
  simg   : Prefixed binary (`"SIMG"` magic + uint16_t width + uint16_t height + RGB565 pixels)
  bmp16  : Uncompressed 16-bit RGB565 `.bmp` (e.g. `games/<app>.bmp` 64x64 launcher icon or wallpaper)

Works with Pillow (`pip install Pillow`) if installed, or falls back to a built-in pure-Python
24/32/16-bit BMP & 8-bit PNG reader when Pillow is not available.

Examples:
  python tools/img2rgb565.py hero.png -o sdk_apps/hero_spr.h --name spr_hero
  python tools/img2rgb565.py level_bg.png -o level_bg.raw --format raw --resize 320x240
  python tools/img2rgb565.py icon.png -o arkanoid.bmp --format bmp16 --icon
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import sys
import zlib
from typing import List, Optional, Tuple

try:
    from PIL import Image as _PILImage  # type: ignore
except ImportError:
    _PILImage = None

HAS_PIL = _PILImage is not None


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)


def rgb565_to_rgb888(c: int) -> Tuple[int, int, int]:
    r5 = (c >> 11) & 0x1F
    g6 = (c >> 5) & 0x3F
    b5 = c & 0x1F
    r = (r5 << 3) | (r5 >> 2)
    g = (g6 << 2) | (g6 >> 4)
    b = (b5 << 3) | (b5 >> 2)
    return (r, g, b)


def _load_bmp_fallback(data: bytes) -> Tuple[int, int, List[Tuple[int, int, int, int]]]:
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("Unsupported or corrupt BMP file")
    data_offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]

    top_down = height < 0
    height = abs(height)
    if width <= 0 or height <= 0 or bpp not in (16, 24, 32):
        raise ValueError(f"Unsupported BMP dimensions/bpp ({width}x{height}, {bpp} bpp)")
    if compression not in (0, 3):
        raise ValueError(f"Compressed BMP (compression={compression}) requires Pillow")

    row_bytes = ((width * (bpp // 8) + 3) & ~3)
    pixels: List[Tuple[int, int, int, int]] = [(0, 0, 0, 255)] * (width * height)

    for dy in range(height):
        src_row = dy if top_down else (height - 1 - dy)
        pos = data_offset + src_row * row_bytes
        row = data[pos : pos + row_bytes]
        for dx in range(width):
            if bpp == 24:
                b, g, r = row[dx * 3 : dx * 3 + 3]
                a = 255
            elif bpp == 32:
                b, g, r, a = row[dx * 4 : dx * 4 + 4]
            else:
                c16 = struct.unpack_from("<H", row, dx * 2)[0]
                r, g, b = rgb565_to_rgb888(c16)
                a = 255
            pixels[dy * width + dx] = (r, g, b, a)

    return width, height, pixels


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _load_png_fallback(data: bytes) -> Tuple[int, int, List[Tuple[int, int, int, int]]]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid PNG signature")

    pos = 8
    width = height = bit_depth = color_type = interlace = 0
    plte: List[Tuple[int, int, int]] = []
    trns: bytes = b""
    idat = bytearray()

    while pos + 8 <= len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        ctype = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        pos += 12 + length

        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk[:13])
        elif ctype == b"PLTE":
            plte = [(chunk[i], chunk[i + 1], chunk[i + 2]) for i in range(0, len(chunk), 3)]
        elif ctype == b"tRNS":
            trns = chunk
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break

    if bit_depth != 8 or interlace != 0 or color_type not in (0, 2, 3, 6):
        raise ValueError("Complex PNG mode requires Pillow (`pip install Pillow`)")

    bpp_map = {0: 1, 2: 3, 3: 1, 6: 4}
    bpp = bpp_map[color_type]
    stride = width * bpp
    raw = zlib.decompress(bytes(idat))

    recon = bytearray(height * stride)
    in_pos = 0
    for y in range(height):
        ftype = raw[in_pos]
        in_pos += 1
        row_start = y * stride
        prev_start = (y - 1) * stride
        for i in range(stride):
            x = raw[in_pos]
            in_pos += 1
            a = recon[row_start + i - bpp] if i >= bpp else 0
            b = recon[prev_start + i] if y > 0 else 0
            c = recon[prev_start + i - bpp] if (y > 0 and i >= bpp) else 0
            if ftype == 0:
                val = x
            elif ftype == 1:
                val = (x + a) & 0xFF
            elif ftype == 2:
                val = (x + b) & 0xFF
            elif ftype == 3:
                val = (x + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                val = (x + _paeth(a, b, c)) & 0xFF
            else:
                raise ValueError(f"Unsupported PNG filter {ftype}")
            recon[row_start + i] = val

    pixels: List[Tuple[int, int, int, int]] = []
    for y in range(height):
        row = recon[y * stride : (y + 1) * stride]
        for x in range(width):
            if color_type == 6:
                r, g, b, a = row[x * 4 : x * 4 + 4]
            elif color_type == 2:
                r, g, b = row[x * 3 : x * 3 + 3]
                a = 255
            elif color_type == 0:
                r = g = b = row[x]
                a = 255
            else:
                idx = row[x]
                r, g, b = plte[idx] if idx < len(plte) else (0, 0, 0)
                a = trns[idx] if idx < len(trns) else 255
            pixels.append((r, g, b, a))

    return width, height, pixels


def load_image_rgba(path: pathlib.Path) -> Tuple[int, int, List[Tuple[int, int, int, int]]]:
    if _PILImage is not None:
        with _PILImage.open(path) as im:
            im_rgba = im.convert("RGBA")
            w, h = im_rgba.size
            return w, h, list(im_rgba.getdata())

    raw = path.read_bytes()
    if raw[:2] == b"BM":
        return _load_bmp_fallback(raw)
    if raw[:8] == b"\x89PNG\r\n\x1a\n":
        return _load_png_fallback(raw)
    raise RuntimeError(
        f"Cannot decode '{path.name}' without Pillow. Install Pillow (`pip install Pillow`) or use BMP/8-bit PNG."
    )


def resize_nearest(
    w: int, h: int, pixels: List[Tuple[int, int, int, int]], dst_w: int, dst_h: int
) -> List[Tuple[int, int, int, int]]:
    if w == dst_w and h == dst_h:
        return pixels
    out: List[Tuple[int, int, int, int]] = [(0, 0, 0, 255)] * (dst_w * dst_h)
    for dy in range(dst_h):
        sy = (dy * h) // dst_h
        for dx in range(dst_w):
            sx = (dx * w) // dst_w
            out[dy * dst_w + dx] = pixels[sy * w + sx]
    return out


def convert_to_rgb565(
    w: int,
    h: int,
    pixels: List[Tuple[int, int, int, int]],
    alpha_key: int,
    alpha_threshold: int,
    dither: bool,
) -> List[int]:
    out: List[int] = [0] * (w * h)
    # Bayer 2x2 ordered dither matrix (-3..+3 in 8-bit space)
    bayer2 = ((0, 2), (3, 1))

    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[y * w + x]
            if a < alpha_threshold:
                out[y * w + x] = alpha_key & 0xFFFF
                continue
            if dither:
                d = bayer2[y & 1][x & 1]
                r = min(255, max(0, r + (d * 2 - 3)))
                g = min(255, max(0, g + (d - 1)))
                b = min(255, max(0, b + (d * 2 - 3)))
            out[y * w + x] = rgb888_to_rgb565(r, g, b)
    return out


def format_c_header(
    symbol: str,
    w: int,
    h: int,
    pixels565: List[int],
    ext_const: bool,
    swap_bytes: bool,
    source_name: str,
) -> str:
    guard = f"{symbol.upper()}_H"
    lines = [
        f"/* Auto-generated by tools/img2rgb565.py from {source_name} ({w}x{h} RGB565) */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stdint.h>",
        "",
        f"#define {symbol.upper()}_WIDTH   {w}U",
        f"#define {symbol.upper()}_HEIGHT  {h}U",
        f"#define {symbol.upper()}_PIXELS  ({w}U * {h}U)",
        "",
    ]

    qual = "static const uint16_t"
    if ext_const:
        qual = '__attribute__((section(".rodata"))) static const uint16_t'

    lines.append(f"{qual} {symbol}[{w * h}] = {{")
    per_row = 12
    for i in range(0, len(pixels565), per_row):
        chunk = pixels565[i : i + per_row]
        if swap_bytes:
            chunk = [((v & 0xFF) << 8) | ((v >> 8) & 0xFF) for v in chunk]
        hex_items = ", ".join(f"0x{v:04X}U" for v in chunk)
        comma = "," if (i + per_row) < len(pixels565) else ""
        lines.append(f"    {hex_items}{comma}")
    lines.extend(["};", "", f"#endif /* {guard} */", ""])
    return "\n".join(lines)


def build_bmp16(w: int, h: int, pixels565: List[int]) -> bytes:
    """Builds a standard 16-bit RGB565 BMP compatible with Load_BMP_To_Buffer in ui_manager.c."""
    row_bytes = (w * 2 + 3) & ~3
    img_size = row_bytes * h
    file_size = 54 + img_size

    header = bytearray(54)
    header[0:2] = b"BM"
    struct.pack_into("<I", header, 2, file_size)
    struct.pack_into("<I", header, 10, 54)      # pixel data offset
    struct.pack_into("<I", header, 14, 40)      # BITMAPINFOHEADER size
    struct.pack_into("<i", header, 18, w)
    struct.pack_into("<i", header, 22, h)       # bottom-up
    struct.pack_into("<H", header, 26, 1)       # planes
    struct.pack_into("<H", header, 28, 16)      # 16 bpp (RGB565 in Load_BMP_To_Buffer)
    struct.pack_into("<I", header, 30, 0)       # BI_RGB
    struct.pack_into("<I", header, 34, img_size)

    body = bytearray(img_size)
    for y in range(h):
        bmp_row = h - 1 - y
        row_off = bmp_row * row_bytes
        for x in range(w):
            struct.pack_into("<H", body, row_off + x * 2, pixels565[y * w + x])

    return bytes(header + body)


def sanitize_symbol(name: str) -> str:
    s = re.sub(r"[^0-9a-zA-Z_]", "_", name)
    if not s or s[0].isdigit():
        s = "img_" + s
    return s.lower()


def parse_size(spec: str) -> Tuple[int, int]:
    m = re.match(r"^(\d+)[xX](\d+)$", spec.strip())
    if not m:
        raise argparse.ArgumentTypeError(f"Invalid size '{spec}', expected WxH (e.g. 64x64 or 320x240)")
    w, h = int(m.group(1)), int(m.group(2))
    if w <= 0 or h <= 0 or w > 2048 or h > 2048:
        raise argparse.ArgumentTypeError(f"Dimensions out of range: {w}x{h}")
    return w, h


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Convert images to RGB565 assets for STM32F407 Console.")
    ap.add_argument("input", type=pathlib.Path, help="Input image file (PNG, BMP, JPG, ...)")
    ap.add_argument("-o", "--output", type=pathlib.Path, required=True, help="Output file (.h, .raw, .simg, .bmp)")
    ap.add_argument(
        "-f",
        "--format",
        choices=("c", "raw", "simg", "bmp16"),
        default=None,
        help="Output format (auto-detected from -o extension if omitted)",
    )
    ap.add_argument("-n", "--name", type=str, default=None, help="C identifier name for --format c")
    ap.add_argument("--resize", type=parse_size, default=None, help="Resize to WxH (e.g. 64x64, 320x240)")
    ap.add_argument("--icon", action="store_true", help="Shortcut for --resize 64x64 (Games Hub tile icon)")
    ap.add_argument("--wallpaper", action="store_true", help="Shortcut for --resize 320x216 (Console workspace wallpaper)")
    ap.add_argument("--alpha-key", type=lambda s: int(s, 0), default=0x0000, help="RGB565 color for transparent pixels (default: 0x0000)")
    ap.add_argument("--alpha-threshold", type=int, default=128, help="Alpha threshold 0..255 (default: 128)")
    ap.add_argument("--dither", action="store_true", help="Apply 2x2 ordered Bayer dithering before RGB565 quantization")
    ap.add_argument("--swap-bytes", action="store_true", help="Byte-swap each 16-bit pixel (big-endian RGB565)")
    args = ap.parse_args(argv)

    if not args.input.is_file():
        print(f"Error: input file not found: {args.input}", file=sys.stderr)
        return 1

    fmt = args.format
    if fmt is None:
        ext = args.output.suffix.lower()
        if ext in (".h", ".c", ".inc"):
            fmt = "c"
        elif ext == ".bmp":
            fmt = "bmp16"
        elif ext == ".simg":
            fmt = "simg"
        else:
            fmt = "raw"

    w, h, rgba = load_image_rgba(args.input)

    target_size = args.resize
    if args.icon:
        target_size = (64, 64)
    elif args.wallpaper:
        target_size = (320, 216)

    if target_size is not None:
        dst_w, dst_h = target_size
        if _PILImage is not None and (dst_w, dst_h) != (w, h):
            with _PILImage.open(args.input) as im:
                im_resized = im.convert("RGBA").resize((dst_w, dst_h), _PILImage.Resampling.LANCZOS)
                rgba = list(im_resized.getdata())
        else:
            rgba = resize_nearest(w, h, rgba, dst_w, dst_h)
        w, h = dst_w, dst_h

    pixels565 = convert_to_rgb565(w, h, rgba, args.alpha_key, args.alpha_threshold, args.dither)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    if fmt == "c":
        sym = sanitize_symbol(args.name or args.output.stem)
        text = format_c_header(sym, w, h, pixels565, False, args.swap_bytes, args.input.name)
        args.output.write_text(text, encoding="utf-8", newline="\n")
    elif fmt == "raw":
        pack_fmt = f">{len(pixels565)}H" if args.swap_bytes else f"<{len(pixels565)}H"
        args.output.write_bytes(struct.pack(pack_fmt, *pixels565))
    elif fmt == "simg":
        pack_fmt = f">{len(pixels565)}H" if args.swap_bytes else f"<{len(pixels565)}H"
        hdr = struct.pack("<4sHH", b"SIMG", w, h)
        args.output.write_bytes(hdr + struct.pack(pack_fmt, *pixels565))
    elif fmt == "bmp16":
        args.output.write_bytes(build_bmp16(w, h, pixels565))

    size_bytes = args.output.stat().st_size
    print(f"[img2rgb565] {args.input.name} -> {args.output} ({w}x{h}, format={fmt}, {size_bytes} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
