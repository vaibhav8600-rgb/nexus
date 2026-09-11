# Games

## Playing

Action button from Home opens the Game Center. Left/right pick a game, the
button launches it.

One button cannot play any of these, so movement comes from the keyboard. The
same six bindings drive every game -- the game never sees a keycode, only
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

## The seven games

| | board | RAM | how it moves |
| --- | --- | --- | --- |
| Tetris | 10x20 | ~245 B | integer grid, gravity on its own clock |
| Snake | 16x16 | ~768 B | integer grid, ring of cells |
| Breakout | free | ~24 B | 8.8 fixed point |
| Pac-Man | 13x10 | ~170 B | integer grid, greedy chasers |
| Jumper | 16x12 | ~90 B | 8.8 fixed point, single screen |
| Invaders | 8x4 | ~70 B | one formation position, bitmask per row |
| Pong | free | ~30 B | 8.8 fixed point |

Snake's board is 16x16 of 12px cells, not 24x24 of 8px. At 8px the snake was
four faint slivers and the apple a speck -- on a panel you look at from across
a desk that is not detail, it is just small. The grid and the ring are both
O(cells), so the larger cells also cost a third of the RAM: 768 bytes rather
than 1,728.

All seven go through `struct nexus_game`, so the Game Center pages between
them with no per-game UI code, and each is one `#if` in `games[]`. Turning any
off with its `CONFIG_NEXUS_*` removes it from the build entirely.

### They all have levels now

Every game shows an **`L`** badge in its HUD, and every game can reach `L2`.
What advances it differs, because a level has to mean something in the game it
is in:

| | a level is | what changes |
| --- | --- | --- |
| Tetris | ten lines | its own gravity table, unchanged - it had levels first |
| Snake | five apples | the step interval it was already dropping |
| Breakout | the wall cleared | the wall comes back, the ball is faster |
| Pac-Man | the maze cleared | the maze refills, the chasers step sooner |
| Jumper | the flag reached | the **next of three boards**, then it wraps |
| Invaders | the fleet cleared | a new fleet, one row lower, marching sooner |
| Pong | every point played | the ball, whoever won the point |

Clearing the board is no longer an ending. Breakout, Pac-Man, Jumper and
Invaders used to stop and say `CLEARED`, which made winning and losing the
same event - the board froze and the score stopped. Now the only ending is
running out of lives, which is what makes the level worth printing: it is how
far you got.

`nexus_game_level_pct()` is the shared curve: **100% at level 1, +12% a level,
capped at 200%**. One helper for both kinds of clock, because a percentage
divides an interval and multiplies a velocity - and that is the whole reason
these games do not share a "speed" number.

The cap is not politeness. The tick can never go below
`CONFIG_NEXUS_UI_REFRESH_FAST_MS` because the panel will not repaint faster,
and two of the games collide by testing a point rather than a swept segment:

- Breakout's `hit_bricks()` tests the ball's **centre** against one cell, so a
  tick longer than the 9px brick row steps straight through a brick without
  ever being inside it. Unclamped, LUDICROUS on a late board is 11.2px.
- Pong's paddle test is the same shape against an 18px window, and unclamped
  reaches 20.5px.

Both clamp to their own geometry -- `TO_FIX(BRICK_H - 1)` and
`TO_FIX(PAD_W + BALL_R - 1)` -- rather than to a number that happens to work,
and the test derives the same bound from the Kconfig defaults.

Snake is the one game that does not use the shared curve: it had its own
speed-step in milliseconds long before there was a level, so its badge is
simply that step given a number. Two curves stacked on one game is how a
difficulty setting stops meaning anything.

### One header

Seven games used to draw seven headers. Snake and Pac-Man labelled the score,
Breakout and Invaders showed a bare number, Tetris put it in a side panel
entirely -- so paging between them moved the furniture, and the set read as
seven programs rather than one product with seven games.

