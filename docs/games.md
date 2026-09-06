# Games

## Playing

Action button from Home opens the Game Center. Left/right pick a game, the
button launches it.

### Tetris controls

| Action | Physical button | Keymap binding |
| --- | --- | --- |
| Move left | -- | `&nexus_action NEXUS_ACT_LEFT` |
| Move right | -- | `&nexus_action NEXUS_ACT_RIGHT` |
| Rotate | -- | `&nexus_action NEXUS_ACT_ROTATE` |
| Soft drop | -- | `&nexus_action NEXUS_ACT_DOWN` |
| Hard drop | -- | `&nexus_action NEXUS_ACT_DROP` |
| Pause / resume / restart | short press | `&nexus_action NEXUS_ACT_SELECT` |
| Quit | long press | `&nexus_action NEXUS_ACT_BACK` |

One button cannot play Tetris, so movement comes from the keyboard. Put the
bindings on a dedicated layer:

```dts
#include <dt-bindings/nexus.h>

games_layer {
    display-name = "GAMES";
    bindings = <
        &nexus_action NEXUS_ACT_LEFT   &nexus_action NEXUS_ACT_ROTATE
        &nexus_action NEXUS_ACT_RIGHT  &nexus_action NEXUS_ACT_DOWN
        &nexus_action NEXUS_ACT_DROP   &nexus_action NEXUS_ACT_BACK
    >;
};
```

The game never sees a keycode. It receives `NEXUS_ACTION_LEFT`, exactly the same
value the physical button would have produced (Section 42).

### Rules

Standard: 10x20 well, seven tetrominoes from a shuffled 7-bag, simple symmetric
wall kicks. Scoring is 100/300/500/800 per 1/2/3/4 lines, multiplied by level,
plus 1 point per soft-dropped row and 2 per hard-dropped row. Level is
`1 + lines/10`; gravity starts at 800 ms and drops 100 ms per level to a floor
of 100 ms.

High scores persist in Zephyr settings under `nexus/games/tetris`, written only
when a record actually improves -- never per frame (Section 107).

## Adding a game

Two things: a file implementing `struct nexus_game`, and one line in the
registry. The launcher, the pause handling, the high-score plumbing and the
input routing are already there and know nothing about your game.

### 1. Implement the interface

```c
#include <nexus/game.h>

static void snake_start(void);               /* reset state, arm the clock */
static void snake_update(void);              /* advance one tick (optional) */
static bool snake_input(enum nexus_action a);/* true if consumed */
static void snake_pause(void);
static void snake_resume(void);
static void snake_stop(void);                /* release timers */
static void snake_draw(void);                /* paint; called once per band */
static uint32_t snake_score(void);
static enum nexus_game_state snake_state(void);

const struct nexus_game nexus_game_snake = {
    .id = "snake",          /* settings key fragment */
    .name = "SNAKE",        /* shown in the launcher */
    .start = snake_start,
    /* ... */
};
```

### 2. Register it

`src/games/game_manager.c`:

```c
#if IS_ENABLED(CONFIG_NEXUS_SNAKE)
extern const struct nexus_game nexus_game_snake;
#endif

static const struct nexus_game *const games[] = {
#if IS_ENABLED(CONFIG_NEXUS_TETRIS)
    &nexus_game_tetris,
#endif
#if IS_ENABLED(CONFIG_NEXUS_SNAKE)
    &nexus_game_snake,
#endif
    NULL,   /* keep the terminator */
};
```

Plus a `CONFIG_NEXUS_SNAKE` in `Kconfig` and the sources in `CMakeLists.txt`.

### Rules that matter

- **No game loop.** Never `while (1) { k_sleep(); }`. Drive the simulation from
  a `k_work_delayable` on `nexus_workq()`, the way `tetris.c` does. A blocking
  loop starves BLE, USB and Studio RPC (Sections 44, 141-F).
- **No hardware.** Do not touch a GPIO, a PWM or the display driver. Draw with
  `gfx_*` and `nexus_draw_*`, make noise through `nexus_sound_play()`
  (Requirement D).
- **`draw()` must be pure.** It is called once per compositor band -- up to
  twenty times per full repaint -- with the scene clipped to that band. Format
  strings and advance state somewhere else; use `gfx_hits()` to skip anything
  the live band does not touch.
- **Keep the rules separable.** Tetris splits into `tetris_core.c` (pure C, no
  Zephyr, no NEXUS) and `tetris.c` (rendering and timing). That split is why
  the rules have real unit tests that run in seconds without a board. Do the
  same and your game gets tested; don't and it won't.
- **Watch RAM.** Tetris keeps no framebuffer at all: the board is
  `uint8_t[22][10]` and the compositor paints straight from it, so the whole
  game is about 220 bytes. Reach for a canvas only when you can say why the
  band compositor cannot do the job.
- **No copyrighted assets.** Original graphics and original names only. A maze
  chase game is fine; a Pac-Man clone with Pac-Man's artwork is not (Section 52).

### Distinguishing pieces without colour

Tetris gives each cell a lit top-left edge, a dark bottom-right edge, and a
two-pixel mark at the piece's own spot in a 3x3 grid. Shape, shading and
pattern all carry the identity, so the board reads correctly in monochrome or
to a colourblind player -- colour is never the only signal (Section 106).
Worth copying.

## The three games

| | RAM | how it moves |
| --- | --- | --- |
| Tetris | ~245 B | integer grid, gravity on its own clock |
| Snake | ~1.7 KB | integer grid, ring of cells |
| Breakout | ~24 B | 8.8 fixed point |

