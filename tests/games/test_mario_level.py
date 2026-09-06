#!/usr/bin/env python3
"""The platformer level, checked against the physics that has to clear it.

A maze fails by walling in a dot. A platformer fails by asking for a jump the
jump cannot make - and that is worse, because it compiles, runs, looks right,
and simply cannot be finished. The player stands at the edge of a pit trying
the same leap forever.

So this re-derives the jump arc from the constants in mario.c and measures
every pit against it. If someone retunes gravity, widens a pit, or changes the
tick interval the arc is tuned to, this fails before the firmware does.

Run: python tests/games/test_mario_level.py
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
SRC = os.path.join(ROOT, 'src', 'games', 'mario', 'mario.c')

bad = []


def ok(cond, why):
    print(('  ok    ' if cond else '  FAIL  ') + why)
    if not cond:
        bad.append(why)


def main():
    src = open(SRC, encoding='utf-8').read()

    num = {k: int(v) for k, v in
           re.findall(r'^#define (TILE|LEVEL_W|LEVEL_H|GRAVITY|RUN_V|MAX_FALL|'
                      r'PLAYER_W|PLAYER_H|MAX_ENEMIES|LIVES|VIEW_Y|VIEW_H|'
                      r'RUN_HOLD_TICKS)\s+(\d+)', src, re.M)}
    jump_v = int(re.search(r'^#define JUMP_V \((-\d+)\)', src, re.M).group(1))

    block = re.search(r'level\[LEVEL_H\] = \{(.*?)\n\};', src, re.S).group(1)
    rows = re.findall(r'"([^"]*)"', block)

    print('Shape')
    ok(len(rows) == num['LEVEL_H'],
       '%d rows, LEVEL_H says %d' % (len(rows), num['LEVEL_H']))
    widths = {len(r) for r in rows}
    ok(widths == {num['LEVEL_W']},
       'every row is LEVEL_W (%d) wide, found %s'
       % (num['LEVEL_W'], sorted(widths)))
    if len(rows) != num['LEVEL_H'] or widths != {num['LEVEL_W']}:
        print('\nFAILED - cannot check further')
        return 1

    flat = ''.join(rows)
    print('\nSpawns and goal')
    ok(flat.count('P') == 1, 'exactly one spawn')
    ok(flat.count('F') == 1, 'exactly one flag')
    ok(flat.count('E') <= num['MAX_ENEMIES'],
       '%d enemies, MAX_ENEMIES is %d' % (flat.count('E'),
                                          num['MAX_ENEMIES']))

    print('\nThe jump, re-derived from the constants')
    # Exactly what step() does: vy += GRAVITY each tick, y += vy.
    v, y, peak, ticks = jump_v, 0, 0, 0
    while True:
        v += num['GRAVITY']
        y += v
        peak = min(peak, y)
        ticks += 1
        if y >= 0 or ticks > 500:
            break
    height = -peak / 256.0
    reach = ticks * num['RUN_V'] / 256.0
    print('    %d ticks airborne, %.1f px up (%.1f tiles), %.1f px across '
          '(%.1f tiles)' % (ticks, height, height / num['TILE'], reach,
                            reach / num['TILE']))
    ok(height >= num['TILE'] * 1.5,
       'the jump clears at least 1.5 tiles of height')

    print('\nEvery pit is jumpable')
    solid = set('#=')
    floor = [r for r in range(num['LEVEL_H'])
             if any(ch in solid for ch in rows[r])]
    ground_rows = floor[-2:] if len(floor) >= 2 else floor

    run, worst, where = 0, 0, -1
    for c in range(num['LEVEL_W']):
        if any(rows[r][c] in solid for r in ground_rows):
            run = 0
        else:
            run += 1
            if run > worst:
                worst, where = run, c - run + 1
    # A standing jump crosses `reach`; landing needs a tile, so allow one less.
    limit = int(reach / num['TILE']) - 1
    ok(worst <= limit,
       'widest pit is %d tiles at col %d, jump allows %d'
       % (worst, where, limit))

    print('\nNothing floats or is stranded')
    for r in range(num['LEVEL_H'] - 1):
        for c in range(num['LEVEL_W']):
            if rows[r][c] == 'E':
                ok(rows[r + 1][c] in solid,
                   'enemy at row %d col %d stands on something' % (r, c))
    pr, pc = next((r, c) for r in range(num['LEVEL_H'])
                  for c in range(num['LEVEL_W']) if rows[r][c] == 'P')
    ok(rows[pr + 1][pc] in solid, 'the spawn is over solid ground')

    # Platforms must be within a jump of something below them, or the coins
    # on them are decoration.
    print('\nPlatforms are reachable')
    max_up = int(height / num['TILE'])
    unreachable = []
    for r in range(num['LEVEL_H']):
        for c in range(num['LEVEL_W']):
            if rows[r][c] != '=':
                continue
            if c and rows[r][c - 1] == '=':
                continue                       # only test each run once
            below = [rr for rr in range(r + 1, num['LEVEL_H'])
                     if rows[rr][c] in solid]
            if below and below[0] - r <= max_up + 1:
                continue
            # or reachable from a neighbouring platform at similar height
            near = any(rows[rr][cc] in solid
                       for rr in range(max(0, r - max_up), r + max_up + 2)
                       for cc in (c - 4, c - 3, c + 4, c + 5)
                       if 0 <= cc < num['LEVEL_W'])
            if not near:
                unreachable.append((r, c))
    ok(not unreachable,
       'every brick run has something within a jump of it%s'
       % ('' if not unreachable else ' - stranded at %s' % unreachable[:3]))

    print('\nIt fits the panel')
    # VIEW_H is `(LEVEL_H * TILE)` in the C, an expression rather than a
    # literal, so it is derived here the same way rather than parsed.
    view_h = num['LEVEL_H'] * num['TILE']
    ok(num['VIEW_Y'] + view_h <= 240,
       'the view (y=%d, %d tall) ends at %d'
       % (num['VIEW_Y'], view_h, num['VIEW_Y'] + view_h))
    ok('#define VIEW_H (LEVEL_H * TILE)' in src,
       'the view is exactly as tall as the level, so it never scrolls '
       'vertically')

    print('\nHeld input, rebuilt from auto-repeat')
    repeat = int(re.search(r'config NEXUS_ACTION_REPEAT_MS\s*\n\s*int[^\n]*\n'
                           r'\s*default (\d+)',
                           open(os.path.join(ROOT, 'Kconfig'),
                                encoding='utf-8').read()).group(1))
    tick = int(re.search(r'config NEXUS_MARIO_TICK_MS\s*\n\s*int[^\n]*\n'
                         r'\s*default (\d+)',
                         open(os.path.join(ROOT, 'Kconfig'),
                              encoding='utf-8').read()).group(1))
    hold_ms = num['RUN_HOLD_TICKS'] * tick
    print('    repeat every %d ms, hold window %d ticks x %d ms = %d ms'
          % (repeat, num['RUN_HOLD_TICKS'], tick, hold_ms))
    ok(hold_ms > repeat,
       'the hold window outlasts the repeat interval, so a held key does not '
       'stutter')

    ram = (num['LEVEL_W'] * num['LEVEL_H'] + 7) // 8
    print('\nCoin bitmask %d B; level is %d B of flash, not RAM'
          % (ram, num['LEVEL_W'] * num['LEVEL_H']))

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
