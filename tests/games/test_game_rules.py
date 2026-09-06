#!/usr/bin/env python3
"""Snake and Breakout rules that are easy to get wrong, and the C that holds them.

Both games have one rule where the obvious implementation is subtly wrong:

  Snake     the tail cell is vacated on the same step the head enters it, so
            moving into your own tail is LEGAL. Testing for collision before
            freeing the tail kills you for chasing yourself, which every
            implementation gets wrong once.

  Breakout  where the ball lands on the paddle has to steer it. Without that
            the angle never changes and the game is a metronome you cannot
            influence.

Run: python tests/games/test_game_rules.py
"""

import os
import sys

COLS = ROWS = 24
EMPTY, BODY, FOOD = 0, 1, 2
UP, RIGHT, DOWN, LEFT = range(4)
DR = (-1, 0, 1, 0)
DC = (0, 1, 0, -1)


class Snake:
    """Mirrors step() in src/games/snake/snake.c."""

    def __init__(self, cells, direction=RIGHT):
        self.grid = [[EMPTY] * COLS for _ in range(ROWS)]
        self.ring = list(cells)            # oldest first
        for r, c in cells:
            self.grid[r][c] = BODY
        self.dir = direction
        self.dead = False
        self.score = 0

    def head(self):
        return self.ring[-1]

    def put_food(self, r, c):
        self.grid[r][c] = FOOD

    def step(self, direction=None):
        if direction is not None:
            self.dir = direction
        r, c = self.head()
        r += DR[self.dir]
        c += DC[self.dir]

        if not (0 <= r < ROWS and 0 <= c < COLS):
            self.dead = True
            return

        ate = self.grid[r][c] == FOOD

        # free the tail FIRST - this is the whole point
        if not ate:
            tr, tc = self.ring.pop(0)
            self.grid[tr][tc] = EMPTY

        if self.grid[r][c] == BODY:
            self.dead = True
            return

        self.ring.append((r, c))
        self.grid[r][c] = BODY
        if ate:
            self.score += 10


def check(name, got, want):
    if got != want:
        print("  FAIL  %s\n        got  %r\n        want %r" % (name, got, want))
        return 1
    print("  ok    %s" % name)
    return 0


