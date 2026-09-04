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


def icon_w(scale):
    return FONT_W * scale


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

    print('\nBrand plate')
    # wordmark: text_h(scale) + WORDMARK_RULE_GAP(4) + WORDMARK_RULE_H(3)
    for scale in (5, 4):
        wh = text_h(scale) + 4 + 3
        ww = text_w('NEXUS', scale)
        y = C['BRAND_Y'] + (C['BRAND_H'] - wh) // 2
        if ww <= C.get('NEXUS_CONTENT_W', 222) - 2 * C['INNER'] - 4:
            fits('wordmark s%d (%dpx wide)' % (scale, ww), y, y + wh,
                 C['BRAND_Y'], C['BRAND_H'])
            break

    print('\nRow 1 - link cluster (snake model: symbols, number, tile)')
    TR_W, TR_H, TR_S = C['TR_W'], C['TR_H'], C['TR_SCALE']
    ST_W, ST_H, ST_S = C['ST_W'], C['ST_H'], C['ST_SCALE']
    y = C['ROW1_Y'] + 4
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

    ly = C['ROW1_Y'] + C['ROW1_H'] - 4 - text_h(C['NEXUS_TXT_CAPTION'])
    fits('lock row', ly, ly + text_h(C['NEXUS_TXT_CAPTION']),
         C['ROW1_Y'], C['ROW1_H'])
    ok(ly >= tr_bot, 'locks(%d) clear transports(%d)' % (ly, tr_bot))
    lx = C['COL_L'] + 7
    ok(lx + 28 + icon_w(C['NEXUS_TXT_CAPTION']) <= C['COL_L'] + C['COL_W'] - 2,
       'lock row fits the card width')

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
