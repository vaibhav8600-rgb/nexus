#!/usr/bin/env python3
"""Checks for the splash asset pipeline.

The PNG decoder in scripts/png2c.py is hand-rolled to avoid a Pillow
dependency, so it gets a real test: synthesise PNGs (every colour type, every
scanline filter), decode them back, and assert the pixels survive.

Run: python tests/splash/test_png2c.py
"""

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))

import png2c  # noqa: E402


def chunk(kind, body):
    return (struct.pack(">I", len(body)) + kind + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))


def make_png(width, height, ctype, rows, filters, palette=None, trns=None):
    """rows: list of bytes, one unfiltered scanline each."""
    bpp = png2c.CHANNELS[ctype]
    stride = width * bpp
    raw = bytearray()
    prev = bytes(stride)

    for y, line in enumerate(rows):
        f = filters[y % len(filters)]
        raw.append(f)
        enc = bytearray(stride)
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            ul = prev[i - bpp] if i >= bpp else 0
            if f == 0:
                enc[i] = line[i]
            elif f == 1:
                enc[i] = (line[i] - left) & 0xFF
            elif f == 2:
                enc[i] = (line[i] - up) & 0xFF
            elif f == 3:
                enc[i] = (line[i] - (left + up) // 2) & 0xFF
            elif f == 4:
                enc[i] = (line[i] - png2c.paeth(left, up, ul)) & 0xFF
        raw += enc
        prev = line

    out = png2c.PNG_SIG
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, ctype,
                                      0, 0, 0))
    if palette:
        out += chunk(b"PLTE", palette)
    if trns:
        out += chunk(b"tRNS", trns)
    out += chunk(b"IDAT", zlib.compress(bytes(raw)))
    out += chunk(b"IEND", b"")
    return out


checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        raise AssertionError(what)


def test_rgb_all_filters():
    """Every filter type must reconstruct the same image."""
    w, h = 6, 5
    rows = [bytes(v for x in range(w)
                  for v in ((x * 40 + y * 7) % 256, (x * 11) % 256,
                            (y * 50) % 256))
            for y in range(h)]
    flat = [tuple(row[x * 3:x * 3 + 3]) + (255,) for row in rows
            for x in range(w)]

    for filters in ([0], [1], [2], [3], [4], [0, 1, 2, 3, 4]):
        png = make_png(w, h, 2, rows, filters)
        gw, gh, px = png2c.decode_png(png)
        check((gw, gh) == (w, h), "size mismatch for filters %s" % filters)
        check(px == flat, "pixels differ for filters %s" % filters)


def test_rgba_flattened_onto_bg():
    """RGB565 has no alpha, so transparency must composite, not vanish."""
    rows = [bytes((255, 0, 0, 0, 255, 0, 0, 255))]
    png = make_png(2, 1, 6, rows, [0])
    _, _, px = png2c.decode_png(png)

    data = png2c.to_rgb565(px, (0, 0, 0))
    # Fully transparent red over black is black; opaque red stays red.
    check(data[0] == 0x0000, "transparent pixel did not flatten to bg")
    check(data[1] != 0x0000, "opaque pixel was lost")

    data = png2c.to_rgb565(px, (255, 255, 255))
    check(data[0] == 0xFFFF, "transparent pixel ignored white bg")


def test_grayscale_and_palette():
    png = make_png(3, 1, 0, [bytes((0, 128, 255))], [0])
    _, _, px = png2c.decode_png(png)
    check(px == [(0, 0, 0, 255), (128, 128, 128, 255), (255, 255, 255, 255)],
          "grayscale decode wrong")

    palette = bytes((10, 20, 30, 40, 50, 60))
    png = make_png(2, 1, 3, [bytes((1, 0))], [0], palette=palette,
                   trns=bytes((7,)))
    _, _, px = png2c.decode_png(png)
    check(px == [(40, 50, 60, 255), (10, 20, 30, 7)],
          "palette/tRNS decode wrong: %r" % px)


