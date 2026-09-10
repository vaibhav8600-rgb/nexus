#!/usr/bin/env python3
"""Rules and geometry for Jumper, Invaders, Pong and 2048.

Each of these has one rule that is easy to get wrong and invisible when you
do:

  2048      a tile that has just merged cannot merge again in the same move.
            Sliding 2 2 4 gives 4 4, not 8. Every implementation writes the
            greedy version once, and the board simply scores too fast.

  2048      a move that changes nothing must not spawn a tile, or pressing
            into a wall fills the board and ends the game for you.

  Jumper    grounded means "is there floor under me", not "did I collide
            going down this tick" - the latched version alternates every
            tick while standing still and eats half the jump presses.

  Invaders  the fleet turns on the LIVE extent, not its nominal width, or
            clearing an edge column makes it turn early against nothing.

  Pong      where the ball hits the paddle has to steer it, and the
            opponent must be slower than the ball or it never loses.

Run: python tests/games/test_new_games.py
"""

import io
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
bad = []


def ok(c, why):
    print(('  ok    ' if c else '  FAIL  ') + why)
    if not c:
        bad.append(why)


def src(rel):
    return io.open(os.path.join(ROOT, rel), encoding='utf-8').read()


def consts(text, names):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    raw = {k: v.strip() for k, v in
           re.findall(r'^#define\s+(\w+)\s+([^\n]+)', text, re.M)}
    out = {}
    for n in names:
        e = raw.get(n, '')
        for _ in range(8):
            nxt = re.sub(r'\b([A-Z_][A-Z0-9_]*)\b',
                         lambda m: '(' + raw[m.group(1)] + ')'
                         if m.group(1) in raw else m.group(1), e)
            if nxt == e:
                break
            e = nxt
        e = e.replace('GFX_W', '240').replace('GFX_H', '240')
        try:
            out[n] = int(eval(e))
        except Exception:
            pass
    return out


# ------------------------------------------------------------------ 2048
def slide_line(line, n=4):
    """Mirrors slide_line() in g2048.c."""
    out, merged, w, score = [0] * n, [False] * n, 0, 0
    for v in line:
        if v == 0:
            continue
        if w > 0 and out[w - 1] == v and not merged[w - 1]:
            out[w - 1] += 1
            merged[w - 1] = True
            score += 1 << out[w - 1]
        else:
            out[w] = v
            w += 1
    return out, score