All three go through `struct nexus_game`, so the Game Center pages between
them with no per-game UI code, and each is one `#if` in `games[]`. Turning any
of them off with `CONFIG_NEXUS_TETRIS` / `_SNAKE` / `_BREAKOUT` removes it from
the build entirely.

### Snake

The body is a **ring of cell indices** plus an occupancy grid, not a list of
segments. Growing is then "do not advance the tail this step" rather than
shifting the whole snake every frame, and the self-collision test is one array
read instead of a walk.

The rule worth knowing: **the tail cell is vacated on the same step the head
enters it**, so moving into your own tail is legal - the classic tail-chase.
Testing for collision before freeing the tail kills you for it, which is the
bug every implementation writes once. `tests/games/test_game_rules.py` asserts
both the behaviour and that the C still frees before it tests.

Input writes `next_dir`, never `dir`: two taps inside one tick would otherwise
let you turn 180 into your own neck, which reads as a bug rather than as a
mistake.

Food placement picks the **Nth free cell** rather than retrying random
positions. Rejection sampling can spin a long time on a nearly-full board, and
this runs on the display work queue.

### Breakout

The only game here that is not on a grid, so positions are **8.8 fixed point
in an int32_t**, and the width is not incidental.

It was `int16_t` first. That holds 8.8 values from -128.0 to +127.996 - and
the field runs to x=230 with the paddle at y=210. Every position past 127
overflowed and wrapped negative, which produced three symptoms that looked
unrelated: the ball vanished off one edge, reappeared at a nonsense
coordinate, and never met the paddle because `reset_ball()` was parking it at
-46 px. One type, three bugs. The test now asserts int16 could not have held
the field, so the narrower type cannot come back.
Integer-per-frame motion would force the ball to travel at least a pixel per
tick - far too fast at any playable rate - and floating point does not belong
in a display work queue handler on a Cortex-M4.

Where the ball lands on the paddle steers it. Without that one line the angle
never changes and the game is a metronome you cannot influence; the test
asserts the deflection is symmetric and never exceeds the base speed, because a
faster-than-base edge hit escapes the paddle entirely.

Bricks are a **bitmask per row**: eight columns in one `uint8_t`, so "cleared
the board" is an OR of five bytes.

The action button launches a parked ball before it pauses. Otherwise the only
way to start a life is a direction key, and on the physical button alone - all
some users have - the game would be unstartable.

## Difficulty, without reflashing

**Settings -> SPEED** cycles SLOW / EASY / NORMAL / FAST / INSANE and persists
with sound, theme and brightness. It applies to all three games and takes
effect on the next round.

That is the knob to reach for. "The ball is too slow" is a judgement you make
while playing, and one you have to reflash to act on is one you turn once and
then live with.

3 (NORMAL) is neutral and reproduces the Kconfig values exactly; each step
either side is 20%. Note that the two kinds of game scale **opposite ways for
the same word**: Snake and Tetris scale an *interval*, so faster means a
smaller number, while Breakout scales a *velocity*. The first attempt used
`speed / 3` for the ball, which put SLOW at a third of normal - about one
pixel a tick, which is not a difficulty setting but a broken game.

## The Kconfig baselines

These set what NORMAL means. A config repo changes the centre of the range;
the Settings knob moves around it.

| option | default | |
| --- | --- | --- |
| `NEXUS_SNAKE_WRAP` | `y` | wrap at the edges instead of dying |
| `NEXUS_SNAKE_TICK_MS` | 160 | starting step interval - **lower is faster** |
| `NEXUS_SNAKE_TICK_MIN_MS` | 70 | floor the speed-up cannot pass |
| `NEXUS_SNAKE_SPEED_EVERY` | 4 | apples per speed-up |
| `NEXUS_SNAKE_SPEED_STEP_MS` | 10 | ms removed each time |
| `NEXUS_BREAKOUT_BALL_SPEED` | 350 | hundredths of a pixel per tick |
| `NEXUS_BREAKOUT_TICK_MS` | 28 | simulation interval |

Two things worth knowing before turning them:

**Snake has a speed floor for a reason.** Below about 50 ms the snake moves
faster than the panel repaints, so what you see lags what the game thinks, and
input stops feeling connected to it.

**Breakout's speed knob is in hundredths of a pixel**, not whole pixels,
because whole pixels would only offer 1, 2 and 3 - sedate, brisk, unplayable -
with nothing usable in between. Keep it under about half the paddle height per
tick, or the ball can cross the paddle inside a single step and be missed
entirely. And note that `TICK_MS` scales speed as well, since the ball moves
`BALL_SPEED` per tick: change one or the other, not both at once.

## The I key

`I` is bound to `NEXUS_ACT_ROTATE` on the game layer because Tetris needs
rotation there. Snake and Breakout have nothing to rotate, so both accept
`ROTATE` as their natural top-of-cluster action - up for Snake, launch for
Breakout. Otherwise `I` would simply be inert in two of the three games, which
reads as a broken key rather than as an unused one.

## Switching games

| | |
| --- | --- |
| `J` / `L` in the Game Center | previous / next |
| **double-tap the action button** | next |
| tap | play the selected game |
| hold | home |

The double-tap exists because with one game the button needed no third
gesture, and with three a dongle that can only ever launch whichever game
happens to be selected is not finished. Tap still plays and hold still goes
home; the carousel did not have to displace either.

It costs something, and only where it is used: a screen that declares
`btn_double` must hold its single tap back until the window closes
(`CONFIG_NEXUS_BUTTON_DOUBLE_MS`, 280 ms) to find out whether a second tap is
coming. Every other screen still dispatches a tap the instant the button comes
up. That is why `btn_double` is opt-in per screen rather than a global
gesture.
