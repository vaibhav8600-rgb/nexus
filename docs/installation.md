# Installation

NEXUS is a ZMK module. You add it to your existing `zmk-config`; you do not
fork it, vendor it, or paste it into a keymap.

## 1. Add the module

`config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: nexus
      url-base: https://github.com/vaibhav8600-rgb
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: nexus
      remote: nexus
      revision: v1.0.0
  self:
    path: config
```

Pin `revision` to a tag. NEXUS talks to ZMK APIs that move between releases;
`docs/development.md` records what this version was verified against.

## 2. Add the shield to your build

`build.yaml` -- two shields, space separated. `nexus_dongle` is the hardware,
your own dongle shield is the keys:

```yaml
include:
  - board: nice_nano_v2
    shield: nexus_dongle sofle_dongle
    snippet: studio-rpc-usb-uart
    artifact-name: nexus_dongle
```

Never wired a keyboard to it yet? Use the shipped demo instead -- it needs
nothing else and boots straight into the UI:

```yaml
  - board: nice_nano_v2
    shield: nexus_dongle nexus_dongle_demo
    snippet: studio-rpc-usb-uart
    artifact-name: nexus_standalone
```

## 3. Configure it

Copy `examples/nexus-config/config/nexus.conf` into your config and append it
to your dongle shield's `.conf`, or keep it separate and reference it. Every
option is documented in [configuration.md](configuration.md).

The defaults are already sensible: display on, home screen, Tetris on, sound
on, splash on, debug off.

## 4. Your keyboard halves

With a dongle as central, both halves are peripherals:

- The right half already is one. Nothing changes.
- The left half usually is not. If your `sofle_left` shield sets
  `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`, make a `sofle_left_peripheral` variant
  that sets it to `n`.

Neither half compiles any NEXUS code. `CONFIG_NEXUS` is off unless the dongle
shield turns it on, and `CMakeLists.txt` returns immediately when it is off, so
the halves pay nothing in flash or RAM (Section 81).

## 5. Build and flash

GitHub Actions is the normal path -- push and download the artifacts. For a
local build see [development.md](development.md).

Flash order does not matter, but the first time:

1. Flash `settings_reset` to all three boards, one at a time, to clear stale
   pairings. Let each one finish and reboot.
2. Flash `nexus_dongle`, then `sofle_left`, then `sofle_right`.
3. Power all three. The halves find the dongle within a few seconds; the
   dashboard battery cards switch from `--` to real numbers when they do.

To enter the bootloader, double-tap the reset button.

## 6. Pair with your computer

The dongle is the only thing your computer sees. Pair it over Bluetooth, or
just leave it plugged in over USB -- the dashboard shows which endpoint is
active.

## Verifying the install

Work through [the hardware test procedure](development.md#hardware-test-procedure).
It is twelve steps and catches every wiring mistake this design can have.
