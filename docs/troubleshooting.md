# Troubleshooting

## Display

**Everything redraws slowly, or the game feels like a slideshow.**
Check which SPI instance the panel is on. Only SPIM3 runs above 8 MHz on an
nRF52840, and the driver silently clamps rather than warning, so a panel on
spi0 pushes a full frame in ~115 ms however high `mipi-max-frequency` is set.
`nexus_dongle.overlay` uses `&spi3` for exactly this reason.

**Nothing on the screen, but the keyboard types.**
Working as designed -- NEXUS never takes the keyboard down with it. Check
`CONFIG_ZMK_DISPLAY=y`, then the wiring: `CS` and `BL` are the two pins the
requirements document and the wiring diagram disagree about. See
[hardware.md](hardware.md#pin-map).

**Everything is a photo-negative.**
`CONFIG_LV_COLOR_16_SWAP` is wrong for your panel. The shield defaults it to
`y` because ST7789 wants big-endian pixels. Set it to `n` in your `.conf` if
your module differs.

**Image is offset by a few pixels, or there is a bright line at one edge.**
`x-offset` / `y-offset` in the `st7789v` node. 240x240 modules vary; 0/0 is the
common case but some need 0/80.

**Rotated the wrong way.**
`CONFIG_NEXUS_DISPLAY_ROTATION` asks the driver, and most panel drivers refuse.
The free fix is the panel's own scan order: change `mdac` in the overlay
(`0x00`, `0x60`, `0xC0`, `0xA0` for 0/90/180/270). Look for the log line
"driver refused rotation" -- if it is there, use `mdac`.

**Garbled or torn.**
Drop `spi-max-frequency` from `32000000` to `16000000`. Long dupont jumpers do
not carry 32 MHz.

**The layer name changes size when I switch layers.**
Fixed. The name is capped at `NEXUS_TXT_BODY`, so every layer name up to eight
characters renders identically; `fit_scale()` only steps down beyond that, to
keep a very long name inside its card. An earlier build capped it a size
higher, which let four- and five-character names render larger than DEFAULT.

## Batteries

**Both cards say `--` forever.**
That is the honest answer for "unknown", not a bug (Section 26). Check:

- Dongle: `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y`
- Halves: `CONFIG_ZMK_BATTERY_REPORTING=y`
- The halves are actually paired. Flash `settings_reset` to everything and
  re-pair.

Levels arrive on ZMK's own reporting interval -- allow a minute.

**LEFT and RIGHT are swapped.**
ZMK numbers peripherals by pairing order, not by physical side. Set
`CONFIG_NEXUS_SPLIT_SWAP_SIDES=y`.

**A card went back to `--` while the half is clearly working.**
`CONFIG_NEXUS_STATUS_STALE_MS` (default 120 s) expired. Either the half really
did go quiet, or its reporting interval is longer than the timeout -- raise the
timeout.

## Button

**One press produces several actions.**
Raise `CONFIG_NEXUS_BUTTON_DEBOUNCE_MS` toward 50.

**A long press fires a short press too.**
It should not -- the release path checks whether long already fired. If you see
it, the button is bouncing hard on release; raise the debounce.

**Long press never fires.**
Lower `CONFIG_NEXUS_BUTTON_LONG_PRESS_MS`, and check the pull-up. The overlay
assumes active-low with an internal pull-up (`GPIO_ACTIVE_LOW | GPIO_PULL_UP`),
i.e. the switch shorts P0.31 to ground.

**Nothing at all.**
Check the Diagnostics screen: `BUTTON` shows `N/A` if devicetree has no
`nexus-button` alias, and `OK` if the GPIO came up.

## Sound

**Silent.**
In order: `CONFIG_NEXUS_SOUND=y`; a `nexus-buzzer` alias exists; the buzzer is
*passive*; the volume pot is not at zero; Diagnostics shows `BUZZER: OK`.

**One flat tone regardless of what should be playing.**
The buzzer is active, not passive. An active buzzer has its own oscillator and
ignores the waveform. Replace it.

**Very quiet.**
The pot, or the duty cycle. NEXUS drives 50%, which is the loudest a square
wave gets; there is no software volume. Note that a passive piezo is much
quieter well below its resonance (~2-4 kHz) - which is why every effect is
pitched into octaves 5-7. A buzzer that is quiet on *everything* is usually
the pot; one that is quiet only on the low notes is physics.

**A half chirps "disconnected" when nothing is wrong.**
It dropped its BLE link - that chirp comes from a real Zephyr disconnect
callback, not from a guess. Check the half's battery and its distance from the
dongle. (An older build inferred disconnects from silence and did chirp at
sleeping halves; if you see that, you are on a build before `split_conn.c`.)

**No chirp when a half comes back.**
The first transition per half is silent on purpose, so the halves connecting
at boot do not talk over the splash fanfare. The one after it will sound.

## Split

**BLE will not advertise or connect at all.**
First suspect is the SPI instance. NEXUS briefly drove the panel from SPIM3
for a 4x throughput win; BLE broke on that build and the panel is back on
SPIM0, which is what snake-module uses and what is known to work on this
board. If you re-enable spi3 in `nexus_dongle.overlay`, re-test pairing.

If BLE is still dead on SPIM0, it is not the display bus. Check next:
`&bt BT_CLR` (a profile bonded to a device you no longer use never advertises
as discoverable), then whether `CONFIG_ZMK_STUDIO` and its USB snippet have
squeezed the BLE stack for heap - `CONFIG_HEAP_MEM_POOL_SIZE` in
`central_dongle.conf` is the knob.

**The connect chirp never fires but the disconnect one does.**
Fixed. A BLE peripheral does not have to reappear under the same address -
privacy rotates it - and the first slot allocator only matched on the recorded
address, then took a free slot, then gave up. After one reconnect both slots
were claimed by stale addresses, so it returned -1 and dropped the cue. An
unknown address now reclaims a slot whose link is down.

**Two or three beeps a few seconds after every restart, and the splash tune
only played the first time.**
One cause. Bringing up a split keyboard produces several link transitions
while the central does discovery; announcing them beeped, and because the
sound engine plays the newest effect and drops what was playing, those chirps
cut off the splash fanfare. `CONFIG_NEXUS_SOUND_SETTLE_MS` (8 s) ignores
link changes until the links have settled.


**Switching BT profile does not change the number on screen.**
Fixed twice over. `refresh_endpoint()` compared only the endpoint and the link
state before marking the dashboard dirty, so `&bt BT_SEL n` updated the model
and returned without repainting; and the profile fields were read only while
BLE was the *selected* endpoint, so on USB they were frozen. Both are now
handled - the profile is read on every refresh and is part of the change
check.

**`BT_SEL 4` does nothing.**
Count your profiles. On a dongle there are `CONFIG_BT_MAX_PAIRED` minus
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS` of them - `7 - 2 = 5` here, so
`BT_SEL 0..4` is exactly the valid range and anything higher selects nothing.

**The BLE tile shows a cross while I am happily typing over USB.**
Fixed. The tile reports the BLE profile's own state now; being on USB is shown
by the USB symbol being the lit one, not by declaring BLE broken.

**Halves will not pair.**
Flash `settings_reset` to all three boards, one at a time, then reflash. This
fixes the great majority of split problems and is the first thing to try, not
the last.

**One half connects, the other does not.**
Confirm the non-working half is built as a peripheral. With a dongle as
central, *both* halves are peripherals -- a left half still built with
`CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y` will fight the dongle and lose.

## Studio

**Studio cannot see the device.**
The `studio-rpc-usb-uart` snippet is missing from `build.yaml`. Without it
`CONFIG_ZMK_STUDIO=y` builds fine but exposes no USB transport.

**Connects but will not save.**
Locked. Press your `&studio_unlock` key.

**Build fails with "no driver for behavior studio_unlock".**
`CONFIG_ZMK_STUDIO=n` but the keymap references `&studio_unlock`. Enable Studio
or remove the binding.

## Build

**`attempt to assign the value ... to the undefined symbol NEXUS_...`**
followed by `error: Aborting due to Kconfig warnings`.

A `.conf` in your config repo sets a symbol this module no longer declares -
usually one that was renamed or removed by a module update. Zephyr treats it
as fatal, and the real cause sits a hundred lines above the CMake error, so it
is worth checking directly:

```
python scripts/check_config.py ../your-config-repo/config
```

It prints the offending file and line and names the nearest symbol that does
exist. Delete the line, or rename it, and the build proceeds.

Note that this is *not* the same as the other Kconfig warnings a ZMK build
prints - `NICE_VIEW_GEM_ANIMATION`, `ZMK_KSCAN_DEBOUNCE_*`, `EC11`,
`Deprecated symbol KSCAN` and friends come from shields and modules that are
present but not selected. They are noisy and harmless; an undefined *symbol*
is the one that stops the build.


**`CONFIG_NEXUS_SPLASH_IMAGE points at a missing file`.**
The path is relative to your `config/` directory, not the repo root, and not
the shield directory.

**`png2c: interlaced PNGs are not supported`.**
Re-save without interlacing / "progressive".

**`NEXUS splash artwork is RGB565; set CONFIG_LV_COLOR_DEPTH_16=y`.**
Exactly what it says. NEXUS assumes a 16-bit colour depth throughout.

**Flash or RAM overflow.**
Cut in the order given in [zmk-studio.md](zmk-studio.md#memory). Splash artwork
first -- it is usually the single biggest thing you added.

**Peripheral (half) build pulls in the display stack.**
`CONFIG_NEXUS` leaked into the half's config. It should only be set by the
dongle shield; `CMakeLists.txt` returns early and compiles nothing without it
(Section 81). Check the half's `.conf` and any shared `nexus.conf` you included
into it by mistake.

## Games

**Game Center is empty.**
`CONFIG_NEXUS_TETRIS=n`, or `CONFIG_NEXUS_GAMES` never got selected.

**High score resets on reboot.**
`CONFIG_NEXUS_GAME_HIGHSCORE_PERSIST=y` and `CONFIG_SETTINGS=y`. The runtime
score works regardless; only persistence needs settings.

**Tetris is unplayable with one button.**
It is meant to be. Movement comes from the keyboard -- see
[games.md](games.md#tetris-controls).

## When the UI has crashed

Press the hardware reset button. It is wired straight to `RST` and has no
software path at all, which is precisely so it works when everything else does
not (Section 14). Double-tap it for the UF2 bootloader.
