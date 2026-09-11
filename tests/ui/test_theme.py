#!/usr/bin/env python3
"""Theme table rules that are not a matter of taste.

A palette is mostly judgement, but a few things in it are decidable, and
those are the ones that get shipped wrong: a device meant for a dark ground
used on a light one, a field left at its zero value because a new theme was
added by copying an old one and deleting the lines that did not compile.

So this checks the rules that have a reason, not the colours.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')

bad = []


def ok(cond, why):
    print(('  ok    ' if cond else '  FAIL  ') + why)
    if not cond:
        bad.append(why)


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding='utf-8') as f:
        return f.read()


def lum(rgb):
    """Rec. 601 luma, 0-255. Good enough to say light from dark."""
    r, g, b = (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF
    return (299 * r + 587 * g + 114 * b) // 1000


def over(top, bot, alpha):
    """What the eye gets: a tint composited over the ground beneath it.

    The panel colour on its own says nothing - NEXUS paints a near-white
    tint at low opacity over a near-black ground and the result is dark.
    """
    out = 0
    for sh in (16, 8, 0):
        t, b = (top >> sh) & 0xFF, (bot >> sh) & 0xFF
        out |= ((t * alpha + b * (255 - alpha)) // 255) << sh
    return out


def panel_lum(t):
    """Luma of a card as drawn, over the top of the background gradient."""
    return lum(over(t['panel'], t['bg_top'], t['panel_alpha']))


def themes():
    out = []
    for blob in read('src', 'ui', 'theme.c').split('.name = ')[1:]:
        t = {'name': re.match(r'"([^"]+)"', blob).group(1)}
        for m in re.finditer(r'\.(\w+) = NEXUS_C\(0x([0-9A-Fa-f]+)u\)', blob):
            t[m.group(1)] = int(m.group(2), 16)
        for m in re.finditer(r'\.(\w+_alpha|radius) = (\d+)', blob):
            t[m.group(1)] = int(m.group(2))
        wm = re.search(r'\.wordmark = \{([^}]*)\}', blob, re.S).group(1)
        t['wordmark'] = [int(x, 16)
                         for x in re.findall(r'0x([0-9A-Fa-f]+)u', wm)]
        out.append(t)
    return out


def main():
    ts = themes()
    print('%d themes' % len(ts))

    print('\nEvery theme fills in every field')
    fields = set()
    for t in ts:
        fields |= set(t)
    for t in ts:
        missing = sorted(fields - set(t))
        ok(not missing, '%-9s declares them all%s'
           % (t['name'], '' if not missing else ' - missing %s' % missing))

    print('\nEvery card reads as a card, top to bottom')
    # A pane has to differ from the ground it sits on, and the ground is a
    # gradient - so it has to differ at BOTH ends, in the same direction.
    # Clay's pane was 5 luma darker than the top of its ground, which left
    # the upper cards as a hairline above and a hairline below with nothing
    # between. Sunset's ran from -2 at the top to -38 at the bottom: the
    # header pane vanished while the battery panes shouted.
    #
    # 8 is the floor because it is what the five themes nobody complained
    # about already clear - the rule is measured from them, not invented.
    FLOOR = 8
    for t in ts:
        top = lum(over(t['panel'], t['bg_top'], t['panel_alpha'])) \
            - lum(t['bg_top'])
        bot = lum(over(t['panel'], t['bg_bot'], t['panel_alpha'])) \
            - lum(t['bg_bot'])
        ok(min(abs(top), abs(bot)) >= FLOOR and (top > 0) == (bot > 0),
           '%-9s pane is %+d luma against the top, %+d against the bottom'
           % (t['name'], top, bot))

    print('\nA held modifier is lit in the accent, and still legible')
    # It was accent_alt at 110 - pink diluted to maroon over a dark card, the
    # one muddy colour in the palette. It is the accent at 170 now, which is
    # only safe if the glyph drawn on top of it can still be read.
    home = read('src', 'ui', 'home.c')
    ok('on ? t->accent : t->track, on ? 170 : 150' in home,
       'the fill is t->accent at 170, from the theme table')
    for t in ts:
        mid = over(t['bg_top'], t['bg_bot'], 128)   # row 2 is mid-gradient
        card = over(t['panel'], mid, t['panel_alpha'])
        fill = over(t['accent'], card, 170)
        d = abs(lum(t['value']) - lum(fill))
        ok(d >= 60, '%-9s held glyph is %d luma off its fill' % (t['name'], d))

    print('\nThe wordmark halo is a dark-ground device')
    # A halo works because a bright letter plausibly spills light into a dark
    # ground. On a light one there is nothing to spill into and the same
    # halo reads as the panel being out of focus, so the rule is decidable
    # from the background rather than per-theme taste.
    for t in ts:
        light = panel_lum(t) > 140
        glow = t.get('wordmark_glow_alpha')
        ok(glow == 0 if light else glow > 0,
           '%-9s card luma %3d -> glow %s'
           % (t['name'], panel_lum(t),
              'off, outlined instead' if glow == 0 else str(glow)))

    print('\nA theme with no halo still needs its edge')
    g = read('src', 'ui', 'gfx.c')
    ok('} else {' in g.split('if (glow_a) {')[1][:900],
       'gfx_face_text_glass draws an outline when glow_a is 0')
    for t in ts:
        if t.get('wordmark_glow_alpha'):
            continue
        # wordmark[2] is what gets drawn as that outline.
        ok(abs(lum(t['wordmark'][2]) - panel_lum(t)) > 60,
           '%-9s outlines in wordmark[2], luma %d against a card at %d'
           % (t['name'], lum(t['wordmark'][2]), panel_lum(t)))

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
