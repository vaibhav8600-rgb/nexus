#!/usr/bin/env python3
"""Render the UI's own glyphs and palettes into docs/images/.

Screenshots of a 240x240 SPI panel are photographs of a desk, and they go
stale the moment a colour moves. Everything drawn here is read out of the
firmware source instead - the glyph bitmaps from src/ui/home.c, the palettes
from src/ui/theme.c - so a doc image cannot disagree with the build. Re-run it
after touching either file.

Pure stdlib, like png2c.py, and for the same reason: this has to work in a
container that has never heard of Pillow.

Run: python scripts/gen_doc_images.py
"""

import io
import os
import re
import struct
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OUT = os.path.join(ROOT, 'docs', 'images', 'ui')


# ---- a minimal PNG writer ----------------------------------------------
def write_png(path, w, h, rgb):
    """rgb is a flat list of (r, g, b) rows-first."""
    raw = b''
    for y in range(h):
        raw += b'\x00'                       # filter: none
        for x in range(w):
            raw += bytes(rgb[y * w + x])

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    io.open(path, 'wb').write(png)


# ---- colour, exactly as the firmware sees it ---------------------------
def rgb565(v):
    """NEXUS_C(): 0xRRGGBB -> the 5/6/5 the panel actually receives."""
    r = (v >> 19) & 0x1F
    g = (v >> 10) & 0x3F
    b = (v >> 3) & 0x1F
    # back to 8 bits the way the panel expands it, so the doc shows the
    # quantised colour rather than the one that was asked for
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def mix(a, b, alpha):
    return tuple((a[i] * (255 - alpha) + b[i] * alpha) // 255 for i in range(3))


# ---- read the firmware -------------------------------------------------
def glyphs():
    src = io.open(os.path.join(ROOT, 'src', 'ui', 'home.c'),
                  encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'static const uint16_t (\w+)\[(\w+)\]\s*=\s*\{'
                         r'([^;]*?)\};', src, re.S):
        name, body = m.group(1), m.group(3)
        out[name] = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', body)]
    # mod_glyphs is a 2-D array; split it into its four rows of 11
    m = re.search(r'static const uint16_t mod_glyphs\[4\]\[MOD_GLYPH_H\]\s*='
                  r'\s*\{(.*?)\n\};', src, re.S)
    if m:
        vals = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', m.group(1))]
        for i, nm in enumerate(('mod_ctrl', 'mod_shift', 'mod_alt', 'mod_gui')):
            out[nm] = vals[i * 11:(i + 1) * 11]
    return out, src


def themes():
    src = io.open(os.path.join(ROOT, 'src', 'ui', 'theme.c'),
                  encoding='utf-8').read()
    out = []
    for blob in src.split('.name = ')[1:]:
        name = re.match(r'"([^"]+)"', blob).group(1)
        f = {}
        for key in ('bg_top', 'bg_bot', 'panel', 'edge_hi', 'value', 'accent',
                    'accent_alt', 'caption', 'muted', 'track', 'success',
                    'warning', 'error'):
            m = re.search(r'\.%s = NEXUS_C\(0x([0-9A-Fa-f]+)u\)' % key, blob)
            if m:
                f[key] = int(m.group(1), 16)
        m = re.search(r'\.panel_alpha = (\d+)', blob)
        f['panel_alpha'] = int(m.group(1)) if m else 30
        m = re.search(r'\.wordmark = \{([^}]*)\}', blob, re.S)
        f['wordmark'] = [int(v, 16)
                         for v in re.findall(r'0x([0-9A-Fa-f]+)u', m.group(1))]
        out.append((name, f))
    return out


def consts(src):
    return {k: int(v) for k, v in
            re.findall(r'^#define (TR_W|TR_H|TR_SCALE|ST_W|ST_H|ST_SCALE|'
                       r'MOD_GLYPH_W|MOD_GLYPH_H|MOD_SCALE)\s+(\d+)',
                       src, re.M)}


def render(rows, w, h, scale, fg, bg, pad=4):
    """One glyph, exactly as gfx_glyph() blits it: bit 0 is the left column."""
    pw, ph = w * scale + pad * 2, h * scale + pad * 2
    buf = [bg] * (pw * ph)
    for r in range(h):
        for c in range(w):
            if not (rows[r] & (1 << c)):
                continue
            for dy in range(scale):
                for dx in range(scale):
                    buf[(pad + r * scale + dy) * pw + pad + c * scale + dx] = fg
    return pw, ph, buf


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    g, home_src = glyphs()
    C = consts(home_src)
    th = themes()
    name, T = th[0]                      # the NEXUS palette leads the docs

    # The card these sit on: a panel tint over the ground, which is what
    # nexus_draw_card() composites. Same arithmetic, so the same colour.
    ground = mix(rgb565(T['bg_top']), rgb565(T['bg_bot']), 128)
    card = mix(ground, rgb565(T['panel']), T['panel_alpha'])

    made = []

    # connectivity: transports, then the profile tile
    want = [
        ('usb-ready', 'tr_usb_ready', 'TR', 'value'),
        ('usb-ready-idle', 'tr_usb_ready', 'TR', 'muted'),
        ('usb-nohid', 'tr_usb_idle', 'TR', 'value'),
        ('ble-selected', 'tr_ble', 'TR', 'value'),
        ('ble-idle', 'tr_ble', 'TR', 'muted'),
        ('tile-ok', 'st_ok', 'ST', 'accent'),
        ('tile-down', 'st_down', 'ST', 'accent'),
        ('tile-open', 'st_open', 'ST', 'accent'),
    ]
    for out_name, sym, kind, col in want:
        if sym not in g:
            print('  MISSING glyph %s' % sym)
            continue
        w, h, s = C[kind + '_W'], C[kind + '_H'], C[kind + '_SCALE']
        pw, ph, buf = render(g[sym], w, h, s, rgb565(T[col]), card)
        write_png(os.path.join(OUT, out_name + '.png'), pw, ph, buf)
        made.append(out_name)

    # modifiers, held and not
    for sym, label in (('mod_ctrl', 'ctrl'), ('mod_shift', 'shift'),
                       ('mod_alt', 'alt'), ('mod_gui', 'gui')):
        for on, suffix, col in ((True, '-on', 'value'), (False, '', 'muted')):
            # Must match draw_mods() in home.c exactly: these swatches are
            # what the docs show a held key looking like. They drifted once
            # already - accent_alt/bg_bot here while the firmware drew
            # accent/track - and the docs showed a colour the device did not.
            slot = mix(card, rgb565(T['accent'] if on else T['track']),
                       170 if on else 150)
            pw, ph, buf = render(g[sym], C['MOD_GLYPH_W'], C['MOD_GLYPH_H'],
                                 C['MOD_SCALE'], rgb565(T[col]), slot, pad=5)
            write_png(os.path.join(OUT, 'mod-%s%s.png' % (label, suffix)),
                      pw, ph, buf)
            made.append('mod-%s%s' % (label, suffix))

    # one palette strip per theme: the colours a reader actually picks between
    SW, SH = 34, 30
    keys = ['bg_bot', 'panel', 'value', 'accent', 'accent_alt', 'warning',
            'error']
    for tname, t in th:
        w, h = SW * len(keys), SH
        buf = [(0, 0, 0)] * (w * h)
        for i, k in enumerate(keys):
            c = rgb565(t.get(k, 0))
            for y in range(h):
                for x in range(SW):
                    buf[y * w + i * SW + x] = c
        fn = 'theme-%s.png' % tname.lower()
        write_png(os.path.join(OUT, fn), w, h, buf)
        made.append(fn[:-4])

    # the wordmark ramp, five stops, per theme
    for tname, t in th:
        wm = t['wordmark']
        w, h = SW * len(wm), 18
        buf = [(0, 0, 0)] * (w * h)
        for i, v in enumerate(wm):
            c = rgb565(v)
            for y in range(h):
                for x in range(SW):
                    buf[y * w + i * SW + x] = c
        fn = 'wordmark-%s.png' % tname.lower()
        write_png(os.path.join(OUT, fn), w, h, buf)
        made.append(fn[:-4])

    print('wrote %d images to docs/images/ui/' % len(made))
    print('themes: %s' % ', '.join(n for n, _ in th))
    return 0


if __name__ == '__main__':
    sys.exit(main())
