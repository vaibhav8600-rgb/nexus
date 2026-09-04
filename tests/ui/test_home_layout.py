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
            'MOD_GLYPH_W', 'MOD_GLYPH_H', 'MOD_SCALE']
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

    print('\nRow 1 - link cluster and locks')
    ts = 3
    y = C['ROW1_Y'] + 4
    icon_bot = y + text_h(ts)
    uy = y + text_h(ts) + 2
    under_bot = uy + 3
    fits('transports s%d' % ts, y, icon_bot, C['ROW1_Y'], C['ROW1_H'])
    fits('endpoint underline', uy, under_bot, C['ROW1_Y'], C['ROW1_H'])

    ly = C['ROW1_Y'] + C['ROW1_H'] - 5 - text_h(C['NEXUS_TXT_LABEL'])
    lock_bot = ly + text_h(C['NEXUS_TXT_LABEL'])
    fits('lock row', ly, lock_bot, C['ROW1_Y'], C['ROW1_H'])
    ok(ly >= under_bot + 1,
       'underline(%d) clears lock row(%d)' % (under_bot, ly))

    # link cluster horizontal
    usb_x = C['COL_L'] + C['INNER']
    ble_x = usb_x + icon_w(ts) + 9
    prof_end = ble_x + icon_w(ts) + 3 + text_w('1', ts)
    ok(prof_end <= C['COL_L'] + C['COL_W'] - 4,
       'link cluster ends %d, card edge %d' % (prof_end, C['COL_L'] + C['COL_W']))

    lx = C['COL_L'] + C['INNER']
    locks_end = lx + 43 + icon_w(C['NEXUS_TXT_LABEL'])
    ok(locks_end <= C['COL_L'] + C['COL_W'] - 2,
       'lock row ends %d, card edge %d' % (locks_end, C['COL_L'] + C['COL_W']))

    print('\nRow 1 - layer card')
    fits('LAYER label', C['ROW1_Y'] + 5,
         C['ROW1_Y'] + 5 + text_h(C['NEXUS_TXT_LABEL']), C['ROW1_Y'], C['ROW1_H'])
    fits('layer value s3', C['ROW1_Y'] + 26,
         C['ROW1_Y'] + 26 + text_h(C['NEXUS_TXT_VALUE']), C['ROW1_Y'], C['ROW1_H'])
    ok(C['ROW1_Y'] + 26 >= C['ROW1_Y'] + 5 + text_h(C['NEXUS_TXT_LABEL']),
       'layer value clears its label')

    print('\nRow 2 - modifiers')
    gw = C['MOD_GLYPH_W'] * C['MOD_SCALE']
    gh = C['MOD_GLYPH_H'] * C['MOD_SCALE']
    gap = 3
    span = 4 * gw + 3 * gap
    ok(span <= C['COL_W'] - 6,
       '4 glyphs + gaps = %d px, card inner %d' % (span, C['COL_W'] - 6))
    my = C['ROW2_Y'] + 7
    fits('mod glyphs %dx%d' % (gw, gh), my, my + gh, C['ROW2_Y'], C['ROW2_H'])
    fits('active accent bar', my + gh + 3, my + gh + 6, C['ROW2_Y'], C['ROW2_H'])
    ok(gw >= 20, 'glyphs are %dpx wide (snake draws 22)' % gw)

    print('\nRow 2 - WPM')
    fits('WPM label', C['ROW2_Y'] + 4,
         C['ROW2_Y'] + 4 + text_h(C['NEXUS_TXT_LABEL']), C['ROW2_Y'], C['ROW2_H'])
    fits('WPM value s3', C['ROW2_Y'] + 20,
         C['ROW2_Y'] + 20 + text_h(C['NEXUS_TXT_VALUE']), C['ROW2_Y'], C['ROW2_H'])
    ok(C['ROW2_Y'] + 20 >= C['ROW2_Y'] + 4 + text_h(C['NEXUS_TXT_LABEL']),
       'WPM value clears its label')
    ok(text_w('000', C['NEXUS_TXT_VALUE']) + C['INNER'] * 2 <= C['COL_RW'],
       'WPM "000" fits the card width')

    print('\nBattery cards')
    fits('battery label', C['BAT_Y'] + 5,
         C['BAT_Y'] + 5 + text_h(C['NEXUS_TXT_LABEL']), C['BAT_Y'], C['BAT_H'])
    num_top = C['BAT_Y'] + 21
    num_bot = num_top + text_h(C['NEXUS_TXT_BIG'])
    fits('battery number s4', num_top, num_bot, C['BAT_Y'], C['BAT_H'])
    ok(num_top >= C['BAT_Y'] + 5 + text_h(C['NEXUS_TXT_LABEL']),
       'battery number clears its label')
    meter_top = C['BAT_Y'] + C['BAT_H'] - 11
    fits('battery meter', meter_top, meter_top + 8, C['BAT_Y'], C['BAT_H'])
    ok(meter_top > num_bot,
       'meter(%d) clears the number(%d)' % (meter_top, num_bot))
    ok(text_w('100', C['NEXUS_TXT_BIG']) + text_w('%', C['NEXUS_TXT_BODY'])
       + 4 + 2 * C['INNER'] <= C['COL_W'],
       '"100%%" fits the narrower battery card')

    print('\nLabels are actually bigger')
    ok(C['NEXUS_TXT_LABEL'] >= 2,
       'NEXUS_TXT_LABEL = %d (%dpx tall)' % (C['NEXUS_TXT_LABEL'],
                                             text_h(C['NEXUS_TXT_LABEL'])))
    ok('nexus_draw_label' in src, 'home.c uses nexus_draw_label')
    ok('nexus_draw_caption(' not in src,
       'no scale-1 captions left on the dashboard')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