`nexus_draw_game_header()` draws all of them: title, `HOLD=EXIT`, score, and
whichever of level, lives and one note the game has. The block it owns is
fixed at `y 0..NEXUS_HUD_END`, so nothing shifts when you page.

That constant is **34**, and Tetris decides it rather than taste: its well is
20 rows of 10px plus a 2px frame either side, which is 204 of the panel's 240
and leaves exactly 36. Two rows of 14px text take 28 of that, and the eight
left over are not margin to spend -- they are three gaps that all have to
exist: four above the title, two between the title and the score, and two
below the score so it does not sit flush on the playfield.

The score is therefore `NEXUS_TXT_BODY` in all seven games. A `NEXUS_TXT_VALUE`
score would need 41px of header and Tetris has 36, so the only way to a bigger
score everywhere is a smaller Tetris well.

Tetris keeps the side panel for `LEVEL`, `LINES` and `NEXT` -- those are
genuinely game-specific -- and having lost the score card, each of the three
gets more room: the next-piece preview is 10px a cell rather than 8. It is
also the one game with no `L` badge, because its panel already says LEVEL in
words and two of them would be a duplicate.

Pong is the other game whose HUD had to bend. It used to put two big numerals
either side of centre, which at `NEXUS_TXT_BIG` ran into the court and the
ball passed through them. Its score is now where every other game's is and in the
same colour, with the opponent's beside it in the red of the paddle it
belongs to.

Playfield borders take `t->radius` in every game, the same corner the
dashboard panels use -- five of them had a hardcoded 3.

### Playfields are flat

The ground is a gradient with two soft colour blobs behind it. That is right
for a dashboard and wrong behind a game: the blob edge is a smooth curve
crossing the play area, which on a 240px panel reads as a tear in the image
rather than as decoration, and everywhere it does not tear it lowers contrast
on whatever you are trying to track.

`nexus_draw_field()` paints the playfield opaque, so the decoration stops at
the field border and keeps the header, the cards and the menus. Breakout and
Invaders had no fill at all before this -- the raw ground showed through their
entire field.

Opaque rather than skipping the blobs for the bands inside the field, which is
cheaper and was the obvious approach: the fields are not the full width of the
panel, so the glow would stop dead along the field's top edge and carry on in
the few pixels of margin either side of it. That is a seam across the whole
screen -- one artifact traded for a worse one.

It is not free. Covering the ground costs an opaque fill where skipping it
would have saved a blended one: the five games that already had a translucent
fill get slightly cheaper, and the two that had none pay about 40,000 opaque
writes a frame against the 57,600 the gradient already costs. That is the
price of not having a seam.

### They all look like one product

Anything solid is drawn through `nexus_draw_block()`, anything round through
`nexus_draw_orb()`, and both agree that the light comes from the top-left: a
contact shadow, light pooling down from the top edge, a lit top-left bevel and
a shaded bottom-right one. Before that, each game shaded its own pieces its own
way, which is what makes a collection look assembled rather than designed.

A square block is drawn with radius 0 on purpose where pieces tile - a rounded
one leaves a gap against its neighbour, so a ledge reads as a row of separate
lozenges instead of one surface.

### Jumper, and why it is single-screen

It replaces a side-scrolling platformer that was removed after four rounds of
tuning failed to make it feel good, and the reason is structural rather than a
matter of constants.

A scrolling platformer must repaint **every** row on any frame the camera
moves, because every row's contents shift. On this panel that is the whole play
area, and once you are running the camera moves on most frames - so the cost of
the worst frame becomes the cost of the normal frame. No physics tuning fixes
that.

Fix the level to one screen and the background never changes. Only the player,
the enemies and a collected coin are ever dirty, which is about three tile rows
- a fifth of the work, on every frame, permanently. The game got smooth by
having less to draw rather than by drawing faster.

