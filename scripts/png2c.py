#!/usr/bin/env python3
"""Convert a user PNG into a NEXUS splash bitmap, at build time.

Section 20: the integrator drops a PNG into their own zmk-config and the build
turns it into firmware data. Nobody hand-generates C arrays and nobody edits
NEXUS source to change artwork.

Output is a flat native-endian RGB565 array plus a `struct nexus_splash_art`,
which is the same symbol assets/splash_default.c provides. The compositor
byte-swaps once per band on flush, so there is no endianness knob here to get
wrong.

Deliberately stdlib-only. Pillow is not installed in ZMK's build container and
adding a pip dependency to a firmware build to read a 240x240 PNG is a bad
trade -- zlib already does the hard part.

Supports 8-bit PNGs: grayscale, grayscale+alpha, RGB, RGBA and palette, with
all five scanline filters. Interlaced PNGs are rejected rather than silently
mangled.
"""

import argparse
import struct
import sys
import zlib

PNG_SIG = b"\x89PNG\r\n\x1a\n"

# Bytes per pixel for each PNG colour type at bit depth 8.
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


class PngError(Exception):
    pass


def read_chunks(data):
    if data[:8] != PNG_SIG:
        raise PngError("not a PNG file")

    pos = 8
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        yield kind, body
        pos += 12 + length  # length + type + body + crc


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def unfilter(raw, width, height, bpp):
    """Reverse the per-scanline filters into a flat bytearray of pixels."""
    stride = width * bpp
    out = bytearray(stride * height)
    pos = 0

    for y in range(height):
        ftype = raw[pos]
        pos += 1
        line = raw[pos:pos + stride]
        pos += stride

        base = y * stride
        prev = base - stride

        if ftype == 0:
            out[base:base + stride] = line
        elif ftype == 1:
            for i in range(stride):
                left = out[base + i - bpp] if i >= bpp else 0
                out[base + i] = (line[i] + left) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                up = out[prev + i] if y else 0
                out[base + i] = (line[i] + up) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = out[base + i - bpp] if i >= bpp else 0
                up = out[prev + i] if y else 0
                out[base + i] = (line[i] + (left + up) // 2) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = out[base + i - bpp] if i >= bpp else 0
                up = out[prev + i] if y else 0
                ul = out[prev + i - bpp] if (y and i >= bpp) else 0
                out[base + i] = (line[i] + paeth(left, up, ul)) & 0xFF
        else:
            raise PngError("unknown scanline filter %d" % ftype)

    return out


def decode_png(data):
    """-> (width, height, list of (r, g, b, a) tuples)."""
    width = height = depth = ctype = None
    palette = b""
    trns = b""
    idat = bytearray()

    for kind, body in read_chunks(data):
        if kind == b"IHDR":
            width, height, depth, ctype, _comp, _filt, interlace = struct.unpack(
                ">IIBBBBB", body
            )
            if interlace:
                raise PngError("interlaced PNGs are not supported; re-save "
                               "without Adam7 interlacing")
            if depth != 8:
                raise PngError("bit depth %d is not supported; save as 8-bit"
                               % depth)
            if ctype not in CHANNELS:
                raise PngError("colour type %d is not supported" % ctype)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if width is None:
        raise PngError("no IHDR chunk")

    bpp = CHANNELS[ctype]
    raw = zlib.decompress(bytes(idat))
    flat = unfilter(raw, width, height, bpp)

    pixels = []
    for i in range(width * height):
        px = flat[i * bpp:(i + 1) * bpp]
        if ctype == 0:
            pixels.append((px[0], px[0], px[0], 255))
        elif ctype == 4:
            pixels.append((px[0], px[0], px[0], px[1]))
        elif ctype == 2:
            pixels.append((px[0], px[1], px[2], 255))
        elif ctype == 6:
            pixels.append((px[0], px[1], px[2], px[3]))
        else:  # palette
            idx = px[0]
            off = idx * 3
            if off + 3 > len(palette):
                raise PngError("palette index %d out of range" % idx)
            alpha = trns[idx] if idx < len(trns) else 255
            pixels.append((palette[off], palette[off + 1], palette[off + 2],
                           alpha))

    return width, height, pixels


def box_downscale(width, height, pixels, max_dim):
    """Integer box filter. Flash is the constraint, not resampling quality."""
    if max(width, height) <= max_dim:
        return width, height, pixels

    factor = (max(width, height) + max_dim - 1) // max_dim
    new_w = max(1, width // factor)
    new_h = max(1, height // factor)
    out = []

    for y in range(new_h):
        for x in range(new_w):
            r = g = b = a = n = 0
            for dy in range(factor):
                sy = y * factor + dy
                if sy >= height:
                    break
                for dx in range(factor):
                    sx = x * factor + dx
                    if sx >= width:
                        break
                    pr, pg, pb, pa = pixels[sy * width + sx]
                    r += pr
                    g += pg
                    b += pb
                    a += pa
                    n += 1
            out.append((r // n, g // n, b // n, a // n))

    return new_w, new_h, out


def to_rgb565(pixels, bg):
    """Flatten alpha onto @bg and pack to RGB565 values.

    Returns a list of 16-bit ints, not bytes: the generated array is uint16_t
    in native order and the compositor handles the panel's byte order on flush,
    so there is nothing to swap here.
    """
    br, bg_, bb = bg
    out = []

    for r, g, b, a in pixels:
        if a != 255:
            r = (r * a + br * (255 - a)) // 255
            g = (g * a + bg_ * (255 - a)) // 255
            b = (b * a + bb * (255 - a)) // 255

        out.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

    return out


def emit(path, symbol, width, height, values, source):
    lines = []
    add = lines.append

    add("/* Generated by scripts/png2c.py from %s - do not edit. */" % source)
    add("")
    add("#include <nexus/gfx.h>")
    add("#include <nexus/splash.h>")
    add("")
    add("#define NEXUS_SPLASH_W %d" % width)
    add("#define NEXUS_SPLASH_H %d" % height)
    add("")
    add("static const uint16_t %s[NEXUS_SPLASH_W * NEXUS_SPLASH_H] = {"
        % symbol)

    for i in range(0, len(values), 12):
        add("\t" + " ".join("0x%04X," % v for v in values[i:i + 12]))

    add("};")
    add("")
    add("static void nexus_splash_blit(int x, int y)")
    add("{")
    add("\tgfx_blit565(x, y, NEXUS_SPLASH_W, NEXUS_SPLASH_H, %s);" % symbol)
    add("}")
    add("")
    add("/* Same symbol assets/splash_default.c provides, so src/ui/splash.c")
    add(" * never learns which one it got (Section 21). */")
    add("const struct nexus_splash_art nexus_splash_art = {")
    add("\t.w = NEXUS_SPLASH_W,")
    add("\t.h = NEXUS_SPLASH_H,")
    add("\t.draw = nexus_splash_blit,")
    add("};")
    add("")

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


def parse_bg(text):
    text = text.lstrip("#")
    v = int(text, 16)
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--symbol", default="nexus_splash_pixels")
    ap.add_argument("--max-size", type=int, default=160,
                    help="downscale so neither side exceeds this")
    ap.add_argument("--bg", default="000000",
                    help="colour transparent pixels flatten onto")
    args = ap.parse_args(argv)

    try:
        with open(args.input, "rb") as fh:
            width, height, pixels = decode_png(fh.read())
    except (PngError, zlib.error, struct.error) as exc:
        sys.stderr.write("png2c: %s: %s\n" % (args.input, exc))
        return 1

    width, height, pixels = box_downscale(width, height, pixels, args.max_size)
    values = to_rgb565(pixels, parse_bg(args.bg))

    emit(args.output, args.symbol, width, height, values, args.input)
    # Flash cost is the number worth printing: Section 101 exists because a
    # full 240x240 splash is 115 KB, and this line is where you notice.
    sys.stderr.write("png2c: %s -> %dx%d, %d bytes of flash\n"
                     % (args.input, width, height, len(values) * 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
