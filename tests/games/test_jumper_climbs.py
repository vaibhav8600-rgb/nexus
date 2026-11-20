#!/usr/bin/env python3
"""
Every Jumper board can be climbed by a person, not just by a perfect tick.

The player's physics are ported from src/games/jumper/jumper.c, with its own
constants read from the source. For every spot a player can stand on, every
jump with a direction held from any tick sends the player somewhere; this
collects the take-off spots that reach each higher ledge and checks the
widest run of them.

Board 2 once had a climb whose only take-off spots were 4 px wide - about one
tick of running - because the opening in the ledge above sat right over the
gap in the ledge below, leaving nowhere to stand beneath it. It looked fine
and could not be cleared.

Run: python tests/games/test_jumper_climbs.py
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
SRC = open(os.path.join(ROOT, 'src', 'games', 'jumper', 'jumper.c'),
           encoding='utf-8').read()

# The narrowest take-off a person can hit by positioning: a few run steps.
# Today's hardest climb is 20 px.
MIN_WINDOW = 10
bad = []


def ok(cond, why):
    print('  %s  %s' % ('ok  ' if cond else 'FAIL', why))
    if not cond:
        bad.append(why)


def define(name):
    return int(re.search(r'#define %s \(?(-?\d+)' % name, SRC).group(1))


TILE, COLS, ROWS, FIX = (define(n) for n in ('TILE', 'COLS', 'ROWS', 'FIX'))
GRAVITY, JUMP_V, RUN_V, MAX_FALL = (
    define(n) for n in ('GRAVITY', 'JUMP_V', 'RUN_V', 'MAX_FALL'))
PW, PH = define('PLAYER_W'), define('PLAYER_H')


def boards():
    block = re.search(r'stage_map\[STAGES\]\[ROWS\] = \{(.*?)\n\};',
                      SRC, re.S).group(1)
    rows = re.findall(r'"([^"]*)"', block)
    return [rows[i:i + ROWS] for i in range(0, len(rows), ROWS)]


def climbs(board):
    """{(feet from, feet to): widest run of take-off x} for upward jumps."""
    def solid(r, c):
        return 0 <= r < ROWS and 0 <= c < COLS and board[r][c] == '='

    def hits(x, y):
        if x < 0 or x + PW > COLS * TILE or y < 0:
            return True
        return any(solid(r, c)
                   for r in range(y // TILE, (y + PH - 1) // TILE + 1)
                   for c in range(x // TILE, (x + PW - 1) // TILE + 1))

    def land(x0, y0, d, delay):
        # step() in jumper.c: X first, then Y, then "is there floor under me".
        x, y, vy = x0 << FIX, y0 << FIX, JUMP_V
        for t in range(120):
            vx = d * RUN_V if t >= delay else 0
            if vx and not hits((x + vx) >> FIX, y >> FIX):
                x += vx
            vy = min(vy + GRAVITY, MAX_FALL)
            want = (y + vy) >> FIX
            if not hits(x >> FIX, want):
                y += vy
            else:
                top = (((want + PH) // TILE) * TILE - PH if vy > 0
                       else (want // TILE + 1) * TILE)
                y, vy = top << FIX, 0
            if t > 0 and vy >= 0 and hits(x >> FIX, (y >> FIX) + 1):
                return (y >> FIX) + PH
        return None

    found = {}
    for feet in range(TILE, ROWS * TILE + 1, TILE):
        for x in range(COLS * TILE - PW + 1):
            if hits(x, feet - PH) or not hits(x, feet - PH + 1):
                continue
            for d in (-1, 1):
                for delay in range(14):
                    to = land(x, feet - PH, d, delay)
                    if to is not None and to < feet:
                        found.setdefault((feet, to), set()).add(x)

    def widest(xs):
        xs, best, run = sorted(xs), 1, 1
        for a, b in zip(xs, xs[1:]):
            run = run + 1 if b == a + 1 else 1
            best = max(best, run)
        return best

    return {k: widest(v) for k, v in found.items()}


def main():
    print('Jumper: every climb has room to take off')
    for i, board in enumerate(boards()):
        got = climbs(board)
        ledges = sorted({f for f, _ in got} | {t for _, t in got},
                        reverse=True)
        for lo, hi in zip(ledges, ledges[1:]):
            w = got.get((lo, hi), 0)
            ok(w >= MIN_WINDOW,
               'board %d: feet %d -> %d px, take-off window %d px'
               % (i + 1, lo, hi, w))

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