def test_rgb565_packing():
    """Channel packing, checked against the three values with no rounding."""
    px = [(255, 255, 255, 255), (0, 0, 0, 255), (255, 0, 0, 255),
          (0, 255, 0, 255), (0, 0, 255, 255)]

    v = png2c.to_rgb565(px, (0, 0, 0))

    check(v[0] == 0xFFFF, "white is not 0xFFFF: %04X" % v[0])
    check(v[1] == 0x0000, "black is not 0x0000: %04X" % v[1])
    check(v[2] == 0xF800, "red is not 0xF800: %04X" % v[2])
    check(v[3] == 0x07E0, "green is not 0x07E0: %04X" % v[3])
    check(v[4] == 0x001F, "blue is not 0x001F: %04X" % v[4])
    check(len(v) == len(px), "one value per pixel")
    check(all(0 <= x <= 0xFFFF for x in v), "value out of 16-bit range")


def test_downscale_halves_and_averages():
    # 4x4 of a single colour must survive; size must actually shrink.
    px = [(200, 100, 50, 255)] * 16
    w, h, out = png2c.box_downscale(4, 4, px, 2)
    check((w, h) == (2, 2), "downscale target wrong: %dx%d" % (w, h))
    check(all(p == (200, 100, 50, 255) for p in out), "flat colour drifted")

    # Under the limit, nothing changes.
    w, h, out = png2c.box_downscale(4, 4, px, 8)
    check((w, h, len(out)) == (4, 4, 16), "downscale ran when it should not")


def test_rejects_interlaced():
    png = make_png(2, 1, 2, [bytes((1, 2, 3, 4, 5, 6))], [0])
    # Flip the interlace byte in IHDR (last byte of the 13-byte body).
    ihdr = png.index(b"IHDR") + 4
    broken = bytearray(png)
    broken[ihdr + 12] = 1
    try:
        png2c.decode_png(bytes(broken))
    except png2c.PngError:
        check(True, "")
        return
    except zlib.error:
        # CRC is now wrong, but we must fail before that matters.
        pass
    raise AssertionError("interlaced PNG was accepted")


def test_emit_produces_compilable_c():
    """The generated file must define the entry point splash.c links against."""
    import tempfile

    w, h = 8, 8
    rows = [bytes(v for x in range(w) for v in (x * 30 % 256, y * 30 % 256, 90))
            for y in range(h)]
    png = make_png(w, h, 2, rows, [0, 1, 2, 3, 4])

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in.png")
        dst = os.path.join(tmp, "out.c")
        with open(src, "wb") as fh:
            fh.write(png)

        rc = png2c.main([src, dst, "--max-size", "4"])
        check(rc == 0, "png2c returned %d" % rc)

        text = open(dst, encoding="utf-8").read()

    # The generated file must define exactly what include/nexus/splash.h
    # declares and what assets/splash_default.c provides, or the link picks
    # neither and the splash silently disappears.
    check("struct nexus_splash_art nexus_splash_art" in text,
          "art symbol missing")
    check("gfx_blit565" in text, "blit call missing")
    check("#define NEXUS_SPLASH_W 4" in text,
          "downscale not applied: %s" % text[:400])
    check("#define NEXUS_SPLASH_H 4" in text, "downscale not applied")

    # 4x4 = 16 uint16 values. Count inside the array only -- the header comment
    # quotes the source path, and a tmp path can itself contain "0x".
    body = text.split("] = {", 1)[1].split("};", 1)[0]
    check(body.count("0x") == 16, "wrong value count: %d" % body.count("0x"))


def main():
    test_rgb_all_filters()
    test_rgba_flattened_onto_bg()
    test_grayscale_and_palette()
    test_rgb565_packing()
    test_downscale_halves_and_averages()
    test_rejects_interlaced()
    test_emit_produces_compilable_c()
    print("png2c: %d checks passed" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
