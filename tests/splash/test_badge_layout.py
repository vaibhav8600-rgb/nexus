#!/usr/bin/env python3
"""The drawn splash badge - geometry, and that it stays the default.

assets/splash_default.c is the whole reason the module does not ship a
115,200-byte PNG. It draws the same composition in a few hundred bytes of
code, so the two things worth guarding are that it is still what a build
picks up by default, and that its hand-placed y positions still stack in the
right order inside 240x240 without colliding.

Run: python tests/splash/test_badge_layout.py
"""

import os
import re
import sys

PANEL = 240
FONT_W, FONT_H = 5, 7
FACE_W, FACE_H = 10, 14

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')

bad = []


def ok(cond, why):
    print(('  ok    ' if cond else '  FAIL  ') + why)
    if not cond:
        bad.append(why)


def text_w(s, scale):
    return len(s) * (FONT_W + 1) * scale - scale


def tracked_w(s, scale, track):
    return text_w(s, scale) + (len(s) - 1) * track


def face_w(s, scale):
    return len(s) * (FACE_W + 1) * scale - scale


def ink_h(scale):
    """Face plus the glow, which reaches two letter-pixels every way."""
    return FACE_H * scale + 4 * scale


def consts(src):
    out = {}
    for m in re.finditer(r'^#define\s+([A-Z0-9_]+)\s+(-?\d+)\s*(?:/\*|$)',
                         src, re.M):
        out[m.group(1)] = int(m.group(2))
    return out


def main():
    src = open(os.path.join(ROOT, 'assets', 'splash_default.c'),
               encoding='utf-8').read()
    C = consts(src)

    # Strings come from Kconfig defaults; the config repo may override them,
    # so these are the widest the module itself ships.
    kc = open(os.path.join(ROOT, 'Kconfig'), encoding='utf-8').read()

    def kdefault(sym):
        m = re.search(r'config %s\s*\n\s*string[^\n]*\n\s*default "([^"]*)"'
                      % sym, kc)
        return m.group(1) if m else ''

    brand = kdefault('NEXUS_BRAND')
    product = kdefault('NEXUS_PRODUCT')
    subtitle = kdefault('NEXUS_SUBTITLE')

    print('Drawn badge is the default')
    # An empty NEXUS_SPLASH_DEFAULT_IMAGE is what routes the build to this
    # file instead of to png2c. A path here costs 115,200 bytes of flash.
    m = re.search(r'config NEXUS_SPLASH_DEFAULT_IMAGE\s*\n(?:[^\n]*\n)*?'
                  r'\s*default "([^"]*)"', kc)
    ok(m is not None, 'NEXUS_SPLASH_DEFAULT_IMAGE has a default')
    ok(bool(m) and m.group(1) == '',
       'and it is empty, so the drawn badge is what builds')

    cm = open(os.path.join(ROOT, 'CMakeLists.txt'), encoding='utf-8').read()
    ok('assets/splash_default.c' in cm,
       'CMake still compiles the drawn badge on that path')

    print('\nStacking order')
    # The reference reads: disc, product, subtitle, divider, brand. Getting
    # this order wrong is what put the maker's name above the product's.
    order = [('disc', C['DISC_CY']), ('wordmark', C['WORD_Y']),
             ('subtitle', C['SUB_Y']), ('divider', C['RULE_Y']),
             ('brand', C['BRAND_Y'])]
    for (an, ay), (bn, by) in zip(order, order[1:]):
        ok(ay < by, '%s (%d) sits above %s (%d)' % (an, ay, bn, by))

    print('\nVertical fit')
    halo_top = C['DISC_CY'] - C['HALO_D'] // 2
    ok(halo_top >= 0, 'the halo starts on the panel (%d)' % halo_top)

    # The N is centred in the inner disc, so it must fit inside it.
    n_h = FACE_H * C['N_SCALE']
    ok(n_h <= C['INNER_D'], 'the N (%d tall) fits the inner disc (%d)'
       % (n_h, C['INNER_D']))
    ok(face_w('N', C['N_SCALE']) <= C['INNER_D'], 'and fits it across')

    disc_bot = C['DISC_CY'] + C['HALO_D'] // 2
    word_bot = C['WORD_Y'] + ink_h(C['WORD_SCALE'])
    sub_bot = C['SUB_Y'] + FONT_H
    brand_bot = C['BRAND_Y'] + FONT_H * 2

    ok(C['WORD_Y'] >= disc_bot, 'the wordmark clears the halo (%d >= %d)'
       % (C['WORD_Y'], disc_bot))
    ok(C['SUB_Y'] >= word_bot,
       'the subtitle clears the wordmark extrusion (%d >= %d)'
       % (C['SUB_Y'], word_bot))
    ok(C['RULE_Y'] > sub_bot, 'the divider clears the subtitle')
    ok(C['BRAND_Y'] > C['RULE_Y'], 'the brand clears the divider')
    ok(brand_bot <= PANEL, 'the brand ends on the panel (%d)' % brand_bot)

    print('\nHorizontal fit')
    ok(face_w(product, C['WORD_SCALE']) + 3 * C['WORD_SCALE'] <= PANEL,
       '"%s" and its outline fit across (%d)'
       % (product, face_w(product, C['WORD_SCALE']) + 3 * C['WORD_SCALE']))
    ok(tracked_w(subtitle, 1, 2) <= PANEL,
       '"%s" fits tracked (%d)' % (subtitle, tracked_w(subtitle, 1, 2)))
    ok(tracked_w(brand, 2, 2) <= PANEL,
       '"%s" fits tracked (%d)' % (brand, tracked_w(brand, 2, 2)))
    ok(C['RULE_W'] <= PANEL, 'the divider fits across')

    print('\nThe badge still draws its own type')
    # It uses _plain deliberately: the widget's strapline is NEXUS_SUBTITLE
    # at the widget's own spacing, and this composition places all three
    # lines at measured offsets from the disc instead.
    ok('nexus_draw_wordmark_plain' in src,
       'the wordmark is the plain variant, not the one with a strapline')
    ok(src.count('nexus_draw_tracked') == 2,
       'the subtitle and the brand are both tracked')

    # The monogram was an accent copy one pixel off a white one. At that
    # offset it does not read as a split, it reads as channel
    # misconvergence - a display fault, on the first frame the device shows.
    ok(src.count('gfx_face_text(nx') == 1,
       'the N is drawn once - one colour, no offset copy')
    ok('nx + 1' not in src, 'and nothing is offset by a single pixel')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
