#!/usr/bin/env python3
"""Render full 240x240 screens to PNG, without a board.

There is no C compiler on the machine this was written on, so this is a port
of src/ui/gfx.c rather than a harness around it. Everything that CAN be read
out of the firmware is read out of the firmware -- both fonts, every glyph
bitmap, all seven palettes, and each screen's layout constants -- and only the
blending arithmetic and the draw ORDER are transcribed. Those are the parts a
reader can check against the C side by side.

That is a real caveat and it belongs at the top of the file: these are
reconstructions, not firmware output. They are exact where the data is shared
and only as good as the transcription where it is not. If a compiler ever
turns up, the honest version of this file is a harness that calls gfx_render()
and dumps the band buffer -- the compositor is already isolated behind
display_write(), so that is a small job.

Run: python scripts/render_ui.py
"""

import io
import os
import re
import struct
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'src', 'ui')
OUT = os.path.join(ROOT, 'docs', 'images', 'screens')

W = H = 240
PAD, GAP = 9, 7
CONTENT_W = W - 2 * PAD
CAPTION, LABEL, BODY, VALUE, BIG = 1, 2, 2, 3, 4
OPAQUE = 255


# ---------------------------------------------------------------- source
def read(rel):
    return io.open(os.path.join(ROOT, rel), encoding='utf-8').read()


def defines(text, names):
    """Resolve a set of #define constants, following references between them."""
    # Strip comments FIRST. Matching "anything but a slash" would also
    # truncate ((GFX_W - CARD_W) / 2) at the divide, which is most of the
    # centred layout in this UI.
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    raw = {k: v.strip() for k, v in
           re.findall(r'^#define\s+([A-Z0-9_]+)\s+([^\n]+)', text, re.M)}
    raw.update({'NEXUS_PAD': '9', 'NEXUS_GAP': '7', 'GFX_W': '240',
                'GFX_H': '240', 'NEXUS_CONTENT_W': '(GFX_W - 2 * NEXUS_PAD)',
                'NEXUS_TXT_CAPTION': '1', 'NEXUS_TXT_LABEL': '2',
                'NEXUS_TXT_BODY': '2', 'NEXUS_TXT_VALUE': '3',
                'NEXUS_TXT_BIG': '4'})
    out = {}
    for n in names:
        e = raw.get(n, '')
        for _ in range(10):
            nxt = re.sub(r'\b([A-Z][A-Z0-9_]*)\b',
                         lambda m: '(' + raw[m.group(1)] + ')'
                         if m.group(1) in raw else m.group(1), e)
            if nxt == e:
                break
            e = nxt
        try:
            out[n] = int(eval(e))
        except Exception:
            pass
    return out


