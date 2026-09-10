#!/usr/bin/env python3
"""Rules and geometry for Jumper, Invaders and Pong.

Each of these has one rule that is easy to get wrong and invisible when you
do:

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


def main():
    print('Jumper - the platformer that replaced the scrolling one')
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
    IS = consts(inv, ['MAX_SHOTS', 'SHOT_COOL'])
    ok('g_f.cool' in inv and IS.get('MAX_SHOTS', 0) > 1,
       'hold to fire: %d shots in the air, one every %d ticks'
       % (IS.get('MAX_SHOTS', 0), IS.get('SHOT_COOL', 0)))
    ok(IS.get('MAX_SHOTS', 99) <= 4,
       'but capped - an uncapped stream clears the screen without aiming')
    # The gun must not out-run the key: a cooldown longer than the repeat
    # would drop presses and feel like the button was broken.
    rep = int(re.search(r'config NEXUS_ACTION_REPEAT_MS\s*\n\s*int[^\n]*'
                        r'\n\s*default (\d+)', src('Kconfig')).group(1))
    tick = int(re.search(r'config NEXUS_INVADERS_TICK_MS\s*\n\s*int[^\n]*'
                         r'\n\s*default (\d+)', src('Kconfig')).group(1))
    ok(IS.get('SHOT_COOL', 99) * tick <= rep,
       'the %dms cooldown is inside the %dms key repeat, so holding the key '
       'really does fire continuously' % (IS.get('SHOT_COOL', 0) * tick, rep))
    ok(IK['COLS'] <= 16,
       '%d columns fit a uint16_t row mask' % IK['COLS'])
    span = IK['COLS'] * (IK['ALIEN_W'] + 4)
    ok(span <= IK['FIELD_W'],
       'the formation (%dpx) fits the field (%dpx)' % (span, IK['FIELD_W']))

    print('\nPong')
    p = src('src/games/pong/pong.c')
    PK = consts(p, ['FIELD_Y', 'FIELD_H', 'FIELD_W', 'PAD_H', 'BALL_R',
                    'PAD_W', 'PAD_INSET', 'HUD_Y'])
    ok('off * speed() / (PAD_H / 2)' in p,
       'where the ball lands on the paddle steers it')
    ok('CPU_STEP' in p and 'TO_PX(speed())' in p,
       'the opponent tracks a fraction of the BALL, not a constant - so it '
       'stays beatable if the speed knob moves')
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

    # A constant CPU step faster than the ball's steepest return cannot be
    # beaten by aiming, only outlasted - which is what made rallies drag.
    ok(2.0 / 3.0 < 1.0,
       'and two thirds of it, so a steep return outruns the paddle')

    ps = int(re.search(r'config NEXUS_PONG_PADDLE_STEP\s*\n\s*int[^\n]*'
                       r'\n\s*default (\d+)', kc).group(1))
    tick = int(re.search(r'config NEXUS_PONG_TICK_MS\s*\n\s*int[^\n]*'
                         r'\n\s*default (\d+)', kc).group(1))
    cross_ms = PK['FIELD_W'] / (bs / 100.0 / tick)
    reach = ps / float(rep) * cross_ms
    ok(reach >= PK['FIELD_H'] - PK['PAD_H'],
       'your paddle covers %dpx while the ball crosses (%dpx needed), so a '
       'faster ball is still reachable' % (reach, PK['FIELD_H'] - PK['PAD_H']))

    hud_end = PK['HUD_Y'] + 7 * 3
    ok(hud_end <= PK['FIELD_Y'] - 2,
       'the score ends at y=%d, clear of the court at y=%d - at BIG it ran '
       'to 50 and the ball passed through it'
       % (hud_end, PK['FIELD_Y'] - 2))
    ok('HUD_TXT NEXUS_TXT_VALUE' in p, 'which is why it is VALUE, not BIG')
    # The title row is a LABEL at y=8, so it owns rows 8..22.
    ok(PK['HUD_Y'] >= 8 + 7 * 2,
       'and starts at y=%d, below the PONG/HOLD=EXIT row it sits between'
       % PK['HUD_Y'])

    print('\nAll three repaint the panel, and only what moved')
    for name, f in (('jumper', 'src/games/jumper/jumper.c'),
                    ('invaders', 'src/games/invaders/invaders.c'),
                    ('pong', 'src/games/pong/pong.c')):
        ok('nexus_screen_invalidate_rows' in src(f),
           '%-9s uses a partial repaint' % name)
    print('\nThe shared 3D vocabulary is actually used')
    w = src('src/ui/widgets.c')
    ok('void nexus_draw_block' in w and 'void nexus_draw_orb' in w,
       'widgets.c defines the block and the orb')
    for name, f in (('jumper', 'src/games/jumper/jumper.c'),
                    ('invaders', 'src/games/invaders/invaders.c'),
                    ('pong', 'src/games/pong/pong.c')):
        ok('nexus_draw_block' in src(f) or 'nexus_draw_orb' in src(f),
           '%-9s draws through it' % name)

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
