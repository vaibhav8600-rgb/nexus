# The screens

Every symbol NEXUS puts on the panel, what its states mean, and which button
gets you between them.

The glyphs below are **rendered from the firmware's own bitmaps** by
`scripts/gen_doc_images.py`, on the theme colour they are actually drawn in.
They are not screenshots and they cannot drift: change a glyph or a palette in
the C, re-run the script, and these images change with it.

## Getting around

Seven screens. One button drives all of them.

```
   splash ──► home ◄──────────────► settings ──┬──► diagnostics
              │  ▲                             └──► about
        tap   │  │ hold
              ▼  │
        game center ──tap──► game ──hold──► home
              ▲   │
              └───┘ double-tap: next game
```

| screen | tap | hold | double-tap |
| --- | --- | --- | --- |
| **Home** | Game Center | Settings | -- |
| **Game Center** | play the selected game | Home | next game |
| **Game** | pause / resume / restart | quit to Home | -- |
| **Settings** | move to the next row | activate that row | -- |
| **Diagnostics** | move to the next row | activate that row | -- |
| **About** | back | Home | -- |
| **Splash** | skip | skip | -- |

The lists invert the usual pairing on purpose. With one button, moving is the
thing you do constantly and committing is the thing you do once, so the cheap
gesture is the cursor and the deliberate one is the change -- and it means a
stray tap can never flip a setting. You leave a list through its own `BACK`
row, which is just another row to hold.

`J` / `L` also move the Game Center's selection.

Only the Game Center declares a double-tap, and only it pays for one: a screen
with `btn_double` must hold a single tap back until the window closes
(`CONFIG_NEXUS_BUTTON_DOUBLE_MS`, 280 ms) to find out whether a second is
coming. Everywhere else a tap dispatches the instant the button comes up.

## Home

```
 ┌────────────────────────────────────────┐
 │                NEXUS                   │  brand plate
 ├──────────────────────┬─────────────────┤
 │  ▯ ✱ 1 ▣             │ LAYER           │  connectivity  ·  layer
 │                      │ DEFAULT         │
 ├──────────────────────┼─────────────────┤
 │  ^ ⇧ ⌥ ⌘             │ WPM             │  modifiers  ·  typing speed
 │                      │ 000             │
 ├──────────────────────┼─────────────────┤
 │ LEFT                 │ RIGHT           │  the two halves' batteries
 │ ▁▁▁▁                 │ ▁▁▁▁            │
 └──────────────────────┴─────────────────┘
```

The title is drawn as glass -- a near-white face over a soft accent halo, with
a one-pixel bevel -- in the active theme's colours. `CONFIG_NEXUS_PRODUCT` sets
the word; nothing sits under it, deliberately.

## Connectivity

Four symbols, read left to right. They answer four separate questions, and
keeping them separate is the point: one highlighted icon cannot say "BLE is
selected but that profile has never paired".

### 1. USB

| | state |
| --- | --- |
| <img src="images/ui/usb-ready.png" height="34"> | **Cable in, HID ready.** The arrow inside the plug is live, and the symbol is in the primary colour -- so USB is also the *selected* transport. |
| <img src="images/ui/usb-ready-idle.png" height="34"> | **Cable in, but BLE is selected.** Same plug, muted. Output is going to a Bluetooth host; USB is only powering the dongle. |
| <img src="images/ui/usb-nohid.png" height="34"> | **Cable in, HID not ready.** The arrow becomes a cross. Usually a charge-only cable, or a host that has not enumerated the keyboard. |

### 2. Bluetooth

The Bluetooth mark is static. Its only job is to say which transport is
selected -- colour carries that, not shape.

| | state |
| --- | --- |
| <img src="images/ui/ble-selected.png" height="34"> | **BLE is the selected transport.** Primary colour. |
| <img src="images/ui/ble-idle.png" height="34"> | **USB is selected.** Muted. |

Exactly one of the two transport symbols is ever in the primary colour.

### 3. Profile number

The current BLE profile, 1-5, at the largest text size on the screen -- it is
the thing you check right after `&bt BT_SEL`. NEXUS shows it **one-based**,
while `BT_SEL` takes 0-4: `&bt BT_SEL 0` lights up `1`.

It is drawn in the accent colour on BLE and in the caption colour on USB, so a
profile number you cannot currently use does not look active.

### 4. Profile status

A bordered tile answering "is this profile usable", independently of which
transport is selected.

| | state |
| --- | --- |
| <img src="images/ui/tile-open.png" height="34"> | **Open.** Nothing has ever paired to this profile. Put the host in pairing mode. |
| <img src="images/ui/tile-down.png" height="34"> | **Bonded, not connected.** A host is remembered but not here -- asleep, out of range, or connected to something else. |
| <img src="images/ui/tile-ok.png" height="34"> | **Bonded and connected.** Type. |

## Modifiers

Four recessed slots, always all four. An unheld modifier is visibly an *empty
slot* rather than something that failed to draw -- four bare symbols on a flat
panel read as unfinished.

| | held | | idle |
| --- | --- | --- | --- |
| <img src="images/ui/mod-ctrl-on.png" height="32"> | **Ctrl** | <img src="images/ui/mod-ctrl.png" height="32"> | |
| <img src="images/ui/mod-shift-on.png" height="32"> | **Shift** | <img src="images/ui/mod-shift.png" height="32"> | |
| <img src="images/ui/mod-alt-on.png" height="32"> | **Alt** | <img src="images/ui/mod-alt.png" height="32"> | |
| <img src="images/ui/mod-gui-on.png" height="32"> | **GUI** | <img src="images/ui/mod-gui.png" height="32"> | |

