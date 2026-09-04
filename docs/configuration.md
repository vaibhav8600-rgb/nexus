# Configuration

Everything is Kconfig or devicetree. If you find yourself editing a file under
`src/`, something is missing here -- open an issue.

## Branding

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_BRAND` | `"VAIBHAV TECH"` | Small line above the wordmark on the splash and About screens. Empty hides it. |
| `CONFIG_NEXUS_PRODUCT` | `"NEXUS"` | The wordmark itself. Rendered as large as it fits (up to 35 px), weighted, over an accent rule; a longer name steps down a size rather than overflowing. |
| `CONFIG_NEXUS_AUTHOR` | `"VAIBHAV RAJPUT"` | Creator credit on the About screen. Empty hides the card. |
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
| `CONFIG_NEXUS_SOUND_STARTUP` | `y` | The fanfare on the splash. |
| `CONFIG_NEXUS_SOUND_SPLIT` | `y` | Chirps when a half connects or disconnects, and on deep sleep / wake. |

There is also a runtime toggle on the Settings screen. Volume is the hardware
potentiometer; there is no software volume and NEXUS does not pretend otherwise.

Every effect sits in octaves 5-7. A passive piezo is a mechanical resonator
rather than a speaker, and its output peaks somewhere near 2-4 kHz: the same
50% square wave that is faint at 500 Hz is loud and bright at 2 kHz. This is
why snake-module's effects live on B6 and E7, and why pitching NEXUS's tables
up an octave did more for how they sound than any change to the notes.

### BLE profiles

The dashboard reads `zmk_ble_active_profile_index()`, `_is_open()` and
`_is_connected()` on every endpoint change, **not** only while BLE is the
selected endpoint. So the profile number and its status tile stay live while
you are typing over USB, and `&bt BT_SEL n` updates the screen either way.

| tile | meaning |
| --- | --- |
| dashed | open - never paired, advertising |
| cross | bonded, not connected right now |
| tick | bonded and connected |

The tile is the *profile's* state; the lit transport symbol is which endpoint
you are typing through. They are deliberately separate, because "USB is
selected and my BLE profile is fine" is a real and common state.

`&bt BT_CLR` and `&bt BT_CLR_ALL` both reach the screen: clearing a bond goes
through ZMK's `set_profile_address()`, which raises
`zmk_ble_active_profile_changed`. Clearing a profile that was already open
raises nothing, which is correct - nothing changed.

Profile count on a dongle is `CONFIG_BT_MAX_PAIRED` minus
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS`. With this repo's `7 - 2` that is
five, matching `BT_SEL 0` through `BT_SEL 4` in the keymap. Raise
`CONFIG_BT_MAX_PAIRED` if you add profiles, or the extra `BT_SEL` keys will
select profiles that do not exist.

### Half connect and disconnect

Four cues, deliberately systematic rather than four unrelated jingles - rising
means arrived, falling means gone, and the left half sits a fifth below the
right one. Learn one and you know all four. Deep sleep and wake get a slower,
softer pair, because they fire when nothing is happening and should read as a
sigh rather than an alert.

Nothing plays for the halves connecting during boot. That is not news, and it
would collide with the splash fanfare.

Both directions are real events. They come from Zephyr connection callbacks
on the dongle's own BLE links (`src/status/split_conn.c`), filtered to the
links where the dongle is the central - which is exactly the set of keyboard
halves, since the host connects to *us*.

That matters because there is nothing in ZMK to subscribe to: the split
central raises no events at all, and snake-module's `peripheral_status.c`
handler is an empty stub with "do we need this ?" in it. An earlier version of
this inferred a disconnect from a half going quiet for 150 s, which announced
a half leaving two minutes late and announced a "disconnect" every time a
sleeping half stopped reporting battery. Connection callbacks know the
difference between a half that dozed and a half that dropped.

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
| Settings / Diagnostics | next row | activate the row |
| About | back | Home |

`BACK` is a **row** on the menus, not a gesture: with one button you need three
verbs from two gestures, and making hold mean "activate" there but "go back"
everywhere else is how a menu stops being predictable. The screens say so --
`TAP=NEXT   HOLD=SELECT`.

### Modifier pills

Four 22 px pills, lit while the modifier is held, drawn with the symbol the key
itself is printed with rather than an initial:

caret = Ctrl · up arrow = Shift · option stroke = Alt · four panes = GUI

