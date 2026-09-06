# Games

## Playing

Action button from Home opens the Game Center. Left/right pick a game, the
button launches it.

One button cannot play any of these, so movement comes from the keyboard. The
same six bindings drive all three games -- the game never sees a keycode, only
a `NEXUS_ACTION_*`, exactly the value the physical button would have produced
(Section 42).

| key | action | Tetris | Snake | Breakout |
| --- | --- | --- | --- | --- |
| `J` | `NEXUS_ACT_LEFT` | move left | turn left | paddle left |
| `L` | `NEXUS_ACT_RIGHT` | move right | turn right | paddle right |
| `I` | `NEXUS_ACT_ROTATE` | rotate | turn up | launch |
| `K` | `NEXUS_ACT_DOWN` | soft drop | turn down | -- |
| | `NEXUS_ACT_DROP` | hard drop | -- | launch |
| button | `NEXUS_ACT_SELECT` | pause / resume / restart | | launch, then pause |
| hold | `NEXUS_ACT_BACK` | quit | | |

**Hold a direction and it repeats.** Holding `K` is a soft drop rather than
thirty taps; holding `J` or `L` slides the Breakout paddle instead of nudging
it. ZMK fires a behavior once per press and nothing repeats it, so NEXUS arms
its own repeat on the movement actions only -- `CONFIG_NEXUS_ACTION_REPEAT_MS`
between repeats after `CONFIG_NEXUS_ACTION_REPEAT_DELAY_MS`. `DROP` is not in
that set: a hard drop and a ball launch are one-shot by definition.

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

Snake and Breakout were both added this way and neither needed a line of
launcher code, so the example below uses a fourth game that does not exist --
`pong` -- rather than one you would then find already written.

```c
#include <nexus/game.h>

static void pong_start(void);               /* reset state, arm the clock */
static bool pong_input(enum nexus_action a);/* true if consumed */
static void pong_pause(void);
static void pong_resume(void);
static void pong_stop(void);                /* release timers */
static void pong_draw(void);                /* paint; called once per band */
static uint32_t pong_score(void);
static enum nexus_game_state pong_state(void);

const struct nexus_game nexus_game_pong = {
    .id = "pong",           /* settings key fragment */
    .name = "PONG",         /* shown in the launcher */
    .start = pong_start,
    /* ... */
};
```

### 2. Register it

`src/games/game_manager.c`:

```c
#if IS_ENABLED(CONFIG_NEXUS_PONG)
extern const struct nexus_game nexus_game_pong;
#endif

static const struct nexus_game *const games[] = {
#if IS_ENABLED(CONFIG_NEXUS_TETRIS)
    &nexus_game_tetris,
#endif
    /* ... snake, breakout ... */
#if IS_ENABLED(CONFIG_NEXUS_PONG)
    &nexus_game_pong,
#endif
    NULL,   /* keep the terminator */
};
```

Plus a `CONFIG_NEXUS_PONG` in `Kconfig` and the sources in `CMakeLists.txt`.
The terminator is not decoration: a zero-length array is not valid C, and the
Game Center can legitimately be built with every game turned off.

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
- **No copyrighted assets.** Original graphics only - every sprite in this
  repo is drawn with compositor primitives, none is traced or imported.

  Names are the owner's call and the maze chase ships as `PAC-MAN`, which is a
  Bandai Namco trademark. Mechanics are not copyrightable and the artwork here
  is original, so the name is the only exposure; if this repo ever needs to be
  uncontroversial, that string in `pacman.c` and its Kconfig prompt are the
  whole change.

### Distinguishing pieces without colour

Tetris gives each cell a lit top-left edge, a dark bottom-right edge, and a
two-pixel mark at the piece's own spot in a 3x3 grid. Shape, shading and
pattern all carry the identity, so the board reads correctly in monochrome or
to a colourblind player -- colour is never the only signal (Section 106).
Worth copying.

## The four games

| | board | RAM | how it moves |
| --- | --- | --- | --- |
| Tetris | 10x20 | ~245 B | integer grid, gravity on its own clock |
| Snake | 16x16 | ~768 B | integer grid, ring of cells |
| Breakout | free | ~24 B | 8.8 fixed point |
| Pac-Man | 19x15 | ~300 B | integer grid, greedy chasers |

Snake's board is 16x16 of 12px cells, not 24x24 of 8px. At 8px the snake was
four faint slivers and the apple a speck -- on a panel you look at from across
a desk that is not detail, it is just small. The grid and the ring are both
O(cells), so the larger cells also cost a third of the RAM: 768 bytes rather
than 1,728.