A held slot changes three things at once -- glyph colour, slot fill and border
-- so it reads without relying on colour alone.

## Settings

| row | does |
| --- | --- |
| `SOUND` | ON / OFF. |
| `BRIGHT` | Backlight level, if the panel has a controllable one. |
| `THEME` | Cycles the seven palettes below. |
| `ANIM` | Whether meters animate to their new value. Read-only display of the build option. |
| `SPEED` | SLOW / EASY / NORMAL / FAST / INSANE / LUDICROUS. Applies to all three games. |
| `SNAKE WALL` | ON = fatal edges, OFF = the board wraps. |
| `SPLASH` | Which splash the build got. Read-only. |
| `GAMES` | How many games are built in. Read-only. |
| `DIAG` | Opens Diagnostics. |
| `ABOUT` | Opens About. |
| `SAVE` | Commits now rather than waiting for the autosave. |
| `BACK` | Leaves the list. |

Everything editable persists to `nexus/ui/prefs` and survives a reflash.
Changes are written after a quiet period (`CONFIG_NEXUS_SETTINGS_AUTOSAVE_MS`,
4 s) so spinning through seven themes costs one flash erase, not seven.

## Diagnostics

`FIRMWARE`, `BOARD`, `DISPLAY`, `BACKLIGHT`, `BUZZER`, `BUTTON`, `HOST`,
`L/R LINK`, `L/R BATT`, `UI STATIC`, `UPTIME`, and `FPS` with
`CONFIG_NEXUS_DEBUG=y`.

`UI STATIC` is the one worth explaining: it is the module's **static RAM
footprint** -- the `.bss` and `.data` NEXUS itself owns -- not free memory and
not the heap. It is a number you compare against itself after a change, which
is exactly what it was added for.

## Themes

`CONFIG_NEXUS_THEME="NAME"`, or cycle them live in Settings. An unknown name
falls back to `NEXUS` rather than failing the build over a typo.

Each strip is that theme's real palette, quantised to the RGB565 the panel
receives -- so these are the colours you get, not the colours the source asks
for.

| palette | | notes |
| --- | --- | --- |
| **NEXUS** | <img src="images/ui/theme-nexus.png" height="26"> | The default. Deep indigo ground, mint accent, pink alt. |
| **AMOLED** | <img src="images/ui/theme-amoled.png" height="26"> | Same language on true black. An OLED burns no power on black, so the gradient and both glow blobs come off. |
| **DAYLIGHT** | <img src="images/ui/theme-daylight.png" height="26"> | Light ground. Panes are *brighter* than the background, so the shaded bottom edge does the work the highlight does elsewhere. |
| **CLAY** | <img src="images/ui/theme-clay.png" height="26"> | Neumorphic. Panes are the same colour as the ground and read only through their lit and shaded edges. |
| **ESPRESSO** | <img src="images/ui/theme-espresso.png" height="26"> | Warm dark, amber accent. |
| **MINT** | <img src="images/ui/theme-mint.png" height="26"> | Dark green ground, high-chroma mint. |
| **SUNSET** | <img src="images/ui/theme-sunset.png" height="26"> | The full-height gradient, violet at the top into amber at the bottom. The clearest demonstration that the pane tint is a real composite: the same panel colour reads violet up top and amber below, because it is. |

Swatch order: `bg_bot`, `panel`, `value`, `accent`, `accent_alt`, `warning`,
`error`.

### The wordmark ramp

Each theme also carries a five-stop ramp for the title, brightest to darkest:
`[0]`/`[1]` shade the face, `[2]` is the halo behind it, `[3]`/`[4]` the two
graded pixels of shade under every edge.

| | ramp |
| --- | --- |
| NEXUS | <img src="images/ui/wordmark-nexus.png" height="16"> |
| AMOLED | <img src="images/ui/wordmark-amoled.png" height="16"> |
| DAYLIGHT | <img src="images/ui/wordmark-daylight.png" height="16"> |
| CLAY | <img src="images/ui/wordmark-clay.png" height="16"> |
| ESPRESSO | <img src="images/ui/wordmark-espresso.png" height="16"> |
| MINT | <img src="images/ui/wordmark-mint.png" height="16"> |
| SUNSET | <img src="images/ui/wordmark-sunset.png" height="16"> |

See [themes.md](themes.md) for what every field means and how to add a palette.

## Games

Three, all reached from the Game Center. `J`/`L` page between them, the button
plays, double-tap skips to the next one.

| | board | RAM | |
| --- | --- | --- | --- |
| **Tetris** | 10x20 | ~245 B | Seven-bag randomiser, symmetric wall kicks, 100/300/500/800 per 1-4 lines times level. |
| **Snake** | 16x16 | ~768 B | Walls or wrap, toggled in Settings. Moving into your own vacated tail is legal. |
| **Breakout** | free | ~24 B | 8.8 fixed point, 5x8 bricks, three lives, paddle position steers the ball. |

High scores persist per game and are written only when a record actually
improves. Difficulty is a **runtime** setting, not a build option.

Full rules, controls and tuning: [games.md](games.md).

## Splash

Boots into a drawn badge -- two corner discs, a ringed disc with the `N`, the
wordmark, the subtitle, a hairline and the brand -- costing a few hundred bytes
of code rather than the 115,200 the same picture would cost as a PNG.

Point `CONFIG_NEXUS_SPLASH_IMAGE` at your own artwork and it replaces the badge
entirely, text included. See [splash.md](splash.md).

## Regenerating these images

```sh
python3 scripts/gen_doc_images.py
```

Reads `src/ui/home.c` and `src/ui/theme.c` and rewrites `docs/images/ui/`.
Stdlib only -- no Pillow, for the same reason `png2c.py` has none.