def main():
    bad = 0
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

    print("Snake")
    # a 2x2 loop: the head follows immediately behind the tail
    s = Snake([(5, 5), (5, 6), (6, 6), (6, 5)], direction=UP)
    s.step(UP)          # into (5,5) - the cell the tail just left
    bad += check("moving into the vacated tail is legal", s.dead, False)

    # a genuine self-hit: turn back into the middle of the body
    s = Snake([(5, 3), (5, 4), (5, 5), (5, 6), (5, 7)], direction=RIGHT)
    s.step(UP)
    s.step(LEFT)
    s.step(DOWN)        # lands on (5,5), squarely in the body
    bad += check("hitting the middle of the body is fatal", s.dead, True)

    s = Snake([(0, 1), (0, 2)], direction=UP)
    s.step()
    bad += check("the wall is fatal", s.dead, True)

    s = Snake([(5, 5), (5, 6)], direction=RIGHT)
    s.put_food(5, 7)
    before = len(s.ring)
    s.step()
    bad += check("eating grows by one", len(s.ring), before + 1)
    bad += check("eating scores", s.score, 10)

    s = Snake([(5, 5), (5, 6)], direction=RIGHT)
    before = len(s.ring)
    s.step()
    bad += check("plain move keeps the length", len(s.ring), before)

    print("\nBreakout")
    PADDLE_W, SPEED = 38, 2 << 8

    def steer(off):
        return (off * SPEED) // (PADDLE_W // 2)

    bad += check("centre hit goes straight up", steer(0), 0)
    bad += check("left edge sends it left", steer(-19) < 0, True)
    bad += check("right edge sends it right", steer(19) > 0, True)
    bad += check("edges are symmetric", steer(-19), -steer(19))
    # the ball must not end up faster than it started, or it escapes the paddle
    bad += check("edge deflection does not exceed base speed",
                 abs(steer(19)) <= SPEED, True)

    print("\nFixed-point range - the ball-teleport bug")
    # int16_t 8.8 spans -128.0 .. +127.996, and this field runs to x=230 with
    # the paddle at y=210. Every position past 127 wrapped negative: the ball
    # vanished off one edge, reappeared at nonsense coordinates, and
    # reset_ball() parked it at -46 px where no paddle could reach it.
    FIELD_X, FIELD_W, FIELD_Y, FIELD_H = 10, 220, 40, 180
    extremes = {
        "FIELD_R": FIELD_X + FIELD_W,
        "FIELD_B": FIELD_Y + FIELD_H,
        "PADDLE_Y": FIELD_Y + FIELD_H - 10,
    }
    I16, I32 = 2 ** 15 - 1, 2 ** 31 - 1
    for name, v in extremes.items():
        bad += check("%s (%d px) would overflow 8.8 in int16" % (name, v),
                     (v << 8) > I16, True)
    bad += check("int32 holds every position with room to spare",
                 all((v << 8) < I32 for v in extremes.values()), True)

    print("\nThe C still holds these")
    snake_c = open(os.path.join(root, "src", "games", "snake", "snake.c"),
                   encoding="utf-8").read()
    step = snake_c.split("static void step(void)")[1].split("\n}\n")[0]
    free_at = step.find("= EMPTY")
    hit_at = step.find("== BODY")
    if free_at == -1 or hit_at == -1:
        print("  FAIL  snake step() lost its tail-free or body test")
        bad += 1
    elif free_at > hit_at:
        print("  FAIL  snake tests for BODY before freeing the tail")
        bad += 1
    else:
        print("  ok    snake frees the tail before the collision test")

    for frag, why in [("opposite[g_s.dir]", "a 180 turn is refused"),
                      ("next_dir", "input writes next_dir, not dir"),
                      ("CONFIG_NEXUS_SNAKE_WRAP", "edges wrap, no fatal walls"),
                      ("CONFIG_NEXUS_SNAKE_TICK_MS", "speed comes from Kconfig"),
                      ("NEXUS_ACTION_ROTATE", "I (ROTATE) steers up")]:
        ok = frag in snake_c
        print(("  ok    " if ok else "  FAIL  ") + "snake: " + why)
        if not ok:
            bad += 1

    bo = open(os.path.join(root, "src", "games", "breakout", "breakout.c"),
              encoding="utf-8").read()
    for frag, why in [
        ("off * BALL_SPEED / (PADDLE_W / 2)", "paddle position steers the ball"),
        ("typedef int32_t fix_t;",
         "fixed point is 32-bit - int16 caps at 128 px and the field is 230"),
        ("TO_FIX", "positions are fixed point, not whole pixels"),
        ("CONFIG_NEXUS_BREAKOUT_BALL_SPEED", "ball speed comes from Kconfig"),
        ("g_b.bricks[row] &= ", "a hit clears the brick bit"),
    ]:
        ok = frag in bo
        print(("  ok    " if ok else "  FAIL  ") + "breakout: " + why)
        if not ok:
            bad += 1

    # both games must be in the registry, and guarded
    gm = open(os.path.join(root, "src", "games", "game_manager.c"),
              encoding="utf-8").read()
    for name in ("snake", "breakout", "tetris"):
        sym = "&nexus_game_%s," % name
        guard = "CONFIG_NEXUS_%s" % name.upper()
        ok = sym in gm and guard in gm
        print(("  ok    " if ok else "  FAIL  ")
              + "registry: %s registered and guarded" % name)
        if not ok:
            bad += 1

    print("\n%s" % ("FAILED (%d)" % bad if bad else "PASSED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
