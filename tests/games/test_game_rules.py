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

COLS = ROWS = 16   # must match src/games/snake/snake.c
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

    print("\nLive difficulty setting (1..6, 3 neutral)")
    # snake and tetris scale an INTERVAL; breakout scales a VELOCITY. The same
    # word "faster" therefore moves the two expressions in opposite
    # directions, which is the easy mistake and worth pinning down.

    def interval(sp):
        return (8 - sp) / 5

    def velocity(sp):
        return (2 + sp) / 5

    bad += check("speed 3 leaves the interval untouched", interval(3), 1.0)
    bad += check("speed 3 leaves the velocity untouched", velocity(3), 1.0)
    bad += check("higher speed shortens the interval", interval(5) < interval(1), True)
    bad += check("higher speed raises the velocity", velocity(5) > velocity(1), True)
    bad += check("the two curves move in opposite directions",
                 interval(5) < 1.0 and velocity(5) > 1.0, True)

    # SLOW must still be a game, and the top speed must not tunnel.
    SPEED_MIN, SPEED_MAX = 1, 6
    PADDLE_H, base = 5, 3.5

    hdr = open(os.path.join(root, "include", "nexus", "game.h"),
               encoding="utf-8").read()
    bad += check("game.h agrees with this test about the top speed",
                 "#define NEXUS_GAME_SPEED_MAX %d" % SPEED_MAX in hdr, True)

    gm_names = open(os.path.join(root, "src", "games", "game_manager.c"),
                    encoding="utf-8").read()
    bad += check("the new top speed has a name",
                 '"LUDICROUS"' in gm_names, True)
    bad += check("and a BUILD_ASSERT so the next bump cannot skip one",
                 "BUILD_ASSERT" in gm_names, True)

    bad += check("slowest ball is still moving",
                 base * velocity(SPEED_MIN) >= 2.0, True)

    # An interval of zero would reschedule the tick with no delay and spin the
    # display work queue forever - a brick, not a glitch. (8 - speed) hits zero
    # at speed 8, so this is the assertion that stops the next bump going there.
    bad += check("the fastest interval is still an interval",
                 interval(SPEED_MAX) > 0, True)

    # The catch window has to be at least one tick of travel deep, or a fast
    # ball steps over the paddle between frames and the life goes to a
    # collision test that never ran. This is why the +2 became scaled.
    travel = base * velocity(SPEED_MAX)
    reach = PADDLE_H + 2 + int(travel)
    bad += check("the fastest ball cannot step over the paddle",
                 travel <= reach, True)
    bo_early = open(os.path.join(root, "src", "games", "breakout",
                                 "breakout.c"), encoding="utf-8").read()
    bad += check("and the window scales rather than being a fixed +2",
                 "PADDLE_H + 2 + TO_PX(BALL_SPEED)" in bo_early, True)

    print("\nHeld movement keys auto-repeat")
    ac = open(os.path.join(root, "src", "action.c"), encoding="utf-8").read()
    for frag, why in [
        ("static bool action_repeats", "there is a repeat allow-list"),
        ("case NEXUS_ACTION_LEFT:", "LEFT repeats"),
        ("case NEXUS_ACTION_DOWN:", "DOWN repeats - this is the soft drop"),
        ("g_held == action", "only the key that armed a repeat can stop it"),
        ("CONFIG_NEXUS_ACTION_REPEAT_DELAY_MS",
         "the first repeat waits longer than the rest"),
    ]:
        ok = frag in ac
        print(("  ok    " if ok else "  FAIL  ") + why)
        if not ok:
            bad += 1

    # repeating a navigation action would open a screen per tick
    reps = ac.split("static bool action_repeats")[1].split("\n}")[0]
    for never in ("NEXUS_ACTION_MENU", "NEXUS_ACTION_SELECT",
                  "NEXUS_ACTION_HOME", "NEXUS_ACTION_DROP"):
        ok = never not in reps
        print(("  ok    " if ok else "  FAIL  ")
              + "%s does not repeat" % never.replace("NEXUS_ACTION_", ""))
        if not ok:
            bad += 1

    beh = open(os.path.join(root, "src", "behaviors",
                            "behavior_nexus_action.c"), encoding="utf-8").read()
    ok = "nexus_action_release" in beh
    print(("  ok    " if ok else "  FAIL  ")
          + "the keymap behaviour reports key release")
    if not ok:
        bad += 1

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
                      ("nexus_snake_wrap()",
                       "walls are a runtime setting, not a build flag"),
                      ("CONFIG_NEXUS_SNAKE_TICK_MS", "speed comes from Kconfig"),
                      ("NEXUS_ACTION_ROTATE", "I (ROTATE) steers up"),
                      ("nexus_game_speed()", "tick scales with the live setting"),
                      ("#define CELL 12",
                       "cells are 12px - 8px was four faint slivers"),
                      ("#define COLS 16", "board matches this test's model")]:
        ok = frag in snake_c
        print(("  ok    " if ok else "  FAIL  ") + "snake: " + why)
        if not ok:
            bad += 1

    print("\nThe live score repaints")
    # Both games draw the score at y=24 and then invalidated only the
    # playfield below it, so the number was painted once at start and never
    # updated - it read as a score stuck on 0. Tetris was fine because its
    # score sits in the side panel, inside the band the well already dirties.
    for name, src in (("snake", snake_c),):
        ok = "nexus_screen_invalidate_rows(NEXUS_HUD_ROW_Y" in src
        print(("  ok    " if ok else "  FAIL  ")
              + "%s: the score band is invalidated when it changes" % name)
        if not ok:
            bad += 1

    # ... and only when it changes: the HUD must not be dirtied every tick.
    step = snake_c.split("static void step(void)")[1].split("\n}\n")[0]
    ate = step.split("if (ate) {")[-1]
    ok = "nexus_screen_invalidate_rows(NEXUS_HUD_ROW_Y" in ate
    print(("  ok    " if ok else "  FAIL  ")
          + "snake: the HUD repaint is inside the scoring branch")
    if not ok:
        bad += 1

    bo = open(os.path.join(root, "src", "games", "breakout", "breakout.c"),
              encoding="utf-8").read()

    ok = bo.count("nexus_screen_invalidate_rows(NEXUS_HUD_ROW_Y") >= 2
    print(("  ok    " if ok else "  FAIL  ")
          + "breakout: both the brick score and the lost life redraw the HUD")
    if not ok:
        bad += 1

    # 12px a step at a 70ms repeat crossed the field in 1.3s, which loses
    # every rally. The paddle has to be able to beat the ball across.
    print("\nSettings rows")
    menus = open(os.path.join(root, "src", "ui", "menus.c"),
                 encoding="utf-8").read()
    # "WALLS" in a shared Settings list reads as a global; it is Snake's.
    bad += check("the walls row names its game", '"SNAKE WALL"' in menus, True)
    # 222px content, 8px inset each side, 6px gap, 10x14 glyphs at 6px advance:
    # the label must leave room for "OFF" without dropping a text size.
    label_w = len("SNAKE WALL") * 12 - 2
    bad += check("the label still leaves room for its value",
                 222 - 2 * 8 - 6 - label_w >= len("OFF") * 12 - 2, True)

    print("\nSplash precedence")
    # The rest of the splash lives in tests/splash/test_badge_layout.py - the
    # module draws its default rather than shipping a PNG for it. What still
    # belongs nowhere else is the priority: a config-repo image must win, or
    # a custom splash silently stops working the day the module gains one.
    cm = open(os.path.join(root, "CMakeLists.txt"), encoding="utf-8").read()
    user_at = cm.find("CONFIG_NEXUS_SPLASH_IMAGE}")
    dflt_at = cm.find("CONFIG_NEXUS_SPLASH_DEFAULT_IMAGE}")
    bad += check("a config-repo splash overrides the module's own",
                 -1 < user_at < dflt_at, True)

    print("\nPaddle speed")
    REPEAT_MS, TRAVEL = 70, 220 - 38
    step_px = 24
    cross_s = TRAVEL / step_px * REPEAT_MS / 1000.0
    bad += check("the paddle crosses the field in under 0.7s",
                 cross_s < 0.7, True)
    bad += check("but not so fast it teleports (>=6 steps across)",
                 TRAVEL // step_px >= 6, True)
    ok = "CONFIG_NEXUS_BREAKOUT_PADDLE_STEP" in bo
    print(("  ok    " if ok else "  FAIL  ")
          + "breakout: the step is a Kconfig, not a hardcoded 12")
    if not ok:
        bad += 1
    for frag, why in [
        ("off * BALL_SPEED / (PADDLE_W / 2)", "paddle position steers the ball"),
        ("typedef int32_t fix_t;",
         "fixed point is 32-bit - int16 caps at 128 px and the field is 230"),
        ("TO_FIX", "positions are fixed point, not whole pixels"),
        ("CONFIG_NEXUS_BREAKOUT_BALL_SPEED", "ball speed comes from Kconfig"),
        ("g_b.bricks[row] &= ", "a hit clears the brick bit"),
        ("(2 + nexus_game_speed()) / 5",
         "velocity scales live, and not by speed/3 which made SLOW unplayable"),
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
