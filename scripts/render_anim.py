#!/usr/bin/env python3
"""Animated GIFs of the UI, driven by the real game rules.

The frames come from render_ui.py, so they are the same reconstruction the
static screenshots are - same fonts, same glyph bitmaps, same palettes, same
draw order. What is added here is TIME, and the motion is simulated with the
rules the C actually implements rather than a hand-drawn approximation:

  snake     ring of cells, tail freed before the collision test, wrap at the
            edges, grow on food - src/games/snake/snake.c step()
  breakout  8.8 fixed point, brick bitmask, and the paddle deflection that
            steers the ball - src/games/breakout/breakout.c step()
  splash    the lit-letter sweep nexus_splash_phase() drives
  home      WPM and modifiers changing, which is what the dashboard does all
            day

Frame delays are the games' real tick intervals, so the GIFs run at the speed
the hardware does.

Run: python scripts/render_anim.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import render_ui as R          # noqa: E402
from gif import write_gif      # noqa: E402

OUT = os.path.join(R.ROOT, 'docs', 'images', 'anim')
SCALE = 2                      # 480x480: crisp, and a GIF is not a poster


def frame(fn):
    cv = R.Canvas()
    fn(cv)
    return cv.b


# ------------------------------------------------------------------ snake
def anim_snake(t):
    """src/games/snake/snake.c: ring of cells, wrap, grow on food."""
    COLS = ROWS = R.SNK['COLS']
    body = [(8, 3), (8, 4), (8, 5), (8, 6), (8, 7)]      # tail first
    food = (8, 12)
    score = 0
    rng = 0x27D4EB2F
    frames, delays = [], []

    def nxt_food(occupied):
        nonlocal rng
        for _ in range(400):
            rng = (rng * 1103515245 + 12345) & 0xFFFFFFFF
            cell = ((rng >> 16) % ROWS, (rng >> 8) % COLS)
            if cell not in occupied:
                return cell
        return None

    for _ in range(64):
        frames.append(frame(lambda cv: R.snake(cv, t, body=list(body),
                                               food=food, score=str(score))))
        delays.append(9)                                  # ~140 ms tick

        hr, hc = body[-1]
        # Steer toward the food, turning on one axis at a time - the same
        # single-direction-per-tick rule the game enforces.
        dr = (food[0] - hr) % ROWS
        dc = (food[1] - hc) % COLS
        if dc and dc <= COLS // 2:
            step = (hr, (hc + 1) % COLS)
        elif dc:
            step = (hr, (hc - 1) % COLS)
        elif dr and dr <= ROWS // 2:
            step = ((hr + 1) % ROWS, hc)
        else:
            step = ((hr - 1) % ROWS, hc)

        ate = step == food
        if not ate:
            body.pop(0)          # tail freed BEFORE the collision test
        body.append(step)
        if ate:
            score += 10
            food = nxt_food(set(body))
    return frames, delays


# --------------------------------------------------------------- breakout
def anim_breakout(t):
    """src/games/breakout/breakout.c: 8.8 fixed point and a steering paddle."""
    K = R.BRK
    FIX = 256
    bx, by = 120 * FIX, 120 * FIX
    vx, vy = int(1.9 * FIX), int(-3.5 * FIX)
    paddle = 96
    gone = set()
    score = 0
    frames, delays = [], []

    for _ in range(90):
        frames.append(frame(lambda cv: R.breakout(
            cv, t, ball=(bx // FIX, by // FIX), paddle=paddle,
            gone=set(gone), score=str(score))))
        delays.append(4)                                  # ~28 ms tick

        bx += vx
        by += vy
        px, py = bx // FIX, by // FIX

        if px - K['BALL_R'] <= 0:
            bx, vx = K['BALL_R'] * FIX, -vx
        elif px + K['BALL_R'] >= K['FIELD_W']:
            bx, vx = (K['FIELD_W'] - K['BALL_R']) * FIX, -vx
        if py - K['BALL_R'] <= 0:
            by, vy = K['BALL_R'] * FIX, -vy

        px, py = bx // FIX, by // FIX
        # Bricks, in field-relative coordinates.
        top = K['BRICK_TOP'] - K['FIELD_Y']
        if top <= py < top + K['BRICK_ROWS'] * K['BRICK_H']:
            row = (py - top) // K['BRICK_H']
            col = px // K['BRICK_W']
            if 0 <= col < K['BRICK_COLS'] and (row, col) not in gone:
                gone.add((row, col))
                score += (K['BRICK_ROWS'] - row) * 10
                vy = -vy

        # The paddle chases the ball, which is what a player does.
        want = px - K['PADDLE_W'] // 2
        paddle += max(-8, min(8, want - paddle))
        paddle = max(0, min(K['FIELD_W'] - K['PADDLE_W'], paddle))

        paddle_y = K['FIELD_H'] - 10
        if vy > 0 and paddle_y <= py + K['BALL_R'] <= paddle_y + 12 \
                and paddle <= px <= paddle + K['PADDLE_W']:
            by = (paddle_y - K['BALL_R']) * FIX
            vy = -vy
            off = px - (paddle + K['PADDLE_W'] // 2)
            vx = off * int(3.5 * FIX) // (K['PADDLE_W'] // 2)

        if py > K['FIELD_H'] + 10:                        # missed: re-serve
            bx, by = (paddle + K['PADDLE_W'] // 2) * FIX, 120 * FIX
            vx, vy = int(1.9 * FIX), int(-3.5 * FIX)
    return frames, delays


# ----------------------------------------------------------------- splash
def anim_splash(t):
    """The lit-letter sweep, then a beat on the finished badge."""
    frames, delays = [], []
    for i in range(5):
        frames.append(frame(lambda cv: R.splash(cv, t, lit=i)))
        delays.append(18)
    frames.append(frame(lambda cv: R.splash(cv, t, lit=-1)))
    delays.append(120)
    return frames, delays


# ------------------------------------------------------------------- home
def anim_home(t):
    """What the dashboard does while you type."""
    MODS = [0, 0b0010, 0b0011, 0b0001, 0, 0b1000, 0b1010, 0]
    LAYERS = ['DEFAULT', 'DEFAULT', 'LOWER', 'LOWER', 'RAISE', 'DEFAULT']
    frames, delays = [], []
    for i in range(16):
        st = dict(R.STATUS)
        st['wpm'] = int(38 + 46 * (1 - abs(1 - i / 8.0)))
        st['mods'] = MODS[i % len(MODS)]
        st['layer'] = LAYERS[(i // 3) % len(LAYERS)]
        st['batt'] = [78, 64]
        frames.append(frame(lambda cv, s=st: R.home(cv, t, s)))
        delays.append(20)
    return frames, delays


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    th = {x['name']: x for x in R.themes()}
    nx = th['NEXUS']

    for name, fn in (('snake', anim_snake), ('breakout', anim_breakout),
                     ('splash', anim_splash), ('home', anim_home)):
        frames, delays = fn(nx)
        path = os.path.join(OUT, name + '.gif')
        size, ncol = write_gif(path, R.W, R.H, frames, delays, SCALE)
        print('  %-9s %3d frames  %3d colours  %6.1f KB'
              % (name + '.gif', len(frames), ncol, size / 1024.0))

    print('-> docs/images/anim/')
    return 0


if __name__ == '__main__':
    sys.exit(main())