def font5x7():
    t = read('src/ui/font5x7.h')
    body = t.split('font5x7[', 1)[1].split('= {', 1)[1]
    vals = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
    return [vals[i * 5:(i + 1) * 5] for i in range(len(vals) // 5)]


def font10x14():
    t = read('src/ui/font10x14.h')
    out = []
    for _, b in re.findall(r'/\*\s*(\S+)\s*\*/\s*\{([^}]*)\}', t):
        out.append([int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', b)])
    return out


def home_glyphs():
    t = read('src/ui/home.c')
    g = {}
    for m in re.finditer(r'static const uint16_t (\w+)\[\w+\]\s*=\s*\{'
                         r'([^;]*?)\};', t, re.S):
        g[m.group(1)] = [int(v, 16)
                         for v in re.findall(r'0x([0-9A-Fa-f]+)', m.group(2))]
    m = re.search(r'mod_glyphs\[4\]\[MOD_GLYPH_H\]\s*=\s*\{(.*?)\n\};', t, re.S)
    v = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]+)', m.group(1))]
    g['mods'] = [v[i * 11:(i + 1) * 11] for i in range(4)]
    return g


def C(v):
    """NEXUS_C(): 0xRRGGBB -> RGB565."""
    return ((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F)


def themes():
    t = read('src/ui/theme.c')
    out = []
    for blob in t.split('.name = ')[1:]:
        f = {'name': re.match(r'"([^"]+)"', blob).group(1)}
        for m in re.finditer(r'\.(\w+) = NEXUS_C\(0x([0-9A-Fa-f]+)u\)', blob):
            f[m.group(1)] = C(int(m.group(2), 16))
        for m in re.finditer(r'\.(\w+_alpha|radius) = (\d+)', blob):
            f[m.group(1)] = int(m.group(2))
        wm = re.search(r'\.wordmark = \{([^}]*)\}', blob, re.S).group(1)
        f['wordmark'] = [C(int(x, 16))
                         for x in re.findall(r'0x([0-9A-Fa-f]+)u', wm)]
        out.append(f)
    return out


HUD = defines(read('include/nexus/widgets.h'),
              ['NEXUS_HUD_END', 'NEXUS_HUD_TITLE_Y', 'NEXUS_HUD_ROW_Y',
               'NEXUS_HUD_ROW_H'])
HUD_END = HUD['NEXUS_HUD_END']
HUD_TITLE_Y = HUD['NEXUS_HUD_TITLE_Y']
HUD_ROW_Y = HUD['NEXUS_HUD_ROW_Y']


# ------------------------------------------------------- the compositor
F5, F10 = font5x7(), font10x14()
FONT_W, FONT_H = 5, 7
FACE_W, FACE_H = 10, 14


def div255(t):
    return (t + (t >> 8) + 1) >> 8


def mix(d, s, a):
    """src/ui/gfx.c mix(): RGB565 blend, in 5/6/5 space, same rounding."""
    if a <= 0:
        return d
    if a >= 255:
        return s
    ia = 255 - a
    r = div255((((s >> 11) & 0x1F) * a) + (((d >> 11) & 0x1F) * ia))
    g = div255((((s >> 5) & 0x3F) * a) + (((d >> 5) & 0x3F) * ia))
    b = div255(((s & 0x1F) * a) + ((d & 0x1F) * ia))
    return (r << 11) | (g << 5) | b


def isqrt32(n):
    return 0 if n <= 0 else int(n ** 0.5)


def corner_cov(dx, dy, r):
    d16 = isqrt32((dx * dx + dy * dy) * 256)
    cov = r * 16 + 8 - d16
    if cov <= 0:
        return 0
    if cov >= 16:
        return 255
    return cov * 255 // 16


class Canvas:
    """The 240x12 band compositor, run with one band the size of the panel."""

    def __init__(self):
        self.b = [0] * (W * H)

    def px(self, x, y, c, a=OPAQUE):
        if 0 <= x < W and 0 <= y < H:
            i = y * W + x
            self.b[i] = mix(self.b[i], c, a)

    def rect(self, x, y, w, h, c, a=OPAQUE):
        if w <= 0 or h <= 0 or a == 0:
            return
        for yy in range(max(y, 0), min(y + h, H)):
            row = yy * W
            if a >= 255:
                for xx in range(max(x, 0), min(x + w, W)):
                    self.b[row + xx] = c
            else:
                for xx in range(max(x, 0), min(x + w, W)):
                    self.b[row + xx] = mix(self.b[row + xx], c, a)

    def hline(self, x, y, w, c, a=OPAQUE):
        self.rect(x, y, w, 1, c, a)

    def vline(self, x, y, h, c, a=OPAQUE):
        self.rect(x, y, 1, h, c, a)

    def vgrad(self, y0, y1, top, bot):
        span = (y1 - y0 - 1) if (y1 - y0) > 1 else 1
        for y in range(max(y0, 0), min(y1, H)):
            c = mix(top, bot, min(255, (y - y0) * 255 // span))
            self.rect(0, y, W, 1, c)

    def round_rect(self, x, y, w, h, r, c, a=OPAQUE):
        if w <= 0 or h <= 0 or a == 0:
            return
        if r <= 0:
            return self.rect(x, y, w, h, c, a)
        r = min(r, w // 2, h // 2)
        self.rect(x, y + r, w, h - 2 * r, c, a)
        for yy in range(max(y, 0), min(y + h, H)):
            top, bot = yy - (y + r), yy - (y + h - 1 - r)
            if top < 0:
                dy = -top
            elif bot > 0:
                dy = bot
            else:
                continue
            for i in range(r):
                cov = corner_cov(r - i, dy, r)
                if cov:
                    aa = cov * a // 255
                    self.px(x + i, yy, c, aa)
                    self.px(x + w - 1 - i, yy, c, aa)
            self.rect(x + r, yy, w - 2 * r, 1, c, a)

    def round_frame(self, x, y, w, h, r, c, a=OPAQUE):
        if w <= 2 or h <= 2 or a == 0:
            return
        self.hline(x + r, y, w - 2 * r, c, a)
        self.hline(x + r, y + h - 1, w - 2 * r, c, a)
        self.vline(x, y + r, h - 2 * r, c, a)
        self.vline(x + w - 1, y + r, h - 2 * r, c, a)
        if r <= 0:
            return
        for yy in range(max(y, 0), min(y + h, H)):
            top, bot = yy - (y + r), yy - (y + h - 1 - r)
            if top < 0:
                dy = -top
            elif bot > 0:
                dy = bot
            else:
                continue
            for i in range(r):
                cov = corner_cov(r - i, dy, r) - corner_cov(r - i, dy, r - 1)
                if cov > 0:
                    aa = cov * a // 255
                    self.px(x + i, yy, c, aa)
                    self.px(x + w - 1 - i, yy, c, aa)

    def disc(self, cx, cy, r, c, a=OPAQUE):
        if r <= 0 or a == 0:
            return
        for y in range(max(cy - r, 0), min(cy + r + 1, H)):
            dy = y - cy
            half = isqrt32(r * r - dy * dy)
            self.rect(cx - half, y, 2 * half + 1, 1, c, a)

    # ---- type
    def text(self, x, y, s, scale, c, a=OPAQUE):
        pen = x
        for ch in s:
            ch = ch.upper()
            o = ord(ch)
            g = F5[o - 32] if 32 <= o <= 90 else F5[0]
            for col in range(FONT_W):
                bits = g[col]
                for row in range(FONT_H):
                    if bits & (1 << row):
                        self.rect(pen + col * scale, y + row * scale,
                                  scale, scale, c, a)
            pen += (FONT_W + 1) * scale

    def text_c(self, cx, y, s, scale, c, a=OPAQUE):
        self.text(cx - text_w(s, scale) // 2, y, s, scale, c, a)

    def glyph(self, x, y, rows, w, h, scale, c, a=OPAQUE, bot=None):
        for r in range(h):
            bits = rows[r]
            if not bits:
                continue
            col = c if bot is None else mix(
                c, bot, (r * 255 // (h - 1)) if h > 1 else 0)
            cc = 0
            while cc < w:
                if not (bits & (1 << cc)):
                    cc += 1
                    continue
                run = 0
                while cc + run < w and (bits & (1 << (cc + run))):
                    run += 1
                self.rect(x + cc * scale, y + r * scale, run * scale, scale,
                          col, a)
                cc += run


def text_w(s, scale):
    return len(s) * (FONT_W + 1) * scale - scale if s else 0


def text_h(scale):
    return FONT_H * scale


def face_w(s, scale):
    return len(s) * (FACE_W + 1) * scale - scale if s else 0


def tracked_w(s, scale, track):
    return text_w(s, scale) + (len(s) - 1) * track if s else 0


def dilate(rows, n):
    out = []
    for r in range(n + 2):
        m = 0
        for k in range(3):
            sr = r - 2 + k
            if 0 <= sr < n:
                m |= rows[sr]
        out.append(m | (m << 1) | (m << 2))
    return out


# --------------------------------------------------------------- widgets
class UI:
    def __init__(self, cv, t):
        self.cv, self.t = cv, t

    def ground(self):
        self.cv.vgrad(0, H, self.t['bg_top'], self.t['bg_bot'])
        if self.t.get('glow_alpha'):
            self.cv.disc(40, 30, 90, self.t['glow_a'], self.t['glow_alpha'])
            self.cv.disc(210, 215, 100, self.t['glow_b'],
                         self.t['glow_alpha'])

    def card(self, x, y, w, h):
        t, cv = self.t, self.cv
        r = t['radius']
        cv.round_rect(x, y, w, h, r, t['panel'], t['panel_alpha'])
        for i in range(4):
            a = t['edge_hi_alpha'] // 6 - i * 3
            if a <= 0:
                break
            cv.rect(x + r, y + 1 + i * 2, w - 2 * r, 2, t['edge_hi'], a)
        cv.hline(x + r, y, w - 2 * r, t['edge_hi'], t['edge_hi_alpha'])
        cv.hline(x + r, y + h - 1, w - 2 * r, t['edge_lo'], t['edge_lo_alpha'])

    def card_sel(self, x, y, w, h, sel):
        self.card(x, y, w, h)
        if sel:
            self.cv.round_frame(x, y, w, h, self.t['radius'], self.t['accent'])

    def caption(self, x, y, s):
        self.cv.text(x, y, s, CAPTION, self.t['caption'])

    def caption_c(self, cx, y, s):
        self.cv.text_c(cx, y, s, CAPTION, self.t['caption'])

    def label(self, x, y, s):
        self.cv.text(x, y, s, LABEL, self.t['caption'])

    def level(self, right, y, n):
        """Mirrors nexus_draw_level(): L in caption, number in accent."""
        num = str(n)
        lw = text_w('L', BODY)
        x = right - lw - text_w(num, BODY)
        self.cv.text(x, y, 'L', BODY, self.t['caption'])
        self.cv.text(x + lw, y, num, BODY, self.t['accent'])
        return x

    def header(self, title, score, rival=None, note=None, level=0,
               lives=0, life=None):
        """nexus_draw_game_header(): the same rect in every game."""
        cv, t = self.cv, self.t
        self.label(PAD, HUD_TITLE_Y, title)
        self.label(W - PAD - text_w('HOLD=EXIT', LABEL), HUD_TITLE_Y,
                   'HOLD=EXIT')

        x = PAD
        cv.text(x, HUD_ROW_Y, str(score), BODY, t['value'])
        if rival is not None:
            x += text_w(str(score), BODY)
            cv.text(x, HUD_ROW_Y, '-', BODY, t['caption'])
            x += text_w('-', BODY)
            cv.text(x, HUD_ROW_Y, str(rival), BODY, t['error'])

        right = W - PAD
        if level:
            right = self.level(right, HUD_ROW_Y, level)
        for i in range(lives):
            cv.disc(right - 12 - i * 12, HUD_ROW_Y + 8, 4, life)
        if lives:
            right -= 12 * lives
        if note:
            cv.text(right - 8 - text_w(note, CAPTION), HUD_ROW_Y + 4, note,
                    CAPTION, t['caption'])

    def field(self, x, y, w, h):
        """nexus_draw_field(): flat and opaque, so no glow edge crosses it."""
        self.cv.rect(x, y, w, h, self.t['track'], OPAQUE)

    def meter(self, x, y, w, h, pct, fill):
        r = h // 2
        self.cv.round_rect(x, y, w, h, r, self.t['track'], 190)
        fw = w * min(pct, 100) // 100
        if 0 < fw < h:
            fw = h
        if fw > 0:
            self.cv.round_rect(x, y, fw, h, r, fill)

    def block(self, x, y, w, h, r, c):
        """nexus_draw_block(): shadow, fill, light pooling, bevel."""
        cv = self.cv
        SH, SP = 0x0000, 0xFFFF
        cv.round_rect(x + 2, y + 2, w, h, r, SH, 80)
        cv.round_rect(x, y, w, h, r, c)
        inset = r if r > 1 else 1
        for i in range(min(3, h // 2)):
            cv.rect(x + inset, y + 1 + i, w - 2 * inset, 1, SP, 64 - i * 18)
        cv.hline(x + inset, y, w - 2 * inset, SP, 120)
        cv.vline(x, y + inset, h - 2 * inset, SP, 80)
        cv.hline(x + inset, y + h - 1, w - 2 * inset, SH, 120)
        cv.vline(x + w - 1, y + inset, h - 2 * inset, SH, 100)

    def orb(self, cx, cy, r, c):
        """nexus_draw_orb(): shadow, fill, specular up-left, dark far rim."""
        cv = self.cv
        SH, SP = 0x0000, 0xFFFF
        cv.disc(cx + 1, cy + 2, r, SH, 80)
        cv.disc(cx, cy, r, c)
        if r >= 3:
            cv.disc(cx - r // 3, cy - r // 3, r // 3, SP, 150)
        if r >= 4:
            cv.disc(cx + r // 3, cy + r // 3, r // 3, SH, 45)

    def tracked(self, cx, y, s, scale, track, c):
        x = cx - tracked_w(s, scale, track) // 2
        for ch in s:
            self.cv.text(x, y, ch, scale, c)
            x += text_w(ch, scale) + scale + track

    def wordmark(self, cx, y, s, scale, glow_a=None, lit=-1):
        """gfx_face_text_glass(), per letter, as wordmark_letters() does."""
        t, cv = self.t, self.cv
        wm = t['wordmark']
        if glow_a is None:
            glow_a = t['wordmark_glow_alpha']
        pen = cx - face_w(s, scale) // 2
        fy = y + 2 * scale
        for i, ch in enumerate(s):
            o = ord(ch.upper())
            g = F10[o - 32] if 32 <= o <= 90 else F10[0]
            top, bot, glow = wm[0], wm[1], glow_a
            if lit >= 0 and i == lit:
                # The splash walks this along the word: the lit letter goes
                # flat and its halo comes up to full - on a theme with no
                # halo the colour change carries it alone.
                top = bot = t['value']
                glow = 255 if glow else 0
            h1 = dilate(g, FACE_H)
            if glow:
                h2 = dilate(h1, FACE_H + 2)
                cv.glyph(pen - 2 * scale, fy - 2 * scale, h2, FACE_W + 4,
                         FACE_H + 4, scale, wm[2], glow // 2)
                cv.glyph(pen - scale, fy - scale, h1, FACE_W + 2, FACE_H + 2,
                         scale, wm[2], glow)
            else:
                # No halo: a one-pixel outline instead, which the face
                # overdraws down to a single lit edge.
                cv.glyph(pen - scale, fy - scale, h1, FACE_W + 2, FACE_H + 2,
                         scale, wm[2], OPAQUE)
            cv.glyph(pen, fy + 2, g, FACE_W, FACE_H, scale, wm[4], 90)
            cv.glyph(pen, fy + 1, g, FACE_W, FACE_H, scale, wm[3], 200)
            cv.glyph(pen, fy - 1, g, FACE_W, FACE_H, scale, t['edge_hi'], 220)
            cv.glyph(pen, fy, g, FACE_W, FACE_H, scale, top, OPAQUE, bot=bot)
            pen += face_w(ch, scale) + scale


def wordmark_h(scale):
    return FACE_H * scale + 4 * scale


# ----------------------------------------------------------------- PNG
def write_png(path, w, h, px565, scale=1):
    raw = b''
    for y in range(h):
        for _ in range(scale):
            row = b'\x00'
            for x in range(w):
                v = px565[y * w + x]
                r = (v >> 11) & 0x1F
                g = (v >> 5) & 0x3F
                b = v & 0x1F
                rgb = bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
                row += rgb * scale
            raw += row

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    io.open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale,
                                     8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 9))
        + chunk(b'IEND', b''))


# --------------------------------------------------------------- screens
HOME = defines(read('src/ui/home.c'),
               ['BRAND_Y', 'BRAND_H', 'ROW1_Y', 'ROW1_H', 'ROW2_Y', 'ROW2_H',
                'BAT_Y', 'BAT_H', 'INNER', 'COL_L', 'COL_W', 'COL_R', 'COL_RW',
                'TR_W', 'TR_H', 'TR_SCALE', 'ST_W', 'ST_H', 'ST_SCALE',
                'MOD_GLYPH_W', 'MOD_GLYPH_H', 'MOD_SCALE'])
G = home_glyphs()


def home(cv, t, st):
    u, K = UI(cv, t), HOME
    u.ground()

    # brand plate
    u.card(K['COL_L'], K['BRAND_Y'], CONTENT_W, K['BRAND_H'])
    u.wordmark(W // 2,
               K['BRAND_Y'] + (K['BRAND_H'] - wordmark_h(2)) // 2, 'NEXUS', 2)

    # link cluster: usb, ble, profile number, status tile
    u.card(K['COL_L'], K['ROW1_Y'], K['COL_W'], K['ROW1_H'])
    y = K['ROW1_Y'] + (K['ROW1_H'] - K['TR_H'] * K['TR_SCALE']) // 2
    usb_x = K['COL_L'] + 6
    ble_x = usb_x + K['TR_W'] * K['TR_SCALE'] + 5
    on_usb = st['on_usb']
    cv.glyph(usb_x, y, G['tr_usb_ready'] if st['usb'] else G['tr_usb_idle'],
             K['TR_W'], K['TR_H'], K['TR_SCALE'],
             t['value'] if on_usb else t['muted'])
    cv.glyph(ble_x, y, G['tr_ble'], K['TR_W'], K['TR_H'], K['TR_SCALE'],
             t['muted'] if on_usb else t['value'])

    num_x = ble_x + K['TR_W'] * K['TR_SCALE'] + 4
    cv.text(num_x, y + 1, str(st['profile'] + 1), BIG,
            t['caption'] if on_usb else t['accent'])

    if not st['bonded']:
        tile, tc = G['st_open'], t['accent']
    elif st['connected']:
        tile, tc = G['st_ok'], t['accent']
    else:
        tile, tc = G['st_down'], t['accent']
    cv.glyph(num_x + text_w('0', BIG) + 5,
             y + (K['TR_H'] * K['TR_SCALE'] - K['ST_H'] * K['ST_SCALE']) // 2,
             tile, K['ST_W'], K['ST_H'], K['ST_SCALE'], tc)

    # layer
    u.card(K['COL_R'], K['ROW1_Y'], K['COL_RW'], K['ROW1_H'])
    u.caption(K['COL_R'] + K['INNER'], K['ROW1_Y'] + 6, 'LAYER')
    name = st['layer']
    sc = BODY if text_w(name, BODY) <= K['COL_RW'] - 10 else 1
    cv.text(K['COL_R'] + 5, K['ROW1_Y'] + 22, name, sc, t['value'])

    # modifiers
    gw, gh = K['MOD_GLYPH_W'] * K['MOD_SCALE'], K['MOD_GLYPH_H'] * K['MOD_SCALE']
    slot_w, slot_h, gap = gw + 2, gh + 6, 2
    x = K['COL_L'] + 3
    my = K['ROW2_Y'] + (K['ROW2_H'] - slot_h) // 2
    u.card(K['COL_L'], K['ROW2_Y'], K['COL_W'], K['ROW2_H'])
    for i in range(4):
        on = bool(st['mods'] & (1 << i))
        cv.round_rect(x, my, slot_w, slot_h, 4,
                      t['accent'] if on else t['track'],
                      170 if on else 150)
        cv.round_frame(x, my, slot_w, slot_h, 4,
                       t['accent'] if on else t['border'],
                       OPAQUE if on else t['border_alpha'])
        cv.glyph(x + (slot_w - gw) // 2, my + (slot_h - gh) // 2, G['mods'][i],
                 K['MOD_GLYPH_W'], K['MOD_GLYPH_H'], K['MOD_SCALE'],
                 t['value'] if on else t['muted'])
        x += slot_w + gap

    # wpm
    u.card(K['COL_R'], K['ROW2_Y'], K['COL_RW'], K['ROW2_H'])
    u.caption(K['COL_R'] + K['INNER'], K['ROW2_Y'] + 6, 'WPM')
    buf = '%03d' % st['wpm']
    cv.text(K['COL_R'] + K['COL_RW'] - K['INNER'] - text_w(buf, BIG),
            K['ROW2_Y'] + 14, buf, BIG, t['accent'])

    # batteries
    for x, w, label, pct in ((K['COL_L'], K['COL_W'], 'LEFT', st['batt'][0]),
                             (K['COL_R'], K['COL_RW'], 'RIGHT', st['batt'][1])):
        u.card(x, K['BAT_Y'], w, K['BAT_H'])
        u.caption(x + K['INNER'], K['BAT_Y'] + 7, label)
        known = pct is not None
        buf = str(pct) if known else '--'
        cv.text(x + K['INNER'], K['BAT_Y'] + 19, buf, BIG,
                t['value'] if known else t['muted'])
        if known:
            cv.text(x + K['INNER'] + text_w(buf, BIG) + 4,
                    K['BAT_Y'] + 19 + text_h(BIG) - text_h(BODY), '%', BODY,
                    t['caption'])
        col = (t['error'] if known and pct <= 15 else
               t['warning'] if known and pct <= 30 else
               t['accent'] if known else t['muted'])
        u.meter(x + K['INNER'], K['BAT_Y'] + K['BAT_H'] - 11,
                w - 2 * K['INNER'], 8, pct if known else 0, col)


# ------------------------------------------------------ menus / lists
MENU = defines(read('src/ui/menus.c'),
               ['TITLE_Y', 'LIST_Y', 'ROW_H', 'ROW_PITCH', 'VIS_ROWS',
                'HINT_Y', 'INNER'])


def menu(cv, t, title, rows, cursor=0):
    """list_draw(): title, up to VIS_ROWS cards, the hint."""
    u, K = UI(cv, t), MENU
    u.ground()
    u.label(PAD, K['TITLE_Y'], title)

    if len(rows) > K['VIS_ROWS']:
        pos = '%d/%d' % (cursor + 1, len(rows))
        cv.text(PAD + CONTENT_W - text_w(pos, LABEL), K['TITLE_Y'], pos,
                LABEL, t['caption'])

    # first_visible(): keep the cursor near the middle, clamped at both ends
    first = 0
    if len(rows) > K['VIS_ROWS']:
        first = min(max(cursor - K['VIS_ROWS'] // 2, 0),
                    len(rows) - K['VIS_ROWS'])

    for v in range(min(K['VIS_ROWS'], len(rows) - first)):
        i = first + v
        label, val = rows[i]
        y = K['LIST_Y'] + v * K['ROW_PITCH']
        sel = (i == cursor)
        u.card_sel(PAD, y, CONTENT_W, K['ROW_H'], sel)
        cv.text(PAD + K['INNER'], y + (K['ROW_H'] - text_h(BODY)) // 2, label,
                BODY, t['value'] if sel else t['caption'])
        if val:
            room = CONTENT_W - 2 * K['INNER'] - 6 - text_w(label, BODY)
            vs = BODY if text_w(val, BODY) <= room else CAPTION
            cv.text(PAD + CONTENT_W - K['INNER'] - text_w(val, vs),
                    y + (K['ROW_H'] - text_h(vs)) // 2, val, vs, t['accent'])

    cv.text_c(W // 2, K['HINT_Y'], 'TAP=NEXT  HOLD=OK', LABEL, t['caption'])


# ---------------------------------------------------------------- host
HOST = defines(read('src/ui/host_screen.c'),
               ['CLOCK_Y', 'CLOCK_H', 'METER_Y', 'METER_H', 'NP_Y', 'NP_H'])


def host_meter(cv, t, x, w, label, pct):
    u, K = UI(cv, t), HOST
    u.card(x, K['METER_Y'], w, K['METER_H'])
    u.caption(x + 8, K['METER_Y'] + 7, label)
    if pct is None:
        cv.text(x + 8, K['METER_Y'] + 21, '--', VALUE, t['muted'])
        return
    cv.text(x + 8, K['METER_Y'] + 19, str(pct), VALUE, t['value'])
    u.meter(x + 8, K['METER_Y'] + K['METER_H'] - 12, w - 16, 6, pct,
            t['warning'] if pct >= 80 else t['accent'])


def host(cv, t, link=True, clock='1:32', suffix='PM', cpu=37, mem=62,
         np='Miles Davis - So What'):
    """The HOST screen, with a companion connected. Mirrors host_screen.c."""
    u, K = UI(cv, t), HOST
    u.ground()
    u.label(PAD, 6, 'HOST')
    state = 'LINKED' if link else 'NO LINK'
    cv.text(W - PAD - text_w(state, LABEL), 6, state, LABEL,
            t['accent'] if link else t['muted'])

    u.card(PAD, K['CLOCK_Y'], CONTENT_W, K['CLOCK_H'])
    if clock is None:
        u.caption_c(W // 2, K['CLOCK_Y'] + 20, 'NO HOST CLOCK')
        cv.text_c(W // 2, K['CLOCK_Y'] + 36, '--:--', BIG, t['muted'])
    else:
        u.caption_c(W // 2, K['CLOCK_Y'] + 12, 'HOST TIME')
        # Numerals big, AM/PM small on their baseline, group centred.
        tw = text_w(clock, BIG)
        sw = text_w(suffix, BODY) + 6 if suffix else 0
        x = W // 2 - (tw + sw) // 2
        cv.text(x, K['CLOCK_Y'] + 28, clock, BIG, t['value'])
        if suffix:
            cv.text(x + tw + 6,
                    K['CLOCK_Y'] + 28 + text_h(BIG) - text_h(BODY),
                    suffix, BODY, t['caption'])

    host_meter(cv, t, PAD, 106, 'CPU', cpu)
    host_meter(cv, t, PAD + 112, 110, 'RAM', mem)

    u.card(PAD, K['NP_Y'], CONTENT_W, K['NP_H'])
    u.caption(PAD + 8, K['NP_Y'] + 7, 'NOW PLAYING')
    if not link or not np:
        cv.text(PAD + 8, K['NP_Y'] + 24, '--', BODY, t['muted'])
    else:
        sc = BODY if text_w(np, BODY) <= CONTENT_W - 16 else CAPTION
        cv.text(PAD + 8, K['NP_Y'] + 24, np, sc, t['value'])


def about(cv, t):
    u = UI(cv, t)
    u.ground()
    u.card(PAD, 14, CONTENT_W, 62)
    u.caption_c(W // 2, 21, 'VAIBHAV TECH')
    u.wordmark(W // 2, 31, 'NEXUS', 2)

    u.card(PAD, 84, CONTENT_W, 85)
    u.caption_c(W // 2, 91, 'FIRMWARE')
    cv.text_c(W // 2, 102, 'V1.0.0', VALUE, t['accent'])

    for i, line in enumerate(('SMART ZMK DONGLE', 'NRF52840  ST7789',
                              'POWERED BY ZMK')):
        u.caption_c(W // 2, 131 + i * 12, line)

    u.card(PAD, 177, CONTENT_W, 46)
    u.caption_c(W // 2, 185, 'CREATED BY')
    cv.text_c(W // 2, 199, 'VAIBHAV RAJPUT', BODY, t['value'])


# ------------------------------------------------------------ game center
GC = defines(read('src/ui/game_center.c'),
             ['TITLE_Y', 'CARD_W', 'CARD_H', 'CARD_X', 'CARD_Y', 'ICON_CY',
              'NAME_Y', 'ARROW_Y', 'HINT_Y', 'HIGH_Y'])


def icon_tetris(cv, t, cx, cy):
    """A 2x2 O piece and an S, the smallest picture that says tetris."""
    for dx, dy in ((-26, -10), (-12, -10), (-26, 4), (-12, 4)):
        block(cv, cx + dx, cy + dy, 13, 2)
    for dx, dy in ((4, -10), (18, -10), (-10, 4), (4, 4)):
        block(cv, cx + dx + 10, cy + dy, 13, 4)


def icon_snake(cv, t, cx, cy):
    c = 9
    for i in range(3):
        cv.rect(cx - 26 + i * (c + 2), cy - c // 2, c, c, t['accent'])
    cv.rect(cx + 7, cy - c // 2, c, c, t['value'])
    cv.disc(cx + 26, cy, c // 2, t['warning'])


def icon_breakout(cv, t, cx, cy):
    for r in range(2):
        for c in range(4):
            cv.rect(cx - 30 + c * 16, cy - 22 + r * 10, 14, 8,
                    mix(t['accent'], t['accent_alt'], r * 160 % 256))
    cv.disc(cx + 4, cy + 2, 4, t['warning'])
    cv.round_rect(cx - 14, cy + 18, 30, 5, 2, t['value'])


# The launcher's pager counts the REGISTRY, exactly as nexus_game_count()
# does in the firmware - not the sample list below. A hand-kept total is how
# the shot ended up reading "01/03" with seven games built, which is the one
# thing the pager exists to tell you.
def registry():
    """Game names in launcher order, from game_manager.c and the games."""
    order = re.findall(r'&nexus_game_(\w+),', read('src/games/game_manager.c'))
    names = {}
    for d in sorted(os.listdir('src/games')):
        if not os.path.isdir('src/games/' + d):
            continue
        for f in sorted(os.listdir('src/games/' + d)):
            if not f.endswith('.c'):
                continue
            src = read('src/games/%s/%s' % (d, f))
            for var, body in re.findall(
                    r'const struct nexus_game nexus_game_(\w+) = \{(.*?)\n\};',
                    src, re.S):
                m = re.search(r'\.name = "([^"]*)"', body)
                if m:
                    names[var] = m.group(1)
    return [names[g] for g in order if g in names]


GAMES = registry()


def game_center(cv, t, sel=0, games=(('TETRIS', 12840, icon_tetris),
                                     ('SNAKE', 430, icon_snake),
                                     ('BREAKOUT', 1120, icon_breakout))):
    u, K = UI(cv, t), GC
    u.ground()
    u.label(PAD, K['TITLE_Y'], 'GAME CENTER')
    pos = '%02d/%02d' % (sel + 1, len(GAMES))
    cv.text(PAD + CONTENT_W - text_w(pos, LABEL), K['TITLE_Y'], pos, LABEL,
            t['caption'])

    name, high, icon = games[sel]
    u.card_sel(K['CARD_X'], K['CARD_Y'], K['CARD_W'], K['CARD_H'], True)
    icon(cv, t, W // 2, K['ICON_CY'])
    cv.text_c(W // 2, K['NAME_Y'], name, VALUE, t['accent'])

    if len(GAMES) > 1:
        cv.text(PAD + 2, K['ARROW_Y'], '<', BODY, t['caption'])
        cv.text(W - PAD - 2 - text_w('>', BODY), K['ARROW_Y'], '>', BODY,
                t['caption'])

    cv.text_c(W // 2, K['HINT_Y'], 'TAP=PLAY  x2=NEXT', LABEL, t['caption'])
    u.card(PAD, K['HIGH_Y'], CONTENT_W, 46)
    cv.text_c(W // 2, K['HIGH_Y'] + 6, 'HIGH SCORE', LABEL, t['caption'])
    cv.text_c(W // 2, K['HIGH_Y'] + 22, str(high), VALUE, t['value'])


# ---------------------------------------------------------------- tetris
# The well's size comes from tetris_core.h - the rules own the board, the
# renderer only lays it out - so both files have to be in scope here.
# widgets.h too: the well's position is expressed against NEXUS_HUD_END, so
# the shared header is what decides where Tetris starts.
TET = defines(read('src/games/tetris/tetris_core.h')
              + read('include/nexus/widgets.h')
              + read('src/games/tetris/tetris.c'),
              ['CELL', 'WELL_X', 'WELL_Y', 'FRAME_X', 'FRAME_Y', 'FRAME_W',
               'FRAME_H', 'SIDE_X', 'SIDE_W', 'SIDE_IN', 'LEVEL_Y',
               'LEVEL_H', 'LINES_Y', 'LINES_H', 'NEXT_Y', 'NEXT_H',
               'NEXT_CELL'])
HUE = [0, C(0x36E0E0), C(0xF5D442), C(0xB44DE0), C(0x4DE07A), C(0xE04D6A),
       C(0x4D7AE0), C(0xE0904D)]


def block(cv, x, y, size, piece):
    """draw_block(): body, lit top-left, shaded bottom-right, piece mark."""
    cv.rect(x, y, size, size, HUE[piece])
    cv.hline(x, y, size, 0xFFFF, 90)
    cv.vline(x, y, size, 0xFFFF, 90)
    cv.hline(x, y + size - 1, size, 0, 110)
    cv.vline(x + size - 1, y, size, 0, 110)
    if size < 8:
        return
    i = (piece - 1) % 7
    cv.rect(x + 2 + (i % 3) * ((size - 5) // 2),
            y + 2 + (i // 3) * ((size - 5) // 2), 2, 2, 0, 120)


def tetris(cv, t):
    u, K = UI(cv, t), TET
    u.ground()
    u.header('TETRIS', '12840')

    u.card(K['FRAME_X'], K['FRAME_Y'], K['FRAME_W'], K['FRAME_H'])
    u.field(K['WELL_X'], K['WELL_Y'], 10 * K['CELL'], 20 * K['CELL'])

    # a plausible stack plus a falling T
    stack = [
        (17, [0, 1, 2, 3, 6, 7, 8, 9], 6),
        (18, [0, 1, 2, 4, 5, 6, 7, 8, 9], 5),
        (19, [0, 1, 2, 3, 4, 5, 7, 8, 9], 2),
        (16, [0, 1, 7, 8, 9], 7),
        (15, [0, 8, 9], 1),
    ]
    for row, cols, piece in stack:
        for c in cols:
            block(cv, K['WELL_X'] + c * K['CELL'], K['WELL_Y'] + row * K['CELL'],
                  K['CELL'], piece)
    for c, r in ((4, 5), (3, 6), (4, 6), (5, 6)):
        block(cv, K['WELL_X'] + c * K['CELL'], K['WELL_Y'] + r * K['CELL'],
              K['CELL'], 3)

    # No SCORE card: it is in the shared header now, where every other
    # game's is. What is left is the Tetris-only half.
    for y, h, cap, val, sc, col in (
            (K['LEVEL_Y'], K['LEVEL_H'], 'LEVEL', '4', VALUE, t['accent']),
            (K['LINES_Y'], K['LINES_H'], 'LINES', '37', VALUE, t['value'])):
        u.card(K['SIDE_X'], y, K['SIDE_W'], h)
        u.label(K['SIDE_X'] + K['SIDE_IN'], y + 4, cap)
        cv.text(K['SIDE_X'] + K['SIDE_W'] - K['SIDE_IN'] - text_w(val, sc),
                y + h - 3 - text_h(sc), val, sc, col)

    u.card(K['SIDE_X'], K['NEXT_Y'], K['SIDE_W'], K['NEXT_H'])
    u.label(K['SIDE_X'] + K['SIDE_IN'], K['NEXT_Y'] + 4, 'NEXT')
    NC = K['NEXT_CELL']
    box = 4 * NC
    ox = K['SIDE_X'] + (K['SIDE_W'] - box) // 2
    oy = K['NEXT_Y'] + K['NEXT_H'] - 8 - box
    cv.round_rect(ox - 3, oy - 3, box + 6, box + 6, 4, t['track'], 200)
    for c, r in ((0, 0), (1, 0), (1, 1), (2, 1)):
        block(cv, ox + c * NC, oy + r * NC, NC, 4)


# ----------------------------------------------------------------- snake
SNK = defines(read('src/games/snake/snake.c'),
              ['COLS', 'ROWS', 'CELL', 'WELL_W', 'WELL_H', 'WELL_X', 'WELL_Y',
               'FRAME_X', 'FRAME_Y', 'FRAME_W', 'FRAME_H'])


# The static renders pass nothing and get the scene below; render_anim.py
# drives the same functions frame by frame. Defaults must not change, or CI's
# PNG diff turns every animation tweak into a docs failure.
SNAKE_BODY = [(8, 3), (8, 4), (8, 5), (8, 6), (7, 6), (6, 6), (6, 7), (6, 8),
              (6, 9), (7, 9), (8, 9), (9, 9), (10, 9), (10, 10)]


def snake(cv, t, body=None, food=(3, 12), score='120'):
    u, K = UI(cv, t), SNK
    body = SNAKE_BODY if body is None else body
    u.ground()
    u.header('SNAKE', score, note='WRAP', level=4)

    cv.round_frame(K['FRAME_X'], K['FRAME_Y'], K['FRAME_W'], K['FRAME_H'],
                   t['radius'], t['border'], t['border_alpha'])
    u.field(K['WELL_X'], K['WELL_Y'], K['WELL_W'], K['WELL_H'])

    for r, c in body:
        x = K['WELL_X'] + c * K['CELL']
        y = K['WELL_Y'] + r * K['CELL']
        cv.round_rect(x + 1, y + 1, K['CELL'] - 2, K['CELL'] - 2, 3,
                      t['accent'])
        cv.hline(x + 3, y + 1, K['CELL'] - 6, t['edge_hi'], 110)
    hr, hc = body[-1]
    cv.round_rect(K['WELL_X'] + hc * K['CELL'] + 1,
                  K['WELL_Y'] + hr * K['CELL'] + 1,
                  K['CELL'] - 2, K['CELL'] - 2, 3, t['value'])
    if food:
        fr, fc = food
        cv.disc(K['WELL_X'] + fc * K['CELL'] + K['CELL'] // 2,
                K['WELL_Y'] + fr * K['CELL'] + K['CELL'] // 2,
                K['CELL'] // 2 - 2, t['warning'])


# -------------------------------------------------------------- breakout
BRK = defines(read('src/games/breakout/breakout.c'),
              ['FIELD_X', 'FIELD_Y', 'FIELD_W', 'FIELD_H', 'BRICK_COLS',
               'BRICK_ROWS', 'BRICK_W', 'BRICK_H', 'BRICK_TOP', 'PADDLE_W',
               'PADDLE_H', 'BALL_R'])


BRK_GONE = {(0, 2), (0, 3), (1, 5), (2, 0), (2, 1), (3, 6), (4, 3), (4, 4)}


def breakout(cv, t, ball=None, paddle=None, gone=None, score='340', lives=3):
    u, K = UI(cv, t), BRK
    gone = BRK_GONE if gone is None else gone
    paddle_y = K['FIELD_Y'] + K['FIELD_H'] - 10
    u.ground()
    u.header('BREAKOUT', score, level=3, lives=lives, life=t['accent'])

    u.field(K['FIELD_X'], K['FIELD_Y'], K['FIELD_W'], K['FIELD_H'])
    cv.round_frame(K['FIELD_X'] - 2, K['FIELD_Y'] - 2, K['FIELD_W'] + 4,
                   K['FIELD_H'] + 4, t['radius'], t['border'], t['border_alpha'])

    for r in range(K['BRICK_ROWS']):
        for c in range(K['BRICK_COLS']):
            if (r, c) in gone:
                continue
            x = K['FIELD_X'] + c * K['BRICK_W']
            y = K['BRICK_TOP'] + r * K['BRICK_H']
            col = mix(t['accent'], t['accent_alt'],
                      r * 255 // (K['BRICK_ROWS'] - 1))
            cv.rect(x + 1, y + 1, K['BRICK_W'] - 2, K['BRICK_H'] - 2, col)
            cv.hline(x + 1, y + 1, K['BRICK_W'] - 2, t['edge_hi'],
                     t['edge_hi_alpha'])

    px_ = K['FIELD_X'] + (96 if paddle is None else paddle)
    cv.round_rect(px_, paddle_y, K['PADDLE_W'], K['PADDLE_H'],
                  K['PADDLE_H'] // 2, t['value'])
    bx, by = (128, 120) if ball is None else ball
    cv.disc(K['FIELD_X'] + bx, K['FIELD_Y'] + by, K['BALL_R'], t['warning'])


# ----------------------------------------------------------------- pacman
PAC = defines(read('src/games/pacman/pacman.c'),
              ['COLS', 'ROWS', 'CELL', 'WELL_W', 'WELL_H', 'WELL_X', 'WELL_Y',
               'FRAME_X', 'FRAME_Y', 'FRAME_W', 'FRAME_H'])
PAC_MAZE = re.findall(
    r'"([^"]*)"',
    read('src/games/pacman/pacman.c').split('maze_src[ROWS] = {')[1]
    .split('\n};')[0])


def pacman(cv, t, pac=(8, 6), pdir=1, ghosts=((4, 6), (3, 4), (6, 8)),
           eaten=(), score='340', lives=3, fright=False, anim=0):
    u, K = UI(cv, t), PAC
    u.ground()
    u.header('PAC-MAN', score, level=2, lives=lives, life=t['warning'])

    cv.round_frame(K['FRAME_X'], K['FRAME_Y'], K['FRAME_W'], K['FRAME_H'],
                   t['radius'], t['border'], t['border_alpha'])
    u.field(K['WELL_X'], K['WELL_Y'], K['WELL_W'], K['WELL_H'])

    CELL = K['CELL']
    for r in range(K['ROWS']):
        for c in range(K['COLS']):
            ch = PAC_MAZE[r][c]
            x = K['WELL_X'] + c * CELL
            y = K['WELL_Y'] + r * CELL
            if ch == '#':
                cv.rect(x, y, CELL, CELL, t['accent'], 70)
                cv.hline(x, y, CELL, t['edge_hi'], 40)
            elif (r, c) in eaten:
                continue
            elif ch == '.':
                cv.rect(x + CELL // 2 - 3, y + CELL // 2 - 3, 6, 6,
                        t['caption'])
            elif ch == 'o' and not (anim & 2):
                cv.disc(x + CELL // 2, y + CELL // 2, 5, t['value'])

    for i, (gr, gc) in enumerate(ghosts):
        x, y = K['WELL_X'] + gc * CELL, K['WELL_Y'] + gr * CELL
        body = t['accent_alt'] if fright else mix(
            t['error'], t['accent'], (0, 128, 255)[i % 3])
        w = h = CELL - 2
        cv.round_rect(x + 1, y + 1, w, h, w // 2, body)
        cv.rect(x + 1, y + 1 + h // 2, w, h // 2, body)
        cv.rect(x + 4, y + 5, 3, 4, 0xFFFF)
        cv.rect(x + w - 4, y + 5, 3, 4, 0xFFFF)

    # The eater, with its mouth cut back out in the well colour.
    px_ = K['WELL_X'] + pac[1] * CELL
    py_ = K['WELL_Y'] + pac[0] * CELL
    cx, cy, rad = px_ + CELL // 2, py_ + CELL // 2, CELL // 2 - 1
    cv.disc(cx, cy, rad, t['warning'])
    if not (anim & 1):
        for i in range(rad):
            half = i // 2 + 1
            if pdir == 1:
                cv.rect(cx + i, cy - half, 1, half * 2, t['track'], 220)
            elif pdir == 3:
                cv.rect(cx - i, cy - half, 1, half * 2, t['track'], 220)
            elif pdir == 0:
                cv.rect(cx - half, cy - i, half * 2, 1, t['track'], 220)
            else:
                cv.rect(cx - half, cy + i, half * 2, 1, t['track'], 220)


# ---------------------------------------------------------------- splash
SPL = defines(read('assets/splash_default.c'),
              ['DISC_CY', 'DISC_D', 'HALO_D', 'INNER_D', 'WORD_Y',
               'WORD_SCALE', 'SUB_Y', 'RULE_Y', 'RULE_W', 'BRAND_Y',
               'N_SCALE', 'HALO_OPA', 'HAIRLINE_OPA'])


def splash(cv, t, lit=-1):
    """assets/splash_default.c - fixed palette, it ignores the theme."""
    K = SPL
    BG, PURPLE, WINE = C(0x0A0912), C(0x3B2079), C(0x59203E)
    ACCENT, D_OUT, D_IN, WHITE = C(0xFF4D9E), C(0x100E18), C(0x1A1822), 0xFFFF
    # Read from the asset rather than restated: the two swapped for
    # N-11, and a copy here would have gone on drawing the old order.
    art = read('assets/splash_default.c')
    BRAND_C, PROD_C = (
        C(int(re.search(r'#define %s NEXUS_C\(0x([0-9A-Fa-f]+)u\)'
                        % n, art).group(1), 16))
        for n in ('COL_BRAND', 'COL_PRODUCT'))
    u = UI(cv, t)

    cv.rect(0, 0, W, H, BG)
    cv.disc(-58 + 84, -40 + 84, 84, PURPLE)
    cv.disc(128 + 88, 146 + 88, 88, WINE)

    cv.disc(120, K['DISC_CY'], K['HALO_D'] // 2, ACCENT, K['HALO_OPA'])
    cv.disc(120, K['DISC_CY'], K['DISC_D'] // 2, D_OUT)
    for i in range(2):
        r = K['DISC_D'] // 2 - i
        cv.round_frame(120 - r, K['DISC_CY'] - r, r * 2, r * 2, r, ACCENT)
    cv.disc(120, K['DISC_CY'], K['INNER_D'] // 2, D_IN)
    r = K['INNER_D'] // 2
    cv.round_frame(120 - r, K['DISC_CY'] - r, r * 2, r * 2, r, WHITE,
                   K['HAIRLINE_OPA'])

    ns = K['N_SCALE']
    nx = 120 - face_w('N', ns) // 2
    ny = K['DISC_CY'] - FACE_H * ns // 2
    g = F10[ord('N') - 32]
    # One colour: a one-pixel offset copy reads as channel misconvergence,
    # not as a deliberate split.
    cv.glyph(nx, ny, g, FACE_W, FACE_H, ns, ACCENT if lit == 0 else WHITE)

    u.wordmark(W // 2, K['WORD_Y'], 'NEXUS', K['WORD_SCALE'], lit=lit)
    u.tracked(W // 2, K['SUB_Y'], 'SMART ZMK DONGLE', BODY, 0, PROD_C)
    cv.rect(W // 2 - K['RULE_W'] // 2, K['RULE_Y'], K['RULE_W'], 1, ACCENT, 110)
    u.tracked(W // 2, K['BRAND_Y'], 'VAIBHAV TECH', CAPTION, 2, BRAND_C)


# ----------------------------------------------------------------- jumper
JMP = defines(read('src/games/jumper/jumper.c'),
              ['TILE', 'COLS', 'ROWS', 'VIEW_Y', 'PLAYER_W', 'PLAYER_H'])
# The first of the three boards; the doc shot is always level 1.
JMP_LEVEL = re.findall(
    r'"([^"]*)"',
    read('src/games/jumper/jumper.c')
    .split('stage_map[STAGES][ROWS] = {')[1].split('\n\t},')[0])


def jumper(cv, t, player=(7, 2), facing=1, taken=(), dead=(), score='700',
           lives=3, anim=0):
    u, K = UI(cv, t), JMP
    TILE, VY = K['TILE'], K['VIEW_Y']
    u.ground()
    u.header('JUMPER', score, level=2, lives=lives, life=t['error'])

    u.field(0, VY, W, K['ROWS'] * TILE)
    for r in range(K['ROWS']):
        y = VY + r * TILE
        for c in range(K['COLS']):
            x = c * TILE
            ch = JMP_LEVEL[r][c]
            if ch == '=':
                u.block(x, y, TILE, TILE, 0,
                        mix(t['accent'], t['accent_alt'], min(255, r * 20)))
            elif ch == 'o' and (r, c) not in taken:
                u.orb(x + TILE // 2, y + TILE // 2, 7, t['warning'])
            elif ch == 'F':
                cv.rect(x + TILE // 2 - 2, y, 4, TILE, t['value'])
                u.block(x + TILE // 2 + 2, y, 12, 10, 2, t['muted'])
            elif ch == 'E' and (r, c) not in dead:
                PW, PH = K['PLAYER_W'], K['PLAYER_H']
                ey = y + TILE - PH
                u.block(x, ey + 3, PW, PH - 3, 5, t['success'])
                cv.rect(x + 3, ey + 7, 3, 3, C(0x0A0A12))
                cv.rect(x + PW - 6, ey + 7, 3, 3, C(0x0A0A12))

    PW, PH = K['PLAYER_W'], K['PLAYER_H']
    sx = player[1] * TILE
    sy = VY + player[0] * TILE + TILE - PH
    u.block(sx, sy, PW, 7, 2, t['error'])
    u.block(sx + 1, sy + 7, PW - 2, 6, 1, t['warning'])
    u.block(sx, sy + 13, PW, 4, 1, t['accent_alt'])
    cv.rect(sx + (PW - 5 if facing > 0 else 3), sy + 8, 3, 3, 0xFFFF)


# --------------------------------------------------------------- invaders
INV = defines(read('src/games/invaders/invaders.c'),
              ['COLS', 'ROWS', 'ALIEN_W', 'ALIEN_H', 'FIELD_X', 'FIELD_Y',
               'FIELD_W', 'FIELD_H', 'CANNON_W', 'CANNON_H', 'CANNON_Y'])


def invaders(cv, t, dead=((0, 0), (0, 7), (1, 3)), fx=None, fy=None,
             cannon=None, score='430', lives=3, anim=0):
    u, K = UI(cv, t), INV
    fx = K['FIELD_X'] + 8 if fx is None else fx
    fy = K['FIELD_Y'] + 6 if fy is None else fy
    cannon = (K['FIELD_X'] + (K['FIELD_W'] - K['CANNON_W']) // 2
              if cannon is None else cannon)
    u.ground()
    u.header('INVADERS', score, level=2, lives=lives, life=t['success'])

    u.field(K['FIELD_X'], K['FIELD_Y'], K['FIELD_W'], K['FIELD_H'])
    cv.round_frame(K['FIELD_X'] - 2, K['FIELD_Y'] - 2, K['FIELD_W'] + 4,
                   K['FIELD_H'] + 4, t['radius'], t['border'], t['border_alpha'])

    AW, AH = K['ALIEN_W'], K['ALIEN_H']
    for r in range(K['ROWS']):
        for c in range(K['COLS']):
            if (r, c) in dead:
                continue
            x, y = fx + c * (AW + 4), fy + r * (AH + 6)
            body = mix(t['accent'], t['accent_alt'],
                       r * 255 // (K['ROWS'] - 1))
            u.block(x + 3, y, AW - 6, AH - 5, 4, body)
            cv.rect(x, y + AH - 6, 5, 6, body)
            cv.rect(x + AW - 5, y + AH - 9, 5, 6, body)
            cv.rect(x + 7, y + 5, 4, 4, C(0x0A0A12))
            cv.rect(x + AW - 11, y + 5, 4, 4, C(0x0A0A12))

    # three in the air, which is what holding the fire key looks like
    for dy in (40, 96, 138):
        u.block(120, K['CANNON_Y'] - dy, 4, 10, 1, t['warning'])
    u.block(cannon, K['CANNON_Y'] + 4, K['CANNON_W'], K['CANNON_H'] - 4, 2,
            t['success'])
    u.block(cannon + K['CANNON_W'] // 2 - 2, K['CANNON_Y'], 4, 6, 1,
            t['success'])


# ------------------------------------------------------------------- pong
PNG = defines(read('src/games/pong/pong.c'),
              ['FIELD_X', 'FIELD_Y', 'FIELD_W', 'FIELD_H', 'PAD_W', 'PAD_H',
               'PAD_INSET', 'BALL_R', 'HUD_Y'])


def pong(cv, t, you=None, cpu=None, ball=None, sy=3, sc=2):
    u, K = UI(cv, t), PNG
    B = K['FIELD_Y'] + K['FIELD_H']
    you = K['FIELD_Y'] + 70 if you is None else you
    cpu = K['FIELD_Y'] + 40 if cpu is None else cpu
    ball = (150, K['FIELD_Y'] + 66) if ball is None else ball
    u.ground()
    u.header('PONG', sy, rival=sc, level=3)

    cv.round_frame(K['FIELD_X'] - 2, K['FIELD_Y'] - 2, K['FIELD_W'] + 4,
                   K['FIELD_H'] + 4, t['radius'], t['border'], t['border_alpha'])
    u.field(K['FIELD_X'], K['FIELD_Y'], K['FIELD_W'], K['FIELD_H'])
    y = K['FIELD_Y'] + 6
    while y < B - 4:
        cv.rect(W // 2 - 1, y, 3, 8, t['border'], 90)
        y += 14
    u.block(K['FIELD_X'] + K['PAD_INSET'], you, K['PAD_W'], K['PAD_H'], 3,
            t['accent'])
    u.block(K['FIELD_X'] + K['FIELD_W'] - K['PAD_INSET'] - K['PAD_W'], cpu,
            K['PAD_W'], K['PAD_H'], 3, t['error'])
    u.orb(ball[0], ball[1], K['BALL_R'], t['warning'])


# ------------------------------------------------------------------ main
SETTINGS_ROWS = [('SOUND', 'ON'), ('BRIGHT', '80%'), ('THEME', 'NEXUS'),
                 ('ANIM', 'ON'), ('SPEED', 'NORMAL'), ('SNAKE WALL', 'OFF'),
                 ('SPLASH', 'IMAGE'), ('GAMES', str(len(GAMES))),
                 ('DIAG', ''),
                 ('ABOUT', ''), ('SAVE', 'OK'), ('BACK', '')]
DIAG_ROWS = [('FIRMWARE', 'V1.0.0'), ('BOARD', 'NICE_NANO'),
             ('DISPLAY', 'OK'), ('BACKLIGHT', 'OK'), ('BUZZER', 'OK'),
             ('BUTTON', 'OK'), ('HOST', 'BLE 1'), ('L/R LINK', 'OK/OK'),
             ('L/R BATT', '78/64'), ('UI STATIC', '5984B'),
             ('UPTIME', '02:14'), ('BACK', '')]
STATUS = dict(on_usb=False, usb=True, profile=0, bonded=True, connected=True,
              layer='DEFAULT', mods=0b0010, wpm=42, batt=[78, 64])


# Nearest-neighbour, integer, no smoothing.
#
# The panel is 240x240 and these were written at 1:1, so anything that showed
# them larger than life-size asked the browser to interpolate 240px up - which
# is what made them look soft and mushy rather than sharp. Upscaling by a whole
# number here keeps every panel pixel a crisp square, which is what the display
# actually looks like when you put your face near it. 3x lands at 720x720:
# comfortably larger than any sane display size, so a browser only ever scales
# these DOWN.
SHOT_SCALE = 3


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    th = {t['name']: t for t in themes()}
    nx = th['NEXUS']

    shots = [
        ('home', lambda cv: home(cv, nx, STATUS)),
        ('game-center', lambda cv: game_center(cv, nx, 0)),
        ('tetris', lambda cv: tetris(cv, nx)),
        ('snake', lambda cv: snake(cv, nx)),
        ('breakout', lambda cv: breakout(cv, nx)),
        ('pacman', lambda cv: pacman(cv, nx)),
        ('jumper', lambda cv: jumper(cv, nx)),
        ('invaders', lambda cv: invaders(cv, nx)),
        ('pong', lambda cv: pong(cv, nx)),
        ('settings', lambda cv: menu(cv, nx, 'SETTINGS', SETTINGS_ROWS, 4)),
        ('diagnostics', lambda cv: menu(cv, nx, 'DIAGNOSTICS', DIAG_ROWS, 9)),
        ('about', lambda cv: about(cv, nx)),
        ('host', lambda cv: host(cv, nx)),
        ('host-nolink', lambda cv: host(cv, nx, link=False, clock=None,
                                        suffix=None, cpu=None, mem=None,
                                        np='')),
        ('splash', lambda cv: splash(cv, nx)),
    ]
    for name, fn in shots:
        cv = Canvas()
        fn(cv)
        write_png(os.path.join(OUT, name + '.png'), W, H, cv.b, SHOT_SCALE)
        print('  %s.png' % name)

    # the same dashboard in every palette, which is what a theme gallery is
    for name, t in th.items():
        cv = Canvas()
        home(cv, t, STATUS)
        write_png(os.path.join(OUT, 'home-%s.png' % name.lower()), W, H,
                  cv.b, SHOT_SCALE)
        print('  home-%s.png' % name.lower())

    print('%d screens -> docs/images/screens/' % (len(shots) + len(th)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
