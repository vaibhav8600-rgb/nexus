#!/usr/bin/env python3
"""The maze, read out of the C, checked for the things you cannot see.

A maze is a picture, and a picture can be wrong in ways that compile perfectly:
a dot walled off in a pocket makes the level uncompletable, and the only symptom
is a player eating all 139 of the reachable ones and nothing happening. There is
no crash and no log line.

So this flood-fills it. It also pins the geometry - the well has to fit on a
240x240 panel under the HUD - and the rules the C has to keep for the maze to
be playable at all.

Run: python tests/games/test_pacman_maze.py
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
SRC = os.path.join(ROOT, 'src', 'games', 'pacman', 'pacman.c')

bad = []


def ok(cond, why):
    print(('  ok    ' if cond else '  FAIL  ') + why)
    if not cond:
        bad.append(why)


def main():
    src = open(SRC, encoding='utf-8').read()

    C = {k: int(v) for k, v in
         re.findall(r'^#define (COLS|ROWS|CELL|WELL_Y|GHOSTS|LIVES|'
                    r'TUNNEL_ROW|HUD_Y|HUD_H)\s+(\d+)', src, re.M)}

    block = re.search(r'maze_src\[ROWS\] = \{(.*?)\n\};', src, re.S).group(1)
    maze = re.findall(r'"([^"]*)"', block)

    print('Shape')
    ok(len(maze) == C['ROWS'],
       '%d rows, ROWS says %d' % (len(maze), C['ROWS']))
    widths = {len(r) for r in maze}
    ok(widths == {C['COLS']},
       'every row is COLS (%d) wide, found %s' % (C['COLS'], sorted(widths)))
    if widths != {C['COLS']} or len(maze) != C['ROWS']:
        print('\nFAILED - cannot check further')
        return 1

    print('\nSpawns')
    flat = ''.join(maze)
    ok(flat.count('P') == 1, 'exactly one player spawn')
    ok(flat.count('G') == 1, 'exactly one chaser spawn')

    print('\nEvery dot is reachable')
    # Columns wrap - that row is the tunnel - and rows never do.
    start = next((r, c) for r in range(C['ROWS']) for c in range(C['COLS'])
                 if maze[r][c] == 'P')
    seen, stack = {start}, [start]
    while stack:
        r, c = stack.pop()
        for dr, dc in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            nr, nc = r + dr, (c + dc) % C['COLS']
            if not 0 <= nr < C['ROWS']:
                continue
            if maze[nr][nc] == '#' or (nr, nc) in seen:
                continue
            seen.add((nr, nc))
            stack.append((nr, nc))

    dots = [(r, c) for r in range(C['ROWS']) for c in range(C['COLS'])
            if maze[r][c] in '.o']
    stranded = [p for p in dots if p not in seen]
    ok(not stranded, 'all %d dots reachable from the spawn%s'
       % (len(dots), '' if not stranded else ' - stranded at %s' % stranded[:4]))

    opens = [(r, c) for r in range(C['ROWS']) for c in range(C['COLS'])
             if maze[r][c] != '#']
    ok(len(seen) == len(opens),
       'no isolated pocket (%d open cells, %d reachable)'
       % (len(opens), len(seen)))

    ghost = next((r, c) for r in range(C['ROWS']) for c in range(C['COLS'])
                 if maze[r][c] == 'G')
    ok(ghost in seen, 'the chasers can leave their spawn')

    print('\nThe tunnel')
    row = maze[C['TUNNEL_ROW']]
    ok(row[0] != '#' and row[-1] != '#',
       'row %d is open at both ends, so the wrap leads somewhere'
       % C['TUNNEL_ROW'])
    ok(all(r[0] == '#' and r[-1] == '#'
           for i, r in enumerate(maze) if i != C['TUNNEL_ROW']),
       'and it is the only row that is - otherwise the maze leaks')

    print('\nIt fits the panel')
    ww, wh = C['COLS'] * C['CELL'], C['ROWS'] * C['CELL']
    ok(ww <= 240 and wh <= 240, 'well is %dx%d' % (ww, wh))
    ok(C['WELL_Y'] >= C['HUD_Y'] + C['HUD_H'],
       'the well (y=%d) clears the HUD (ends %d)'
       % (C['WELL_Y'], C['HUD_Y'] + C['HUD_H']))
    ok(C['WELL_Y'] + wh <= 240,
       'and ends on the panel at %d' % (C['WELL_Y'] + wh))
    ok(C['CELL'] >= 10,
       'cells are %dpx - small enough and the pieces stop reading'
       % C['CELL'])

    print('\nRules the C has to keep')
    for frag, why in [
        ('g_p.fright', 'a power pellet makes the chasers edible'),
        ('score = -score', 'and frightened flips the same search, not a '
                           'second pathfinder'),
        ('d == opposite', 'a chaser never reverses, so it commits to a route'),
        ('next_dir', 'a turn is a request, taken at the next junction'),
        ('nexus_game_speed()', 'the tick scales with the live setting'),
        ('nexus_screen_invalidate_rows(HUD_Y', 'the score band repaints'),
    ]:
        ok(frag in src, why)

    # Collisions are tested before AND after the chasers move: with only one
    # test a chaser and the player swap cells and pass through each other.
    step = src.split('static void step(void)')[1].split('\n}\n')[0]
    ok(step.count('touching(&g_p.pac') >= 2,
       'collisions are tested either side of the move, so nothing passes '
       'through')

    ram = C['ROWS'] * C['COLS']
    print('\nGrid is %d bytes (%dx%d), %d chasers, %d lives'
          % (ram, C['COLS'], C['ROWS'], C['GHOSTS'], C['LIVES']))

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
