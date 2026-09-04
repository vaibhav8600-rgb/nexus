# Architecture

## The one rule

NEXUS sits **on top of** ZMK. It does not fork it, wrap it, or replace any part
of it. BLE, USB HID, the split transport, keymap storage, Studio RPC, settings
and behaviors are all ZMK's, unchanged (Section 99).

Corollary: NEXUS is a *non-critical subsystem*. Every init can fail, every
public call is a no-op when its subsystem is absent, and no code path in this
module can stop the keyboard from typing (Requirement A).

## Layers

```
                    ZMK  (BLE · USB · split · keymap · Studio · settings)
                     │
      events ────────┤────────  zmk_display_status_screen()
                     │
   ┌─────────────────▼──────────────────────────────────────────┐
   │ src/status/zmk_events.c    the ONLY file touching zmk/events│
   └─────────────────┬──────────────────────────────────────────┘
                     ▼
              struct nexus_status  ──── observers ───► widgets
                     │
   ┌─────────────────▼──────────────────────────────────────────┐
   │ screen stack ─ home · game_center · game · menus · splash   │
   │      │           each is a draw() over the status model      │
   │      ├── widgets.c  (glass cards, pills, meters, wordmark)   │
   │      ├── theme.c    (every colour in the product)            │
   │      └── gfx.c      (240x12 band compositor -> display_write)│
   └─────────────────┬──────────────────────────────────────────┘
                     ▼
        game_manager ──► struct nexus_game ──► tetris.c
                                                  │
                                            tetris_core.c   (pure C, tested)
                     │
   ┌─────────────────▼──────────────────────────────────────────┐
   │ HAL: nexus_button · nexus_buzzer · nexus_backlight          │
   └─────────────────┬──────────────────────────────────────────┘
                     ▼
              devicetree · Zephyr drivers (GPIO, PWM, SPI, ST7789)
```

## Decisions worth knowing

### It renders as ZMK's status screen

`CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y` and NEXUS implements
`zmk_display_status_screen()`. ZMK already owns the display device, a tick and
a work queue, so this buys the whole display stack for free: no ST7789 init
code of our own, no display thread, and above all **no NEXUS thread**. Every
thread costs RAM this chip does not have (Section 89).

Everything in NEXUS runs on that one queue, which is also why nothing touches
the SPI bus from two contexts (Section 122).

### The UI is a strip compositor, not a widget tree

This is the decision the rest of the UI follows from.

A 240x240 RGB565 frame is 115,200 bytes. There is no version of "keep a
framebuffer" that fits next to ZMK, BLE, the split link and Studio on an
nRF52840. So `src/ui/gfx.c` composites **one 240x12 band at a time** into a
5,760-byte static buffer, pushes it with `display_write()`, and moves down.

That one choice pays for everything else:

- **Glass is real, not faked.** The ground is a linear vertical gradient, and
  blurring a linear gradient returns the same gradient -- so an alpha tint over
  it *is* a correct frosted composite. No blur pass, no full-frame
  read-modify-write. (Section 57 permits faking it; we did not need to.)
- **Partial repaint is free.** `gfx_render_range()` only touches bands that
  overlap the dirty rows. A WPM tick pushes 4 bands (23 KB) instead of 20
  (115 KB) -- Section 61 in one function.
- **No widget RAM, and nothing to keep in sync.** Screens are `draw()`
  functions over `struct nexus_status`, so the model cannot disagree with the
  display, and a screen change frees nothing because it allocated nothing.
- **No font library.** One 295-byte 5x7 bitmap font, integer-scaled to four
  sizes. Four Montserrat cuts would have been tens of KB of flash to render the
  same uppercase labels and numerals (Sections 102-103).

LVGL is still linked, because ZMK's status-screen API hands back an `lv_obj_t`
and Section 99 says do not fork ZMK to avoid that. It gets one empty object and
draws nothing after the initial clear, which is why the shield pins
`CONFIG_LV_Z_VDB_SIZE=10`: the buffer has to exist, not to be big.

The compositor is not new code. It is the renderer already proven on this exact
panel in the author's Snake dongle, brought across and generalised -- which is
what Section 127 asks for.

### There is no second event bus

Section 90 asks for a NEXUS event bus. Section 99 says do not reinvent ZMK. The
resolution: ZMK's event manager *is* the bus. `zmk_events.c` subscribes,
updates `struct nexus_status`, and fans out to widgets through one queued work
item carrying a changed-bitmask.

A widget repaints only what changed. WPM ticking does not touch the battery
cards (Section 61).

