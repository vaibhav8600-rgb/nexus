#!/usr/bin/env python3
"""The launcher's pager must count the registry, not a constant.

It read `01/03` with seven games built, which is precisely the fact the
pager exists to carry - and the README's flow diagram said EIGHT while its
own prose said seven, so the product disagreed with itself in three places.

The firmware was never wrong: nexus_game_count() is ARRAY_SIZE(games) - 1.
What drifts is everything that restates the number by hand - the renderer
that produces the screenshots, the settings row, the README. So this test
derives the count once and holds all of them to it.
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


def main():
    gm = read('src', 'games', 'game_manager.c')
    registry = re.findall(r'&nexus_game_(\w+),', gm)
    n = len(registry)

    print('Registry: %d games - %s' % (n, ', '.join(registry)))

    print('\nThe firmware derives it')
    ok('#define GAME_COUNT (ARRAY_SIZE(games) - 1)' in gm,
       'GAME_COUNT is the array length, minus the NULL terminator')
    gc = read('src', 'ui', 'game_center.c')
    ok('nexus_game_count()' in gc,
       'the pager calls nexus_game_count() rather than carrying a total')
    ok(not re.search(r'"\d+/\d+"', gc), 'and no literal "n/m" anywhere in it')
    # The digits are laid out by gfx_utoa with pad 2, so a count over 99
    # would silently widen the string and push it off the right edge.
    ok(n < 100, '%d games still fit the two-digit pager' % n)

    print('\nEverything that restates it agrees')
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import render_ui

    ok(len(render_ui.GAMES) == n,
       'the renderer reads the same registry (%d)' % len(render_ui.GAMES))
    ru = read('scripts', 'render_ui.py')
    ok("'%02d/%02d' % (sel + 1, len(GAMES))" in ru,
       'and its pager string is built from that list, not a literal')
    rows = dict(render_ui.SETTINGS_ROWS)
    ok(rows.get('GAMES') == str(n),
       'the settings GAMES row says %s' % rows.get('GAMES'))

    words = {1: 'ONE', 2: 'TWO', 3: 'THREE', 4: 'FOUR', 5: 'FIVE', 6: 'SIX',
             7: 'SEVEN', 8: 'EIGHT', 9: 'NINE', 10: 'TEN'}
    rm = read('README.md')
    for wrong in set(words.values()) - {words[n]}:
        ok('%s GAMES' % wrong not in rm,
           'README does not say %s GAMES' % wrong)
    ok('%s GAMES' % words[n] in rm,
       'README flow diagram says %s GAMES' % words[n])
    ok(rm.count('%s playable games' % words[n].lower()) == 1,
       'and its prose says the same word')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
