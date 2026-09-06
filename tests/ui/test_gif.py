#!/usr/bin/env python3
"""The GIF writer, checked by decoding its own output.

Two things here can be wrong in ways that still produce a file every viewer
opens without complaint:

  LZW         the code width grows AFTER the code that filled the old range.
              Off by one and the stream decodes to plausible garbage.
  delta frames  every frame after the first is only the rectangle that
              changed, drawn at an offset over what is already on screen. A
              wrong offset leaves frame 1 perfect and everything after it
              subtly displaced - which is invisible in a thumbnail.

So this decodes with an independent implementation and composites the result,
rather than checking that the header says GIF89a.

Run: python tests/ui/test_gif.py
"""

import os
import struct
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

from gif import write_gif      # noqa: E402

bad = []


def ok(cond, why):
    print(('  ok    ' if cond else '  FAIL  ') + why)
    if not cond:
        bad.append(why)


def rgb(v):
    r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def decode(path):
    """A GIF89a decoder written from the spec, not from gif.py."""
    d = open(path, 'rb').read()
    assert d[:6] == b'GIF89a', 'not a GIF89a'
    sw, sh, packed, _, _ = struct.unpack('<HHBBB', d[6:13])
    ncol = 1 << ((packed & 7) + 1)
    pal = [tuple(d[13 + n * 3:16 + n * 3]) for n in range(ncol)]
    i = 13 + ncol * 3
    canvas = [None] * (sw * sh)
    out = []
    while i < len(d) and d[i] != 0x3B:
        if d[i] == 0x21:                       # extension
            i += 2
            while d[i]:
                i += 1 + d[i]
            i += 1
            continue
        if d[i] != 0x2C:
            break
        x, y, w, h, _p = struct.unpack('<HHHHB', d[i + 1:i + 10])
        i += 10
        mcs = d[i]
        i += 1
        data = bytearray()
        while d[i]:
            data += d[i + 1:i + 1 + d[i]]
            i += 1 + d[i]
        i += 1

        clear, eoi = 1 << mcs, (1 << mcs) + 1
        tbl = [bytes([k]) for k in range(clear)] + [b'', b'']
        width, buf, nb, px, prev, done = mcs + 1, 0, 0, bytearray(), None, False
        for byte in data:
            if done:
                break
            buf |= byte << nb
            nb += 8
            while nb >= width:
                code = buf & ((1 << width) - 1)
                buf >>= width
                nb -= width
                if code == clear:
                    tbl = [bytes([k]) for k in range(clear)] + [b'', b'']
                    width, prev = mcs + 1, None
                    continue
                if code == eoi:
                    done = True
                    break
                entry = tbl[code] if code < len(tbl) else prev + prev[:1]
                px += entry
                if prev is not None:
                    tbl.append(prev + entry[:1])
                    if len(tbl) == (1 << width) and width < 12:
                        width += 1
                prev = entry
        assert len(px) == w * h, 'frame %d: %d px for a %dx%d rect' % (
            len(out), len(px), w, h)
        for r in range(h):
            for c in range(w):
                canvas[(y + r) * sw + x + c] = pal[px[r * w + c]]
        out.append(list(canvas))
    return sw, sh, out


def roundtrip(name, w, h, frames, scale=1):
    path = os.path.join(tempfile.gettempdir(), '_nexus_%s.gif' % name)
    size, ncol = write_gif(path, w, h, frames, [10] * len(frames), scale)
    sw, sh, got = decode(path)
    ok(sw == w * scale and sh == h * scale,
       '%s: canvas is %dx%d' % (name, sw, sh))
    ok(len(got) == len(frames),
       '%s: %d frames in, %d out' % (name, len(frames), len(got)))

    def expand(src):
        """The source frame at output resolution, so scale>1 compares like
        for like rather than 1024 pixels against 9216."""
        if scale == 1:
            return [rgb(v) for v in src]
        out = []
        for y in range(h):
            row = [rgb(v) for v in src[y * w:(y + 1) * w] for _ in range(scale)]
            out += row * scale
        return out

    exact = all(expand(src) == dec for src, dec in zip(frames, got))
    ok(exact, '%s: every frame composites back pixel-exact' % name)
    return size


def main():
    print('Round trip through an independent decoder')

    # A moving dot on a still ground: the delta path, and the case where a
    # naive encoder gets the sub-rectangle offset wrong.
    W = H = 32
    frames = []
    for i in range(8):
        f = [0x0841] * (W * H)
        f[(6 + i) * W + (4 + i * 2)] = 0xF800
        f[(6 + i) * W + (5 + i * 2)] = 0x07E0
        frames.append(f)
    delta = roundtrip('dot', W, H, frames)

    # No change at all between two frames - the 1x1 no-op that still has to
    # carry a delay without corrupting the canvas.
    still = [[0x1234] * (W * H)] * 3
    roundtrip('still', W, H, still)

    # Integer upscale, which multiplies the offsets too.
    roundtrip('scaled', W, H, frames[:4], scale=3)

    print('\nDelta framing actually saves something')
    full = sum(len(f) for f in frames)
    ok(delta < full, 'a mostly-static animation encodes to %d B, under the '
       '%d B of raw indices' % (delta, full))

    print('\nLimits are refused, not silently degraded')
    try:
        write_gif(os.path.join(tempfile.gettempdir(), '_nexus_over.gif'),
                  16, 16, [list(range(256 + 1)) + [0] * (256 - 1)], [10])
        ok(False, 'a >256 colour set raises rather than dithering')
    except ValueError:
        ok(True, 'a >256 colour set raises rather than dithering')

    try:
        write_gif(os.path.join(tempfile.gettempdir(), '_nexus_bad.gif'),
                  4, 4, [[0] * 16, [0] * 16], [10])
        ok(False, 'a delay per frame is required')
    except ValueError:
        ok(True, 'a delay per frame is required')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
