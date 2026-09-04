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
