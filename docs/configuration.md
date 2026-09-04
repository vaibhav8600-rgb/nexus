# Configuration

Everything is Kconfig or devicetree. If you find yourself editing a file under
`src/`, something is missing here -- open an issue.

## Branding

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_BRAND` | `"VAIBHAV TECH"` | Small line above the wordmark on the splash and About screens. Empty hides it. |
| `CONFIG_NEXUS_PRODUCT` | `"NEXUS"` | The wordmark itself. Keep it short -- it is rendered at 28 px with a five-layer extrude. |
| `CONFIG_NEXUS_SUBTITLE` | `"SMART ZMK DONGLE"` | Caption under the wordmark. Empty hides it. |

These are Kconfig *defaults*, not constants. Nothing in `src/` contains a brand
string, and CI fails the build if one appears (Requirement B).

## Display and UI

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_DISPLAY` | `y` | Turns the whole UI on. Selects ZMK's custom status screen. |
| `CONFIG_NEXUS_THEME` | `"NEXUS"` | See [themes.md](themes.md). An unknown name falls back to `NEXUS` rather than failing the build over a typo. |
| `CONFIG_NEXUS_DISPLAY_ROTATION` | `0` | 0/90/180/270, done in the compositor so all drawing stays in logical coordinates. Try the panel's `mdac` byte in your overlay first -- that rotates in the controller for free. 90/270 cost a second 5,760-byte transpose buffer. |
| `CONFIG_NEXUS_ANIMATIONS` | `y` | Battery bars animate to their new value instead of snapping. |
| `CONFIG_NEXUS_DEFAULT_SCREEN_HOME` | `y` | Or `_GAME_CENTER` / `_DIAGNOSTICS`. |
| `CONFIG_NEXUS_UI_REFRESH_FAST_MS` | `33` | Games and animations (~30 FPS). |
| `CONFIG_NEXUS_UI_REFRESH_NORMAL_MS` | `200` | Status dashboard. |
| `CONFIG_NEXUS_UI_REFRESH_IDLE_MS` | `1000` | Static screens. |
| `CONFIG_NEXUS_BACKLIGHT_TIMEOUT_S` | `0` | Blank after N idle seconds. `0` disables. Never fires during a game. Needs a controllable backlight -- see [hardware.md](hardware.md#backlight). |

The three refresh rates are the whole of Section 60's frame manager: each
screen declares which class it belongs to and the scheduler picks the period.
A screen can temporarily promote itself with `nexus_screen_request_refresh()`.

## Splash

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_SPLASH` | `y` | |
| `CONFIG_NEXUS_SPLASH_DURATION_MS` | `3000` | `0` skips the splash entirely. Any button press skips it early. |
| `CONFIG_NEXUS_SPLASH_IMAGE` | `""` | Path to a PNG, relative to your `config/` directory. Empty uses the drawn default. |
| `CONFIG_NEXUS_SPLASH_MAX_DIM` | `160` | Downscale ceiling. 160x160 costs 51 KB of flash; 240x240 costs 115 KB. |

Full details in [splash.md](splash.md).

## Sound

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_SOUND` | `y` | Master switch. Off removes the sound engine and the buzzer HAL from the build. |
| `CONFIG_NEXUS_UI_SOUND` | `y` | Navigation, connect/disconnect, menus. |
| `CONFIG_NEXUS_GAME_SOUND` | `y` | Everything a game asks for. |
| `CONFIG_NEXUS_SOUND_STARTUP` | `y` | The four-note jingle on the splash. |

There is also a runtime toggle on the Settings screen. Volume is the hardware
potentiometer; there is no software volume and NEXUS does not pretend otherwise.

## Games

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_GAME_CENTER` | `y` | The launcher. Selects `NEXUS_GAMES`. |
| `CONFIG_NEXUS_TETRIS` | `y` | |
| `CONFIG_NEXUS_GAME_HIGHSCORE_PERSIST` | `y` | Store high scores in Zephyr settings. Written only when a record actually improves. |

Turning `CONFIG_NEXUS_GAME_CENTER=n` removes the launcher, the game engine and
Tetris from the firmware entirely -- worth about 6 KB of flash and 4 KB of RAM.

## Input

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_BUTTON` | `y` | Needs a `nexus-button` alias in devicetree. Missing hardware degrades to "no button", never to a crash. |
| `CONFIG_NEXUS_BUTTON_DEBOUNCE_MS` | `30` | Tune on real hardware; 20-50 ms is the usable range. |
| `CONFIG_NEXUS_BUTTON_LONG_PRESS_MS` | `600` | Fires the moment the threshold passes, not on release. |
| `CONFIG_NEXUS_BEHAVIOR_ACTION` | `y` | The `&nexus_action` keymap behavior. |

### What the button does

The mapping is data on each screen, not a switch statement (Section 12):

| Screen | Short press | Long press |
| --- | --- | --- |
| Splash | skip | skip |
| Home | Game Center | Settings |
| Game Center | launch selected | Home |
| Game (running) | pause | leave game |
| Game (paused) | resume | leave game |
| Game (over) | restart | leave game |
| Settings / Diagnostics | next row | activate / back |
| About | back | Home |

### Modifier pill legend

The dashboard shows four 22 px pills. They light when the modifier is held:

`C` Ctrl · `S` Shift · `A` Alt · `G` GUI

Left and right variants are merged -- there is no room for eight pills and no
value in distinguishing them at a glance.

## Split status

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_SPLIT_SWAP_SIDES` | `n` | ZMK numbers peripherals by pairing order, not by physical side. Set this if LEFT and RIGHT are the wrong way round. |
| `CONFIG_NEXUS_STATUS_STALE_MS` | `120000` | A half that has said nothing for this long shows `--` and `RECONNECTING` instead of a stale percentage. |

Battery levels require `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y`
on the dongle and `CONFIG_ZMK_BATTERY_REPORTING=y` on the halves. Without them
both cards read `--` forever, which is correct: unknown is not zero.

## Diagnostics

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_DEBUG` | `n` | Adds an FPS row to Diagnostics, counted from frames actually pushed to the panel. Pair with `CONFIG_NEXUS_LOG_LEVEL_DBG=y` for verbose logging. |
| `CONFIG_NEXUS_LOG_LEVEL` | `2` (warning) | Standard Zephyr log level. |

Leave debug off in daily use. USB logging measurably raises idle power draw and
NEXUS is not worth the milliamps.

## ZMK options NEXUS wants

Set by `nexus_dongle`'s `Kconfig.defconfig`, listed so you know what changed:

```
CONFIG_ZMK_SPLIT=y
CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
CONFIG_ZMK_DISPLAY=y
CONFIG_ZMK_WPM=y                 # or the WPM card reads 000 forever
CONFIG_ZMK_HID_INDICATORS=y      # or caps lock never lights
CONFIG_BT_MAX_CONN=6
```

All of them are `default`, so your own `.conf` still wins.