Building a second dispatch framework on top of ZMK's would have been ~200 lines
that do what 40 lines already do.

### Input is logical, everywhere

```
physical button ─┐
&nexus_action  ──┼──► nexus_action_dispatch(NEXUS_ACTION_*) ──► screen->action()
UI              ─┘                                                    │
                                                              nexus_game_input()
```

The button knows nothing about screens. Screens declare `btn_short` and
`btn_long` as data, so Section 12's context-sensitive mapping is a table rather
than an if-ladder. Tetris receives `NEXUS_ACTION_LEFT` and cannot tell whether
a finger or a keycap produced it (Sections 13, 42).

Dispatch is safe from ISR context: it pushes a byte into a `k_msgq` and queues
work. It never blocks the caller, and a full queue drops the action rather than
stalling an interrupt.

### The Tetris split

`tetris_core.c` is pure C -- `<string.h>` and its own header, nothing else. No
Zephyr, no NEXUS, no rendering. `tetris.c` does drawing, timing and sound.

That boundary is why the rules have 100+ assertions running in CI in under a
second, instead of "it looked right on the bench". Keep it.

### Memory choices

| Thing | Chosen | Obvious alternative | Why |
| --- | --- | --- | --- |
| Framebuffer | one 240x12 band, 5,760 B | full frame, 115 KB (double: 230 KB) | the panel alone is more RAM than the chip can spare |
| Tetris well | composited from the board bytes | 30x60 canvas at 3x zoom, 3.5 KB | with a band compositor the canvas has no job left |
| Board storage | `uint8_t[22][10]`, 220 B | one widget per cell | 200 objects is slower *and* bigger |
| Fonts | one 5x7 bitmap, 295 B | 4 Montserrat cuts | tens of KB to draw the same uppercase labels |
| Splash default | drawn with primitives, ~200 B | 160x160 bitmap, 51 KB | it is a logo, not a photograph |
| Sound | synthesised square waves | PCM samples | a passive buzzer wants a waveform anyway |
| Glass | alpha tint over a gradient | real backdrop blur | over a linear gradient the tint *is* the correct result |

### What is *not* built

Honest list, so nobody hunts for it:

- **Double-press on the action button.** Section 11 marks it optional.
  Supporting it means holding every short press for a 250 ms window to see if a
  second one arrives, which makes every single press feel laggy. Not worth it
  for one button.
- **Full SRS rotation.** Simple symmetric wall kicks instead. Marked with a
  `ponytail:` comment in `tetris_core.c` with the upgrade path. Nobody is
  T-spinning on a dongle.
- **A runtime theme editor.** Section 78 allows build-time only for v1. The
  Settings screen cycles the built-in palettes.
- **More than two split halves.** `nexus_status_peripheral_battery()` drops
  sources above 1, marked with a `ponytail:` comment. The dashboard has two
  battery cards because keyboards have two halves.
- **Colour depths other than RGB565.** The compositor is 16-bit throughout.
  A different panel format means one new `flush()` branch, not a rework.
- **A general widget toolkit.** Screens draw; they do not build trees. If you
  want a scrolling list with momentum, this is the wrong renderer.

### Priority order, when something has to give

```
1. ZMK keyboard functionality
2. BLE split reliability
3. USB
4. ZMK Studio
5. Status display
6. Games
7. Animations
8. Sound
```

This is Section 67, and it is the rule for every trade in this codebase. If a
visual feature threatens anything above it, the visual feature loses.

## Source map

| Path | |
| --- | --- |
| `boards/shields/nexus_dongle/` | The hardware. Pins live here and nowhere else. |
| `boards/shields/nexus_dongle_demo/` | Two-key standalone shield for bring-up and CI. |
| `dts/bindings/` | `nexus,button`, `nexus,buzzer`, the behavior binding. |
| `dts/behaviors/nexus_action.dtsi` | Declares `&nexus_action`. |
| `include/nexus/` | Public API. |
| `include/dt-bindings/nexus.h` | Action IDs, shared by C and devicetree so they cannot drift. |
| `src/status/` | ZMK adapter + the status model. |
| `src/ui/` | Screens, widgets, theme. |
| `src/games/` | Manager + Tetris. |
| `src/hal/` | Button, buzzer, backlight. |
| `src/behaviors/` | `&nexus_action`. |
| `scripts/png2c.py` | Splash asset pipeline. |
| `assets/splash_default.c` | Drawn fallback artwork. |
| `tests/` | Host-runnable checks. |
