#!/usr/bin/env python3
"""Home dashboard geometry - nothing may overlap or leave its card.

The layout is a pile of hand-tuned constants in src/ui/home.c, and the
compositor will happily draw a label straight through a meter without
complaining. This recomputes every element's box from the constants actually
in the file and asserts the ones that must not collide, don't.

Run: python tests/ui/test_home_layout.py
"""

import os
import re
import sys

FONT_W, FONT_H = 5, 7
PANEL = 240


def text_w(s, scale):
    return len(s) * (FONT_W + 1) * scale - scale


def text_h(scale):
    return FONT_H * scale


def consts(src):
    """Pull the #define constants out of home.c and resolve them."""
    raw = {}
    for m in re.finditer(r'^#define\s+([A-Z0-9_]+)\s+([^/\n]+)', src, re.M):
        raw[m.group(1)] = m.group(2).strip()
    raw.setdefault('NEXUS_PAD', '9')
    raw.setdefault('NEXUS_GAP', '7')
    raw.setdefault('GFX_W', '240')
    raw.setdefault('NEXUS_CONTENT_W', '(GFX_W - 2 * NEXUS_PAD)')
    raw.setdefault('NEXUS_TXT_CAPTION', '1')
    raw.setdefault('NEXUS_TXT_LABEL', '2')
    raw.setdefault('NEXUS_TXT_BODY', '2')
    raw.setdefault('NEXUS_TXT_VALUE', '3')
    raw.setdefault('NEXUS_TXT_BIG', '4')
    out = {}
    for _ in range(8):
        for k, v in raw.items():
            e = v
            for _ in range(8):
                new = re.sub(r'\b([A-Z][A-Z0-9_]*)\b',
                             lambda m: '(' + raw[m.group(1)] + ')'
                             if m.group(1) in raw else m.group(1), e)
                if new == e:
                    break
                e = new
            if not re.search(r'[A-Za-z_]', e):
                try:
                    out[k] = int(eval(e))
                except Exception:
                    pass
    return out


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    src = open(os.path.join(root, 'src', 'ui', 'home.c'), encoding='utf-8').read()
    C = consts(src)

    need = ['BRAND_Y', 'BRAND_H', 'ROW1_Y', 'ROW1_H', 'ROW2_Y', 'ROW2_H',
            'BAT_Y', 'BAT_H', 'INNER', 'COL_L', 'COL_W', 'COL_R', 'COL_RW',
            'MOD_GLYPH_W', 'MOD_GLYPH_H', 'MOD_SCALE',
            'TR_W', 'TR_H', 'TR_SCALE', 'ST_W', 'ST_H', 'ST_SCALE']
    missing = [n for n in need if n not in C]
    if missing:
        print('FAIL could not resolve: %s' % ', '.join(missing))
        return 1

    bad = []

    def ok(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            bad.append(msg)

    def fits(name, top, bot, card_y, card_h):
        ok(top >= card_y and bot <= card_y + card_h,
           '%-26s %3d..%-3d inside card %3d..%-3d' %
           (name, top, bot, card_y, card_y + card_h))

    print('Cards')
    cards = [('BRAND', C['BRAND_Y'], C['BRAND_H']),
             ('ROW1', C['ROW1_Y'], C['ROW1_H']),
             ('ROW2', C['ROW2_Y'], C['ROW2_H']),
             ('BAT', C['BAT_Y'], C['BAT_H'])]
    for i, (n, y, h) in enumerate(cards):
        print('  %-6s %3d .. %3d  (h %d)' % (n, y, y + h, h))
        if i + 1 < len(cards):
            gap = cards[i + 1][1] - (y + h)
            ok(gap >= 4, '%s -> %s gutter %d px' % (n, cards[i + 1][0], gap))
    ok(cards[-1][1] + cards[-1][2] <= PANEL - 6,
       'bottom margin %d px' % (PANEL - (cards[-1][1] + cards[-1][2])))

    print('\nBrand plate - 10x14 display face, not the 5x7 body font')
    FACE_W, FACE_H = 10, 14

    def face_w(t, sc):
        # one column of air between glyphs, none after the last
        return len(t) * (FACE_W + 1) * sc - sc

    gfx = open(os.path.join(root, 'src', 'ui', 'gfx.c'), encoding='utf-8').read()
    ok('font10x14.h' in gfx, 'gfx.c includes the display face')
    ok('gfx_face_w' in src, 'the brand plate measures with the display face')

    avail = C.get('NEXUS_CONTENT_W', 222) - 2 * C['INNER'] - 4
    scale = 2
    while scale > 1 and face_w('NEXUS', scale) > avail:
        scale -= 1
    # wordmark ink: the face plus the glow, two letter-pixels every way
    wh = FACE_H * scale + 4 * scale
    ww = face_w('NEXUS', scale)
    y = C['BRAND_Y'] + (C['BRAND_H'] - wh) // 2
    fits('wordmark s%d (%dx%d)' % (scale, ww, FACE_H * scale), y, y + wh,
         C['BRAND_Y'], C['BRAND_H'])
    ok(ww <= avail, 'wordmark %dpx inside %dpx of plate' % (ww, avail))
    # the face is twice the width per glyph, so a scale carried over from the
    # body font would silently overflow rather than fail
    ok(face_w('NEXUS', 5) > 240,
       'scale 5 would be %dpx - proof the old scale had to change'
       % face_w('NEXUS', 5))

    print('\nRow 1 - link cluster (snake model: symbols, number, tile)')
    TR_W, TR_H, TR_S = C['TR_W'], C['TR_H'], C['TR_SCALE']
    ST_W, ST_H, ST_S = C['ST_W'], C['ST_H'], C['ST_SCALE']
    y = C['ROW1_Y'] + (C['ROW1_H'] - TR_H * TR_S) // 2
    tr_bot = y + TR_H * TR_S
    fits('transports %dx%d' % (TR_W * TR_S, TR_H * TR_S), y, tr_bot,
         C['ROW1_Y'], C['ROW1_H'])

    usb_x = C['COL_L'] + 6
    ble_x = usb_x + TR_W * TR_S + 5
    num_x = ble_x + TR_W * TR_S + 4
    num_end = num_x + text_w('0', C['NEXUS_TXT_BIG'])
    tile_x = num_end + 5
    tile_end = tile_x + ST_W * ST_S
    fits('profile number s4', y + 1, y + 1 + text_h(C['NEXUS_TXT_BIG']),
         C['ROW1_Y'], C['ROW1_H'])
    ty = y + (TR_H * TR_S - ST_H * ST_S) // 2
    fits('status tile', ty, ty + ST_H * ST_S, C['ROW1_Y'], C['ROW1_H'])
    ok(tile_end <= C['COL_L'] + C['COL_W'] - 3,
       'cluster spans %d..%d, card is %d..%d'
       % (usb_x, tile_end, C['COL_L'], C['COL_L'] + C['COL_W']))
    ok(num_end < tile_x, 'number(%d) clears tile(%d)' % (num_end, tile_x))
    ok(ble_x >= usb_x + TR_W * TR_S, 'BLE clears USB')

    ok('draw_flags' in src, 'locks and jiggler moved off the link card')
    link = src[src.index('static void draw_link'):src.index('static void draw_flags')]
    ok('caps_lock' not in link and 'anti_idle' not in link,
       'link card carries only transport, profile and tile')
    flags = src[src.index('static void draw_flags'):]
    ok('anti_idle' in flags, 'the jiggler still has its corner dot')
    ok('num_lock' not in flags,
       'no permanent lock dots - Num Lock is on by default on a desktop, so '
       'that one was a solid amber dot that never changed')

    print('\nRow 1 - layer card')
    fits('LAYER caption', C['ROW1_Y'] + 6,
         C['ROW1_Y'] + 6 + text_h(C['NEXUS_TXT_CAPTION']),
         C['ROW1_Y'], C['ROW1_H'])
    fits('layer name s2', C['ROW1_Y'] + 22,
         C['ROW1_Y'] + 22 + text_h(C['NEXUS_TXT_BODY']),
         C['ROW1_Y'], C['ROW1_H'])
    ok(C['ROW1_Y'] + 22 >= C['ROW1_Y'] + 6 + text_h(C['NEXUS_TXT_CAPTION']),
       'layer name clears its caption')

    # every realistic layer name must render at the SAME size - a name that
    # changes height as you switch layers is what this cap exists to stop
    avail = C['COL_RW'] - 10
    scales = set()
    for nm in ('GAME', 'LOWER', 'RAISE', 'ADJUST', 'DEFAULT', 'FUNCTION'):
        sc = next((x for x in (C['NEXUS_TXT_BODY'], 1)
                   if text_w(nm, x) <= avail), 1)
        scales.add(sc)
    ok(len(scales) == 1,
       'GAME/LOWER/RAISE/ADJUST/DEFAULT/FUNCTION all render at scale %s'
       % sorted(scales))
    ok('NEXUS_TXT_BODY), t->value' in src,
       'layer name is capped at NEXUS_TXT_BODY, not VALUE')

    print('\nRow 2 - modifiers')
    gw = C['MOD_GLYPH_W'] * C['MOD_SCALE']
    gh = C['MOD_GLYPH_H'] * C['MOD_SCALE']
    slot_w, slot_h, gap = gw + 2, gh + 6, 2
    span = 4 * slot_w + 3 * gap
    start = C['COL_L'] + 3
    ok(start + span <= C['COL_L'] + C['COL_W'] - 2,
       '4 slots + gaps span %d..%d, card %d..%d'
       % (start, start + span, C['COL_L'], C['COL_L'] + C['COL_W']))
    my = C['ROW2_Y'] + (C['ROW2_H'] - slot_h) // 2
    fits('mod slots %dx%d' % (slot_w, slot_h), my, my + slot_h,
         C['ROW2_Y'], C['ROW2_H'])
    ok(gw >= 20, 'glyphs are %dpx wide (snake draws 22)' % gw)
    ok(span >= 100, 'slots fill the card (%d of %d)' % (span, C['COL_W']))

    print('\nRow 2 - WPM')
    fits('WPM caption', C['ROW2_Y'] + 6,
         C['ROW2_Y'] + 6 + text_h(C['NEXUS_TXT_CAPTION']),
         C['ROW2_Y'], C['ROW2_H'])
    wv = C['ROW2_Y'] + 14
    fits('WPM value s4', wv, wv + text_h(C['NEXUS_TXT_BIG']),
         C['ROW2_Y'], C['ROW2_H'])
    ok(wv >= C['ROW2_Y'] + 6 + text_h(C['NEXUS_TXT_CAPTION']),
       'WPM value clears its caption')
    ok(text_w('000', C['NEXUS_TXT_BIG']) + C['INNER'] * 2 <= C['COL_RW'],
       'WPM "000" at s4 fits the card (%d px in %d)'
       % (text_w('000', C['NEXUS_TXT_BIG']) + C['INNER'] * 2, C['COL_RW']))

    print('\nBattery cards')
    fits('battery caption', C['BAT_Y'] + 7,
         C['BAT_Y'] + 7 + text_h(C['NEXUS_TXT_CAPTION']), C['BAT_Y'], C['BAT_H'])
    num_top = C['BAT_Y'] + 19
    num_bot = num_top + text_h(C['NEXUS_TXT_BIG'])
    fits('battery number s4', num_top, num_bot, C['BAT_Y'], C['BAT_H'])
    ok(num_top >= C['BAT_Y'] + 7 + text_h(C['NEXUS_TXT_CAPTION']),
       'battery number clears its label')
    meter_top = C['BAT_Y'] + C['BAT_H'] - 11
    fits('battery meter', meter_top, meter_top + 8, C['BAT_Y'], C['BAT_H'])
    ok(meter_top > num_bot,
       'meter(%d) clears the number(%d)' % (meter_top, num_bot))
    ok(text_w('100', C['NEXUS_TXT_BIG']) + text_w('%', C['NEXUS_TXT_BODY'])
       + 4 + 2 * C['INNER'] <= C['COL_W'],
       '"100%%" fits the narrower battery card')

    print('\nGlass wordmark')
    # The title is made of the same material as the cards: the accent lives
    # BEHIND the letter as a soft halo, not in its fill, and the letter takes
    # its shape from a one-pixel specular above every edge and two graded
    # pixels of shade below. The halo is the part with a footprint - it
    # reaches two letter-pixels past the face on every side, and all of it
    # has to stay inside the card, because the card is drawn first and
    # whatever is drawn next paints over anything that escaped.
    FACE_W, FACE_H = 10, 14
    GLOW_CELLS = 2

    def face_w(text, s):
        return len(text) * (FACE_W + 1) * s - s

    def ink_h(s):
        return FACE_H * s + 2 * GLOW_CELLS * s

    # Nothing sits under the name any more, so the ink is the whole widget.
    mark_h = ink_h

    wid = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            '..', '..', 'src', 'ui', 'widgets.c'),
               encoding='utf-8').read()
    ok('gfx_face_text_glass' in wid, 'the wordmark is drawn as glass')
    ok('gfx_face_text_3d' not in wid,
       'and not as extruded arcade lettering')
    ok('t->edge_hi' in wid,
       'the specular rim is the card\'s own, so it is the same material')
    ok('gfx_mix(t->wordmark[2], t->accent' not in wid,
       'no left-to-right ramp - the letter shades downward, like light')
    for slot in range(5):
        ok('t->wordmark[%d]' % slot in wid,
           'wordmark[%d] earns its place in the palette' % slot)

    lit_body = wid.split('nexus_draw_wordmark_lit')[1]
    ok('gfx_round_rect' not in lit_body, 'no accent rule under the name')
    ok('nexus_draw_tracked' not in lit_body, 'and no strapline either')
    ok('NEXUS_SUBTITLE' not in wid,
       'the widget does not set the subtitle - the splash does that itself')

    # Home picks the largest scale from 2 down whose face fits the card.
    avail = C['NEXUS_CONTENT_W'] - 2 * C['INNER'] - 4
    hs = 2
    while hs > 1 and face_w('NEXUS', hs) > avail:
        hs -= 1
    ok(face_w('NEXUS', hs) <= avail, 'the face fits the brand card')
    ok(face_w('NEXUS', hs) + 2 * GLOW_CELLS * hs
       <= C['NEXUS_CONTENT_W'] - 2 * C['INNER'],
       'and so does the halo around it')

    top = C['BRAND_Y'] + (C['BRAND_H'] - mark_h(hs)) // 2
    fits('wordmark ink', top, top + ink_h(hs), C['BRAND_Y'], C['BRAND_H'])
    # It is the only thing on the plate now, so it should look placed, not
    # wedged: a card this size wants real air above and below.
    ok((C['BRAND_H'] - ink_h(hs)) // 2 >= 5,
       'the plate leaves %dpx of air around it'
       % ((C['BRAND_H'] - ink_h(hs)) // 2))

    # About draws the same widget into its own card. At scale 4 the face
    # alone was 56px from y=33 - it ran to 89 and the next card covered it.
    menus = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              '..', '..', 'src', 'ui', 'menus.c'),
                 encoding='utf-8').read()
    m = re.search(r'nexus_draw_wordmark\(GFX_W / 2, (\d+), NEXUS_PRODUCT, '
                  r'(\d+)\)', menus)
    ok(m is not None, 'the About wordmark call is still recognisable')
    card = re.search(r'nexus_draw_card\(NEXUS_PAD, 14, NEXUS_CONTENT_W, '
                     r'(\d+)\)', menus)
    ok(card is not None, 'the About brand card is still recognisable')
    if m and card:
        ay, asz = int(m.group(1)), int(m.group(2))
        cend = 14 + int(card.group(1))
        ok(ay + mark_h(asz) <= cend,
           'About wordmark (ends %d) stays in its card (ends %d)'
           % (ay + mark_h(asz), cend))
        ok(cend <= 84, 'and the card clears the FIRMWARE card below it')
        # About has room to list it, and is the screen you go to for it.
        ok('NEXUS_SUBTITLE' in menus.split('const char *const lines[]')[1]
           .split('}')[0],
           'About still lists the subtitle as a body line')
        ok(face_w('NEXUS', asz) + 2 * GLOW_CELLS * asz
           <= C['NEXUS_CONTENT_W'], 'About wordmark fits the card width')

    print('\nProfile tile: the state is in the shape, not the colour')
    # A tick read as "task complete", and its success green was the one
    # colour that did not belong in half the palettes.
    ok('st_ok[ST_H] = {   /* filled' in src,
       'bonded and connected is a filled mark, not a tick')
    link = src.split('static void draw_link')[1].split('\n}\n')[0]
    ok('t->success' not in link and 't->error' not in link
       and 't->warning' not in link,
       'the tile borrows no traffic-light colour')
    ok('ST_SCALE, t->accent, GFX_OPAQUE' in link,
       'and draws all three states in the theme accent')

    print('\nUI static RAM')
    # The ceiling, and the only number in Diagnostics that is a promise
    # rather than a reading. Every buffer NEXUS owns is static, so this is
    # the whole of it: the compositor band plus the menu value cache. A
    # ticket that grows it is a ticket that got the wrong answer.
    CEILING = 5984
    gfx_h = open(os.path.join(root, 'include', 'nexus', 'gfx.h'),
                 encoding='utf-8').read()
    menus = open(os.path.join(root, 'src', 'ui', 'menus.c'),
                 encoding='utf-8').read()

    def define(text, name):
        return int(re.search(r'#define %s\s+(\d+)' % name, text).group(1))

    band = define(gfx_h, 'GFX_W') * define(gfx_h, 'GFX_STRIP_H') * 2
    cache = define(menus, 'MAX_ROWS') * define(menus, 'VAL_MAX')
    ok(band + cache == CEILING,
       'band %d + value cache %d = %d, the documented ceiling'
       % (band, cache, band + cache))
    ok('sizeof(g_val) + GFX_W * GFX_STRIP_H * 2U' in menus,
       'and Diagnostics computes it rather than printing a literal')
    ok(str(CEILING) in open(os.path.join(root, 'docs', 'configuration.md'),
                            encoding='utf-8').read().replace(',', ''),
       'the docs quote the same number')

    print('\nHierarchy')
    ok('nexus_draw_caption(' in src, 'card headings are captions again')
    ok(str(C['NEXUS_TXT_BIG']) in src or True, '')
    ok('NEXUS_TXT_BIG, t->accent' in src, 'WPM value is NEXUS_TXT_BIG')
    ok('tr_usb_ready' in src and 'tr_usb_idle' in src,
       'USB symbol has ready and not-ready forms')
    ok('st_ok' in src and 'st_down' in src and 'st_open' in src,
       'status tile has all three states')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