Grounded means "is there floor under me", probed one pixel down, not "did I
collide going down this tick". The latched version alternates true and false
every tick while you stand still, because landing zeroes the vertical velocity
and the next tick's fraction of a pixel moves nothing - which silently ate half
the jump presses in the game this replaced. A jump pressed just before touching
down is buffered rather than dropped.

### Invaders

The fleet is a **bitmask per row** - eight columns in a `uint16_t` - and it has
one position and one direction, with every alien drawn at an offset from it. A
formation of 32 therefore costs about the same to simulate as a single sprite.

It turns on the **live** extent rather than its nominal width: clearing the
left column has to let it slide further left, and testing the origin instead
makes it turn early against a wall that is not there.

Speed rises as the fleet thins because there are fewer aliens to step, which is
the arc of the original - nothing ramps a difficulty variable.

`SELECT` fires rather than pauses while a round is running. A shooter whose
main button pauses is unplayable on the dongle's own button, and pause is still
a hold away.

Clearing the fleet starts the next wave rather than ending the round: same
formation, one row lower, marching sooner. The drop caps at six waves, because
past that the fleet starts level with the cannon, which is not difficulty but
an instant loss.

**Hold to fire.** The original allowed one shot at a time, which is what made a
miss cost you something - but that rule was written for a cabinet with its own
fire button. Here the key repeats every `CONFIG_NEXUS_ACTION_REPEAT_MS`, and
under the old rule holding it gave you a shot, a long wait while it flew the
length of the field, then another: the gun felt broken rather than strict. Now
it fires every four ticks with up to three in the air. The cap is what keeps it
a game - three in flight is a stream you still have to aim.

### Pong

The cheapest game here: two paddles, a ball, no board. Where the ball lands on
the paddle steers it, exactly as in Breakout - without that one line the angle
never changes and a rally is a metronome neither player can influence.

The opponent is deliberately beatable. It moves at **two thirds of the ball's
own speed** and only reacts once the ball is coming at it; a paddle that tracks
exactly never loses, and a game you cannot win is a screensaver.

That fraction is the whole difficulty curve, and it used to be a constant 4 px
per tick - which was *faster* than the ball's steepest return. The opponent
could not be beaten by aiming, only outlasted, so points ran until someone got
bored. The game read as slow because the rally never ended, not because the
ball was gentle. It is a fraction of `speed()` now, so it keeps tracking the
ball if you move the knob.

The ball crosses the court in a little over half a second, and the paddle's
travel per key repeat is sized against that - a ball you cannot reach is not
difficulty, it is a bug.

Pong has no board to clear, so its level is the point you are on: every point
played speeds up the next ball, whoever won it. A match to seven is therefore a
short difficulty curve rather than seven identical rallies.

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

A 13x10 maze at 18px cells, 61 dots, four power pellets, three chasers and a
wrapping tunnel row.

The board started at 19x15 of 12px and was rebuilt bigger after the first
hardware test: the finer version looked correct in a render and read as a
texture rather than a game from the distance a dongle is actually looked at.
Fewer, larger cells cost maze complexity and buy pieces you can see - the
same trade Snake made going from 24x24 to 16x16.

**The maze is stored as text** and decoded once at round start:

```c
"####.###.#.###.####",
".........G.........",      /* the tunnel - the only row that wraps */
"####.###.#.###.####",
```

That costs 130 bytes of flash for the layout and buys a level you can read and
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
and persists with sound, theme and brightness. It applies to every game
with a clock, and takes effect on the next round.

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
| `NEXUS_JUMPER_TICK_MS` | 16 | platformer physics - **the jump is tuned to it** |
| `NEXUS_INVADERS_TICK_MS` | 16 | fleet and shot simulation |
| `NEXUS_PONG_TICK_MS` | 16 | ball simulation |
| `NEXUS_PONG_BALL_SPEED` | 320 | hundredths of a pixel per tick |
| `NEXUS_PONG_PADDLE_STEP` | 14 | paddle travel per key repeat |

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
Breakout. Otherwise `I` would simply be inert in most of the games, which
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
