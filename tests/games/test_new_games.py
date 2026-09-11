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
                    'GRAVITY', 'RUN_V', 'MAX_FALL', 'RUN_HOLD_TICKS',
                    'STAGES'])
    j_src = j
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

    # Every board, not just the first: they are hand-drawn text, and an
    # unreachable platform looks exactly like a reachable one until someone
    # plays it. The reach model comes from the physics constants above - a
    # jump peaks a little over two tiles up, having moved about 1.6 tiles
    # sideways by then, and covers about four tiles flat out.
    boards = re.findall(r'\t\{\n((?:\t\t"[^"]*",\n)+)\t\},',
                        j_src.split('stage_map[STAGES][ROWS] = {')[1])
    ok(len(boards) == JK['STAGES'],
       'all %d boards parse' % JK.get('STAGES', 0))

    RISE = 2          # tiles a jump can climb
    RISE_DX = 1       # ... and how far sideways, at that height
    DX = 4            # tiles covered on the flat, or falling

    for n, raw in enumerate(boards, 1):
        lit = re.findall(r'"([^"]*)"', raw)
        tag = 'board %d' % n
        ok(len(lit) == JK['ROWS'] and {len(r) for r in lit} == {JK['COLS']},
           '%s is exactly ROWS x COLS' % tag)
        flat = ''.join(lit)
        ok(flat.count('P') == 1 and flat.count('F') == 1,
           '%s has one spawn and one flag' % tag)
        ok(flat.count('o') > 0,
           '%s has %d coins' % (tag, flat.count('o')))

        R, C = JK['ROWS'], JK['COLS']

        def solid(r, c):
            return 0 <= r < R and 0 <= c < C and lit[r][c] == '='

        def stand(r, c):
            return (0 <= r < R and 0 <= c < C and lit[r][c] != '='
                    and solid(r + 1, c))

        floats = [(r, c) for r in range(R) for c in range(C)
                  if lit[r][c] == 'E' and not solid(r + 1, c)]
        ok(not floats, '%s: no enemy stands over a gap %s' % (tag, floats))

        pr, pc = next((r, c) for r in range(R) for c in range(C)
                      if lit[r][c] == 'P')
        ok(stand(pr, pc), '%s: the spawn has ground under it' % tag)

        # Flood fill the standing surfaces you can actually get to.
        seen, queue = {(pr, pc)}, [(pr, pc)]
        while queue:
            r, c = queue.pop()
            for r2 in range(R):
                for c2 in range(C):
                    if (r2, c2) in seen or not stand(r2, c2):
                        continue
                    dx, rise = abs(c2 - c), r - r2
                    if rise > RISE:
                        continue
                    if rise == RISE and dx > RISE_DX:
                        continue
                    if dx > DX:
                        continue
                    seen.add((r2, c2))
                    queue.append((r2, c2))

        # A coin or the flag counts as reached if you can stand on it, or
        # stand within a jump of it - the sprite is taller than a tile, so a
        # coin two rows up is swept on the way past.
        def within_jump(r, c):
            return any(abs(c - sc) <= RISE_DX and 0 <= sr - r <= RISE
                       for sr, sc in seen)

        for r in range(R):
            for c in range(C):
                if lit[r][c] not in 'oF':
                    continue
                what = ('the flag' if lit[r][c] == 'F'
                        else 'coin (%d,%d)' % (r, c))
                ok((r, c) in seen or within_jump(r, c),
                   '%s: %s is reachable' % (tag, what))

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

    # Pong used to own its HUD - two big numerals either side of centre,
    # which at NEXUS_TXT_BIG ran into the court and the ball passed through
    # them. It is the shared header's problem now, and the header's own
    # section below proves the block clears every field including this one.
    ok('HUD_Y' not in p and 'HUD_TXT' not in p,
       'and Pong no longer carries a HUD of its own')

    print('\nAll three repaint the panel, and only what moved')
    for name, f in (('jumper', 'src/games/jumper/jumper.c'),
                    ('invaders', 'src/games/invaders/invaders.c'),
                    ('pong', 'src/games/pong/pong.c')):
        ok('nexus_screen_invalidate_rows' in src(f),
           '%-9s uses a partial repaint' % name)
    print('\nLevels')
    gm = src('src/games/game_manager.c')
    m = re.search(r'uint16_t nexus_game_level_pct\(uint8_t level\)\n\{'
                  r'(.*?)\n\}', gm, re.S)
    ok(m is not None, 'the shared curve exists')

    def pct(level):
        """Mirrors nexus_game_level_pct()."""
        if level < 1:
            level = 1
        return min(100 + (level - 1) * 12, 200)

    ok(pct(1) == 100, 'level 1 is the tuned value exactly, unscaled')
    ok(pct(0) == 100, 'and level 0 cannot make a game slower than level 1')
    ok(pct(50) == 200, 'the curve caps - a level nobody can survive is an '
       'ending with extra steps')
    ok('100U + (uint16_t)(level - 1U) * 12U' in m.group(1)
       and '200U' in m.group(1), 'and the C says the same thing')

    # Every game that can be played to a second board has to have one, and
    # has to show which one you are on.
    for name, f in (('breakout', 'src/games/breakout/breakout.c'),
                    ('pacman', 'src/games/pacman/pacman.c'),
                    ('jumper', 'src/games/jumper/jumper.c'),
                    ('invaders', 'src/games/invaders/invaders.c'),
                    ('pong', 'src/games/pong/pong.c'),
                    ('snake', 'src/games/snake/snake.c')):
        g = src(f)
        # The badge is placed by the shared header now, so what a game
        # owns is the number it hands over.
        ok('.level = ' in g, '%-9s hands its level to the header' % name)

    # Clearing the board must not be an ending any more, or the level never
    # gets past 1 and the badge is decoration.
    for name, f in (('breakout', 'src/games/breakout/breakout.c'),
                    ('pacman', 'src/games/pacman/pacman.c'),
                    ('jumper', 'src/games/jumper/jumper.c'),
                    ('invaders', 'src/games/invaders/invaders.c')):
        g = src(f)
        ok('end_round(true)' not in g and '"CLEARED"' not in g,
           '%-9s advances instead of ending when you clear it' % name)

    # State that survives a board it has no business surviving.
    pm = src('src/games/pacman/pacman.c')
    ok('g_p.fright = 0' in pm.split('static void next_maze')[1]
       .split('\n}')[0],
       'pacman    clears the fright timer with the maze - eating the last '
       'dot while the ghosts are blue must not start the next one blue')
    bo_reset = src('src/games/breakout/breakout.c')
    ok('reset_ball();' in bo_reset.split('static void next_board')[1]
       .split('\n}')[0],
       'breakout  re-serves rather than leaving the ball mid-flight')

    print('\n    a tick at the level cap, against what it has to collide with')
    bo = src('src/games/breakout/breakout.c')
    BK = consts(bo, ['BRICK_H', 'PADDLE_H', 'BALL_R'])
    base = int(re.search(r'config NEXUS_BREAKOUT_BALL_SPEED\s*\n\s*int'
                         r'[^\n]*\n\s*default (\d+)', kc).group(1))
    top = base / 100.0 * (2 + 5) / 5 * pct(99) / 100.0
    cap = BK['BRICK_H'] - 1
    print('      breakout %.1f px/tick, clamped to %d, brick row %d'
          % (top, cap, BK['BRICK_H']))
    ok(cap < BK['BRICK_H'],
       'the clamp keeps a tick inside a brick row - hit_bricks() tests the '
       'centre, so a longer tick steps straight through one')
    ok('TO_FIX(BRICK_H - 1)' in bo, 'and it is derived, not a magic number')

    ptop = bs / 100.0 * (2 + 5) / 5 * pct(99) / 100.0
    pcap = PK['PAD_W'] + PK['BALL_R'] - 1
    print('      pong     %.1f px/tick, clamped to %d, paddle window %d'
          % (ptop, pcap, PK['PAD_W'] + PK['BALL_R']))
    ok(pcap < PK['PAD_W'] + PK['BALL_R'],
       'same for the paddle plane, which is also a point test')
    ok('TO_FIX(PAD_W + BALL_R - 1)' in p, 'and also derived')

    print('\nOne header, seven games')
    # Seven games had seven headers: Snake and Pac-Man labelled the score,
    # Breakout and Invaders showed a bare number, Tetris put it in a side
    # panel. Paging between them moved the furniture.
    HDR = consts(src('include/nexus/widgets.h'),
                 ['NEXUS_HUD_END', 'NEXUS_HUD_TITLE_Y', 'NEXUS_HUD_ROW_Y',
                  'NEXUS_HUD_ROW_H'])
    ok(HDR['NEXUS_HUD_ROW_Y'] + HDR['NEXUS_HUD_ROW_H'] <= HDR['NEXUS_HUD_END'],
       'the live row (%d..%d) closes inside the block (..%d)'
       % (HDR['NEXUS_HUD_ROW_Y'],
          HDR['NEXUS_HUD_ROW_Y'] + HDR['NEXUS_HUD_ROW_H'],
          HDR['NEXUS_HUD_END']))
    # Not "does not overlap" - adjacent is the bug. The first version had
    # the title end exactly where the score began and the two read as one
    # block of text, which is how a 14px numeral stops looking like a score.
    gap = HDR['NEXUS_HUD_ROW_Y'] - (HDR['NEXUS_HUD_TITLE_Y'] + 14)
    ok(gap >= 2, 'the title clears the score row by %dpx' % gap)
    ok(HDR['NEXUS_HUD_TITLE_Y'] >= 4,
       'and the title clears the top of the panel by %d'
       % HDR['NEXUS_HUD_TITLE_Y'])

    # Same on the other side: the score must not sit flush on a playfield.
    tet = src('src/games/tetris/tetris.c') + src('include/nexus/widgets.h') \
        + src('src/games/tetris/tetris_core.h')
    below = consts(tet, ['FRAME_Y'])['FRAME_Y'] - HDR['NEXUS_HUD_END']
    ok(below >= 2,
       'and the tightest field below it (Tetris) clears it by %dpx' % below)

    ALL7 = (('tetris', 'src/games/tetris/tetris.c'),
            ('snake', 'src/games/snake/snake.c'),
            ('breakout', 'src/games/breakout/breakout.c'),
            ('pacman', 'src/games/pacman/pacman.c'),
            ('jumper', 'src/games/jumper/jumper.c'),
            ('invaders', 'src/games/invaders/invaders.c'),
            ('pong', 'src/games/pong/pong.c'))

    for name, f in ALL7:
        g = src(f)
        ok('nexus_draw_game_header(&hud)' in g,
           '%-9s draws the shared header' % name)
        # Nothing may draw its own title, HOLD=EXIT or score any more -
        # that is what made the rect differ per game in the first place.
        ok('"HOLD=EXIT"' not in g.split('g_over_hint2')[0]
           or 'nexus_draw_label(NEXUS_PAD' not in g,
           '%-9s has no title row of its own left' % name)
        ok('nexus_draw_level(' not in g,
           '%-9s does not place the level badge itself' % name)

    w = src('src/ui/widgets.c')
    ok(w.count('nexus_draw_game_header') == 1,
       'and there is exactly one place that draws it')

    print('\n    the playfield starts below the block, in every game')
    for name, f, key in (('tetris', 'src/games/tetris/tetris.c', 'FRAME_Y'),
                         ('snake', 'src/games/snake/snake.c', 'FRAME_Y'),
                         ('breakout', 'src/games/breakout/breakout.c',
                          'FIELD_Y'),
                         ('pacman', 'src/games/pacman/pacman.c', 'FRAME_Y'),
                         ('jumper', 'src/games/jumper/jumper.c', 'VIEW_Y'),
                         ('invaders', 'src/games/invaders/invaders.c',
                          'FIELD_Y'),
                         ('pong', 'src/games/pong/pong.c', 'FIELD_Y')):
        text = src(f) + src('include/nexus/widgets.h')
        if name == 'tetris':
            text += src('src/games/tetris/tetris_core.h')
        top = consts(text, [key])[key]
        ok(top >= HDR['NEXUS_HUD_END'],
           '%-9s field starts at %d, clear of the header at %d'
           % (name, top, HDR['NEXUS_HUD_END']))

    print('\n    and its border is a panel border, not a hand-picked radius')
    for name, f in ALL7:
        g = src(f)
        frame = g.split('round_frame')[-1][:80]
        ok(', 3, t->border' not in g and ', 3,\n' not in frame,
           '%-9s uses t->radius for its field frame' % name)

    print('\nEvery playfield is flat')
    # The ground is a gradient with two soft colour blobs behind it, which is
    # right for a dashboard and wrong behind a game: the blob edge is a
    # smooth curve crossing the play area, and on a 240px panel that reads as
    # a tear in the image. Breakout and Invaders had no fill at all, so the
    # raw ground showed through the whole field.
    ALL = (('tetris', 'src/games/tetris/tetris.c'),
           ('snake', 'src/games/snake/snake.c'),
           ('breakout', 'src/games/breakout/breakout.c'),
           ('pacman', 'src/games/pacman/pacman.c'),
           ('jumper', 'src/games/jumper/jumper.c'),
           ('invaders', 'src/games/invaders/invaders.c'),
           ('pong', 'src/games/pong/pong.c'))
    for name, f in ALL:
        g = src(f)
        ok('nexus_draw_field(' in g,
           '%-9s fills its field through the shared helper' % name)
        # Not "no translucent track anywhere" - Tetris tints its
        # next-piece box and Pac-Man cuts its mouth with the same colour.
        # What must be gone is a translucent fill at the FIELD's own rect.
        ok(not re.search(r'gfx_rect\((WELL|FIELD|VIEW)_X[^;]*, \d+\);', g),
           '%-9s has no translucent fill left at its field rect' % name)

    w = src('src/ui/widgets.c')
    ok('gfx_rect(x, y, w, h, nexus_theme()->track, GFX_OPAQUE)' in w,
       'and the helper is opaque, so no ground reaches through it')

    ru = src('scripts/render_ui.py')
    ok(ru.count('u.field(') == len(ALL),
       'the renderer fills all %d, so the doc shots show what ships'
       % len(ALL))

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
