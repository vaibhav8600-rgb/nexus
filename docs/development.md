# Development

## Host tests

The two pieces with real logic are deliberately free of RTOS dependencies, so
they test in seconds without a board:

```sh
# Tetris rules engine
cc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/tt \
   tests/tetris/test_tetris.c src/games/tetris/tetris_core.c && /tmp/tt

# Splash asset pipeline
python3 tests/splash/test_png2c.py
```

CI runs both, plus the Tetris tests again under ASan/UBSan.

On Windows, or anywhere without a C compiler, the splash test still runs on
its own -- which is part of why the PNG decoder is hand-rolled Python and not
a C tool.

`tetris_core.c` includes nothing but `<string.h>` and its own header. Keep it
that way -- the moment it needs `<zephyr/kernel.h>` the tests stop being
runnable and start being a firmware build.

## Local firmware build

> **Verification status.** The command below is the same one
> `.github/workflows/build.yml` runs, so it is not invented -- but it has not
> been executed on a developer machine for this release: the environment it
> was authored in had no Zephyr SDK, no `west` and no ARM toolchain. Treat CI
> as the authority. If you are the first to run it locally and it needs a
> tweak, that tweak belongs in this file.

```sh
# One-time workspace setup
mkdir nexus-ws && cd nexus-ws
west init -l /path/to/nexus/.github/ci
west update
west zephyr-export

# Build the standalone demo
west build -s zmk/app -b nice_nano_v2 -S studio-rpc-usb-uart -- \
  -DSHIELD="nexus_dongle nexus_dongle_demo" \
  -DZMK_EXTRA_MODULES="/path/to/nexus"
```

Output is `build/zephyr/zmk.uf2`.

With your own config repo, add `-DZMK_CONFIG=/path/to/zmk-config/config` and
swap the shield list.

A pristine rebuild after changing Kconfig defaults or devicetree:

```sh
west build -t pristine
```

## Verified against

| | |
| --- | --- |
| ZMK | `main`, verified 2026-09 |
| Zephyr | 3.5.x (whatever ZMK's `west.yml` pins) |
| LVGL | present but unused for drawing; `LV_COLOR_DEPTH=16`, `LV_Z_VDB_SIZE=10`, `LV_COLOR_16_SWAP=n` |
| Rendering | NEXUS strip compositor, `src/ui/gfx.c`, 240x12 RGB565 band |
| Board | nRF52840 ProMicro-compatible (`nice_nano_v2` tested) |
| Display | Zephyr `sitronix,st7789v` driver, 240x240, RGB565 |
| Studio | `studio-rpc-usb-uart` snippet, `CONFIG_ZMK_STUDIO=y` |

NEXUS tracks ZMK `main` on purpose: Studio and the split battery-fetch APIs it
depends on are only there. Section 125 allows that as long as the intent is
stated rather than accidental, which is what the comment block in
`examples/nexus-config/config/west.yml` is for.

If you would rather not have someone else's merge break your build, pin it:

```sh
west list zmk -f '{revision}'     # the commit you last built successfully
```

and put that SHA in your `west.yml`. The table above is what to re-check when
you bump it.

## ZMK API surface

NEXUS touches ZMK in exactly two files. Everything else in the module is pure
Zephyr or plain C.

| File | What it uses |
| --- | --- |
| `src/status/zmk_events.c` | The event manager, layer/WPM/battery/endpoint/HID-indicator events, `zmk_keymap_*`, `zmk_ble_*`, `zmk_usb_*`, `zmk_endpoint_get_selected()`, `zmk_hid_get_explicit_mods()`. |
| `src/behaviors/behavior_nexus_action.c` | `BEHAVIOR_DT_INST_DEFINE` and `struct behavior_driver_api`. |
| `src/ui/screen.c` | `zmk_display_status_screen()` (the extension point ZMK calls) and `zmk_display_work_q()`. |

If a ZMK bump breaks the build, start at `nexus_layer_name()` in
`zmk_events.c`. Layer naming is the API that has moved most: current ZMK
separates layer *index* from layer *id* and needs
`zmk_keymap_layer_index_to_id()`; older trees had a single `uint8_t` and
`zmk_keymap_layer_label()`.

## Architecture rules CI enforces

- No `P0.xx` or `&gpio0 N` outside the shield overlays and the HAL.
- No brand string in `src/` or `include/`.
- Every devicetree binding parses and declares a `compatible`.
- Every `*.zmk.yml` parses and declares an id.

## Adding a subsystem

Follow what is already there:

1. Public header in `include/nexus/`, internals in `src/nexus_priv.h`.
2. Sources guarded in `CMakeLists.txt` with
   `zephyr_library_sources_ifdef(CONFIG_NEXUS_X ...)` so a build without it
   pays nothing.
3. Kconfig option with a `default` and a `depends on`.
4. If it can fail, return an error and let `nexus.c` record it in
   `struct nexus_health`. Nothing in NEXUS may stop ZMK.
5. If it has branches or loops, leave one runnable check behind.

## Hardware test procedure

Run this end to end after any wiring change (Section 111).

| # | Step | Pass looks like |
| --- | --- | --- |
| 1 | Power on over USB | Board enumerates; no reset loop |
| 2 | Splash | Artwork and all three text lines, correct orientation |
| 3 | Display | Home dashboard, no tearing, no inverted colours |
| 4 | Colours | If everything is a photo-negative, `CONFIG_LV_COLOR_16_SWAP` is wrong |
| 5 | Buzzer | Startup jingle plays four rising notes, not one flat beep |
| 6 | Action button, short | Home → Game Center |
| 7 | Action button, long | Home → Settings, and no burst of short presses |
| 8 | Left half | Battery card goes `--` → a number within ~30 s |
| 9 | Right half | Same |
| 10 | Typing | WPM card climbs smoothly, layer name changes with layers |
| 11 | USB / BLE | Endpoint glyph and colour follow the active transport |
| 12 | Studio | See [zmk-studio.md](zmk-studio.md#verifying-it-works) |
| 13 | Tetris | Launch, move, rotate, drop, clear a line, pause, resume, game over, restart |
| 14 | Reset | Press reset *during* a game with Studio connected: MCU restarts, ZMK boots, halves reconnect |

Step 14 is the one people skip. It is the one that matters: the reset button
must work when the UI has crashed, and it can only do that because it is wired
to `RST` and NEXUS has no code near it.

## Failure modes to check deliberately

| Disconnect | Expected |
| --- | --- |
| The display | Keyboard still types. `nexus_health()->display` is false. |
| The buzzer | Keyboard still types. Sound silently no-ops. |
| The action button | Keyboard still types. UI is display-only. |
| One half | Other half and host keep working; that card shows `--`. |

If any of those bricks the keyboard, that is a bug in NEXUS, not a
configuration problem (Sections 87, 113, 141-A).