All four go through `struct nexus_game`, so the Game Center pages between
them with no per-game UI code, and each is one `#if` in `games[]`. Turning any
of them off with `CONFIG_NEXUS_TETRIS` / `_SNAKE` / `_BREAKOUT` / `_PACMAN`
removes it from the build entirely.

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

**Walls are a setting, not a build flag.** *Settings -> SNAKE WALL* toggles
between a torus and fatal edges and persists; `CONFIG_NEXUS_SNAKE_WRAP` is only
the power-on default. Wrap or no wrap is the single biggest change to how the
game plays, and that is not a decision worth a reflash. The board says which
mode it is in, because otherwise you find out by dying.

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

The paddle moves `CONFIG_NEXUS_BREAKOUT_PADDLE_STEP` pixels per key repeat, so
its speed is that times the repeat rate. At 12px it crossed the field in about
1.3 seconds, which in a paddle game means the ball reaches the corner first
every time; 24 halves that. It is a Kconfig rather than a constant because the
right number depends on the repeat interval it is paired with.

The paddle's catch window scales with the ball's own travel rather than being a
fixed `PADDLE_H + 2`. That margin was enough at the old top speed and is not at
LUDICROUS: a ball moving 5.6px a tick steps straight over a 7px window, and the
life goes to a collision test that never ran.

### Pac-Man

A 19x15 maze at 12px cells, 144 dots, four power pellets, three chasers and a
wrapping tunnel row.

**The maze is stored as text** and decoded once at round start:

```c
"####.###.#.###.####",
".........G.........",      /* the tunnel - the only row that wraps */
"####.###.#.###.####",
```

That costs 285 bytes of flash for the layout and buys a level you can read and
edit without counting bits. It also makes the level testable:
`tests/games/test_pacman_maze.py` flood-fills it from the spawn and fails if a
single dot is unreachable -- a walled-in dot makes the level uncompletable, and
the only symptom on hardware is eating all the others and nothing happening.

**The chasers are greedy with one rule: never reverse.** That single constraint
is what turns "walks at you" into something that commits to a route and can be
led away from a corridor; without it a chaser oscillates on the spot every time
you cross its axis. Frightened mode is the *same* search with the sign flipped,
not a second pathfinder.

**Collisions are tested twice**, before and after the chasers move. Testing once
lets a chaser and the player swap cells in a single tick and pass straight
through each other, which reads as broken hit detection rather than a near miss.

A turn is a *request*: it is stored and taken at the next junction that allows
it, so you can press early into a corner rather than having to time it. That is
also why turning makes no sound - the press often does not become a move.

## Difficulty, without reflashing

**Settings -> SPEED** cycles SLOW / EASY / NORMAL / FAST / INSANE / LUDICROUS
and persists with sound, theme and brightness. It applies to all three games
and takes effect on the next round.

That is the knob to reach for. "The ball is too slow" is a judgement you make
while playing, and one you have to reflash to act on is one you turn once and
then live with.

3 (NORMAL) is neutral and reproduces the Kconfig values exactly; each step
either side is 20%. A `BUILD_ASSERT` ties the name table to
`NEXUS_GAME_SPEED_MAX`, so adding a seventh notch without naming it fails the
build rather than printing whatever follows the array. Note that the two kinds of game scale **opposite ways for
the same word**: Snake and Tetris scale an *interval*, so faster means a
smaller number, while Breakout scales a *velocity*. The first attempt used
`speed / 3` for the ball, which put SLOW at a third of normal - about one
pixel a tick, which is not a difficulty setting but a broken game.

## The Kconfig baselines

These set what NORMAL means. A config repo changes the centre of the range;
the Settings knob moves around it.

| option | default | |
| --- | --- | --- |
| `NEXUS_SNAKE_WRAP` | `y` | wrap at the edges instead of dying -- **power-on default only**, Settings owns it after that |
| `NEXUS_SNAKE_TICK_MS` | 160 | starting step interval - **lower is faster** |
| `NEXUS_SNAKE_TICK_MIN_MS` | 70 | floor the speed-up cannot pass |
| `NEXUS_SNAKE_SPEED_EVERY` | 4 | apples per speed-up |
| `NEXUS_SNAKE_SPEED_STEP_MS` | 10 | ms removed each time |
| `NEXUS_BREAKOUT_BALL_SPEED` | 350 | hundredths of a pixel per tick |
| `NEXUS_BREAKOUT_TICK_MS` | 28 | simulation interval |
| `NEXUS_BREAKOUT_PADDLE_STEP` | 24 | pixels the paddle travels per key repeat |
| `NEXUS_PACMAN_TICK_MS` | 150 | maze step interval - **lower is faster** |
| `NEXUS_PACMAN_FRIGHT_TICKS` | 40 | ticks a power pellet lasts, ~6 s |

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