def main():
    print('2048 - the merge rule')
    got, _ = slide_line([1, 1, 2, 0])
    ok(got == [2, 2, 0, 0],
       '2 2 4 slides to 4 4, not 8 - a merged tile cannot merge again')
    got, _ = slide_line([1, 1, 1, 0])
    ok(got == [2, 1, 0, 0], '2 2 2 slides to 4 2, merging the leading pair')
    got, _ = slide_line([1, 1, 1, 1])
    ok(got == [2, 2, 0, 0], '2 2 2 2 slides to 4 4, two independent merges')
    got, _ = slide_line([0, 1, 0, 2])
    ok(got == [1, 2, 0, 0], 'gaps close without merging unequal tiles')
    _, score = slide_line([1, 1, 0, 0])
    ok(score == 4, 'merging two 2s scores 4, the value of the tile made')

    g = src('src/games/g2048/g2048.c')
    ok('!merged[w - 1]' in g, 'the C still carries the merged guard')
    ok('if (!slide(d)) {' in g and 'return;' in g,
       'a move that changes nothing returns before spawning')
    ok('any_move()' in g, 'game over tests for a legal move, not a full board')

    K = consts(g, ['N', 'CELL', 'GAP', 'BOARD_W', 'BOARD_Y', 'HUD_Y', 'HUD_H'])
    print('\n2048 - it fits the panel')
    ok(K['BOARD_Y'] + K['BOARD_W'] <= 240,
       'square board %d ends at %d' % (K['BOARD_W'],
                                       K['BOARD_Y'] + K['BOARD_W']))
    ok(K['HUD_Y'] >= 6 + 14,
       'the HUD (y=%d) clears the title, which is 14 rows from y=6'
       % K['HUD_Y'])
    # Four digits at body size must fit the cell, or "2048" overflows it.
    ok(4 * 6 * 2 - 2 <= K['CELL'],
       'four digits at body size (%dpx) fit a %dpx cell'
       % (4 * 6 * 2 - 2, K['CELL']))
    ok(3 * 6 * 3 - 3 > K['CELL'],
       'and three at value size (%dpx) do not, which is why the step down '
       'happens at 100' % (3 * 6 * 3 - 3))
    ok('v >= 100 ? NEXUS_TXT_BODY' in g, 'the C steps down at 100')

    print('\nJumper - the platformer that replaced the scrolling one')
    j = src('src/games/jumper/jumper.c')
    JK = consts(j, ['TILE', 'COLS', 'ROWS', 'VIEW_Y', 'PLAYER_W', 'PLAYER_H',
                    'GRAVITY', 'RUN_V', 'MAX_FALL', 'RUN_HOLD_TICKS'])
    jv = int(re.search(r'#define JUMP_V \((-\d+)\)', j).group(1))

    v, y, peak, ticks = jv, 0, 0, 0
    while True:
        v += JK['GRAVITY']
        y += v
        peak = min(peak, y)
        ticks += 1
        if y >= 0 or ticks > 500:
            break
    height, reach = -peak / 256.0, ticks * JK['RUN_V'] / 256.0
    print('    jump %.1f px = %.2f tiles, reach %.1f tiles, %d ticks'
          % (height, height / JK['TILE'], reach / JK['TILE'], ticks))
    ok(height >= JK['TILE'] * 2, 'the jump clears two tiles')

    ok('g_j.on_ground = hits(px, py + 1' in j,
       'grounded probes one pixel down, so it cannot flicker while standing')
    ok('g_j.on_ground && g_j.vy > 0' in j,
       'and vy is zeroed while grounded')
    ok('jump_want' in j, 'a jump pressed just before landing is buffered')

    # No camera at all is the whole point: nothing scrolls, so a frame only
    # ever dirties the actors.
    ok('cam' not in j.replace('camera', ''),
       'there is no camera - the level is fixed to one screen')
    ok('VIEW_Y + lo' in j and 'VIEW_Y + hi' in j,
       'and only the band the actors occupy is invalidated')

    lit = re.findall(r'"([^"]*)"',
                     re.search(r'level\[ROWS\] = \{(.*?)\n\};', j,
                               re.S).group(1))
    ok(len(lit) == JK['ROWS'] and {len(r) for r in lit} == {JK['COLS']},
       'the level is exactly ROWS x COLS')
    flat = ''.join(lit)
    ok(flat.count('P') == 1 and flat.count('F') == 1,
       'one spawn and one flag')
    ok(flat.count('o') > 0, '%d coins to collect' % flat.count('o'))

    solid = set('=')
    floats = [(r, c) for r in range(JK['ROWS'] - 1)
              for c in range(JK['COLS'])
              if lit[r][c] == 'E' and lit[r + 1][c] not in solid]
    ok(not floats, 'no enemy stands over a gap%s'
       % ('' if not floats else ' - %s' % floats))
    pr, pc = next((r, c) for r in range(JK['ROWS'])
                  for c in range(JK['COLS']) if lit[r][c] == 'P')
    ok(lit[pr + 1][pc] in solid, 'the spawn has ground under it')

    print('\nInvaders')
    inv = src('src/games/invaders/invaders.c')
    IK = consts(inv, ['COLS', 'ROWS', 'ALIEN_W', 'FIELD_X', 'FIELD_W',
                      'MAX_BOMBS'])
    ok('int lo = FIELD_R;' in inv and 'alien_x(c) < lo' in inv,
       'the fleet turns on its LIVE extent, so clearing an edge column lets '
       'it slide further')
    ok('march_period' in inv and 'g_f.left * span / total' in inv,
       'and speeds up as it thins, from the count rather than a ramp')
    ok('g_f.shot.live' in inv,
       'one shot at a time, which is what makes a miss cost something')
    ok(IK['COLS'] <= 16,
       '%d columns fit a uint16_t row mask' % IK['COLS'])
    span = IK['COLS'] * (IK['ALIEN_W'] + 4)
    ok(span <= IK['FIELD_W'],
       'the formation (%dpx) fits the field (%dpx)' % (span, IK['FIELD_W']))

    print('\nPong')
    p = src('src/games/pong/pong.c')
    PK = consts(p, ['FIELD_Y', 'FIELD_H', 'PAD_H', 'BALL_R', 'PAD_W',
                    'PAD_INSET'])
    ok('off * speed() / (PAD_H / 2)' in p,
       'where the ball lands on the paddle steers it')
    ok('PAD_STEP / CPU_LAG' in p,
       'the opponent moves slower than the paddle can, so it is beatable')
    ok('g_g.vx > 0' in p,
       'and only reacts once the ball is coming at it')
    ok(PK['PAD_H'] < PK['FIELD_H'],
       'the paddle (%d) is shorter than the field (%d)'
       % (PK['PAD_H'], PK['FIELD_H']))

    kc = src('Kconfig')
    bs = int(re.search(r'config NEXUS_PONG_BALL_SPEED\s*\n\s*int[^\n]*\n\s*'
                       r'default (\d+)', kc).group(1))
    # At the top difficulty the ball must not cross the paddle in one tick.
    top = bs / 100.0 * (2 + 5) / 5
    print('    ball at the fastest setting: %.1f px/tick' % top)
    ok(top < PK['PAD_W'] + PK['BALL_R'],
       'which is under the paddle width plus the ball radius (%d), so it '
       'cannot tunnel through' % (PK['PAD_W'] + PK['BALL_R']))

    print('\nAll four repaint the panel, and only what moved')
    for name, f in (('jumper', 'src/games/jumper/jumper.c'),
                    ('invaders', 'src/games/invaders/invaders.c'),
                    ('pong', 'src/games/pong/pong.c')):
        ok('nexus_screen_invalidate_rows' in src(f),
           '%-9s uses a partial repaint' % name)
    ok('nexus_screen_invalidate_rows' not in g,
       '2048       repaints whole, which is right - it is turn-based and '
       'nothing moves between presses')

    print('\nThe shared 3D vocabulary is actually used')
    w = src('src/ui/widgets.c')
    ok('void nexus_draw_block' in w and 'void nexus_draw_orb' in w,
       'widgets.c defines the block and the orb')
    for name, f in (('jumper', 'src/games/jumper/jumper.c'),
                    ('invaders', 'src/games/invaders/invaders.c'),
                    ('pong', 'src/games/pong/pong.c'),
                    ('2048', 'src/games/g2048/g2048.c')):
        ok('nexus_draw_block' in src(f) or 'nexus_draw_orb' in src(f),
           '%-9s draws through it' % name)

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