Left and right variants are merged -- there is no room for eight pills and no
value in telling them apart at a glance.

## Settings that survive a restart

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_SETTINGS_PERSIST` | `y` | Sound, theme and brightness are stored under `nexus/ui/prefs`. Without it the Settings screen edits only the running session. Needs `CONFIG_SETTINGS`. |
| `CONFIG_NEXUS_SETTINGS_AUTOSAVE_MS` | `4000` | Quiet period before a *deferred* save commits. Used by controls that fire repeatedly in one gesture, so spinning an encoder through seven themes costs one flash erase, not seven. |

Saving from the menu is explicit -- the `SAVE` row, which doubles as the
unsaved-changes marker (`*` dirty, `OK` clean). The encoder and
`&nexus_action NEXUS_ACT_SAVE` do not need it.

## Keymap control (`&nexus_action`)

Everything the physical button can do, plus the things one button cannot.
Include `<dt-bindings/nexus.h>` and bind any of:

| Action | Does |
| --- | --- |
| `NEXUS_ACT_HOME` / `NEXUS_ACT_GAME_CENTER` / `NEXUS_ACT_MENU` | jump to that screen |
| `NEXUS_ACT_UP` / `NEXUS_ACT_DOWN` | move a menu cursor; `DOWN` also soft-drops in Tetris |
| `NEXUS_ACT_SELECT` | activate a row, or pause / resume / restart a game |
| `NEXUS_ACT_BACK` | leave the current screen |
| `NEXUS_ACT_LEFT` / `RIGHT` / `ROTATE` / `DROP` | gameplay |
| `NEXUS_ACT_SAVE` | commit settings, from any screen |
| `NEXUS_ACT_THEME_NEXT` / `THEME_PREV` | cycle themes without opening Settings; saves itself once you stop |

Verbs are context-sensitive by design (Section 13): the screen decides what
`SELECT` means, not the key.

### Guarding keymap bindings

`#ifdef CONFIG_NEXUS` **does not work in a keymap.** Zephyr builds the
devicetree before it runs Kconfig, so `autoconf.h` does not exist yet and the
test is silently false. The `nexus_dongle` shield defines `NEXUS_DONGLE`
instead, and shield overlays are concatenated ahead of the keymap:

```c
#ifdef NEXUS_DONGLE
#include <dt-bindings/nexus.h>
/* ... bindings that need &nexus_action ... */
#endif
```

### Theme on an encoder

```dts
nexus_theme_enc: nexus_theme_enc {
    compatible = "zmk,behavior-sensor-rotate-var";
    #sensor-binding-cells = <2>;
    bindings = <&nexus_action>, <&nexus_action>;
    tap-ms = <20>;
};
/* ... then on the layer ... */
sensor-bindings = <&nexus_theme_enc NEXUS_ACT_THEME_NEXT NEXUS_ACT_THEME_PREV
                   &inc_dec_kp C_VOL_UP C_VOL_DN>;
```

## Split status

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_SPLIT_SWAP_SIDES` | `n` | ZMK numbers peripherals by pairing order, not by physical side. Set this if LEFT and RIGHT are the wrong way round. |
| `CONFIG_NEXUS_STATUS_STALE_MS` | `0` | Blank a half's battery after N ms of silence. **Off by default**: a half with `CONFIG_ZMK_SLEEP` stops reporting when idle, so any timeout eventually blanks a good reading. An idle half has a battery level; we just heard it a while ago. |
| `CONFIG_NEXUS_ANTI_IDLE_STATUS` | `n` | Cursor icon showing whether the mouse jiggler is on. Needs the `&anti_idle` behavior, which comes from snake-module, not from NEXUS. |

Battery levels require `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y`
on the dongle and `CONFIG_ZMK_BATTERY_REPORTING=y` on the halves. Without them
both cards read `--` forever, which is correct: unknown is not zero.

## Diagnostics

**`UI STATIC`** is the UI's own compile-time footprint - the 5,760-byte
compositor band plus the 224-byte menu value cache, 5,984 B in total. It is a
constant, not a live reading, and it is neither free nor used memory.

There is no free-RAM row because NEXUS has no heap to watch: every buffer it
owns is static, so the UI cannot run out of memory at an awkward moment.
Section 63 asks for "Free RAM"; on this design the honest answer is that the
number would be about Zephyr's heap, not NEXUS's, and inventing one would be
worse than none. For whole-image figures read the linker output, which CI
prints.

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
