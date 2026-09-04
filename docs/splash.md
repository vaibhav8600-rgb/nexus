# Splash screen

Changing the boot artwork must never mean editing NEXUS source. It doesn't.

## How it works

```
your-config/nexus/splash/splash.png
            │
            │  build time, scripts/png2c.py
            ▼
   nexus_splash_user.c   (uint16_t[] + struct nexus_splash_art)
            │
            │  link time
            ▼
   src/ui/splash.c reads nexus_splash_art  ->  gfx_blit565()
```

`splash.c` never learns whether it got your bitmap or the built-in default --
both define the same symbol, `nexus_splash_art`, carrying a width, a height and
a draw function. So the priority rule in Section 21 is settled by the linker,
with no runtime branch and no `#ifdef` in the screen.

## Using your own image

1. Put a PNG anywhere under your `config/` directory:

   ```
   config/nexus/splash/splash.png
   ```

2. Point at it, relative to `config/`:

   ```
   CONFIG_NEXUS_SPLASH_IMAGE="nexus/splash/splash.png"
   ```

3. Build. The converter runs automatically and prints what it cost:

   ```
   png2c: .../splash.png -> 160x160, 51200 bytes of flash
   ```

## What the converter accepts

8-bit PNG, any of: grayscale, grayscale+alpha, RGB, RGBA, palette (with
`tRNS`). All five scanline filters. **Interlaced PNGs are rejected** rather
than silently mangled -- re-save without "progressive"/Adam7.

The converter is stdlib-only Python. It does not need Pillow, because ZMK's
build container does not have Pillow and adding a pip dependency to a firmware
build to read one PNG is a bad trade.

### Size and flash

`CONFIG_NEXUS_SPLASH_MAX_DIM` (default 160) caps the larger side; anything
bigger is box-downscaled by an integer factor.

| Size | Flash |
| --- | --- |
| 96x96 | 18 KB |
| 128x128 | 32 KB |
| 160x160 | 51 KB |
| 240x240 | 115 KB |

An nRF52840 has 1 MB of flash and ZMK with Studio uses roughly half of it, so
240x240 fits -- but check the size report in CI before assuming it does on
your build.

### Transparency

There is no alpha channel in the on-device format, so transparent pixels are
flattened onto black (or `--bg` if you run the script by hand). Design for the
theme's background.

## Text

Three lines, all configuration:

```
CONFIG_NEXUS_BRAND="VAIBHAV TECH"      # small, above
CONFIG_NEXUS_PRODUCT="NEXUS"           # the wordmark
CONFIG_NEXUS_SUBTITLE="SMART ZMK DONGLE"
```

Set `BRAND` or `SUBTITLE` to `""` to hide that line. `PRODUCT` is drawn as a
five-layer extruded wordmark in the active theme's colours -- keep it short.

## Timing

```
CONFIG_NEXUS_SPLASH_DURATION_MS=3500
```

`0` disables the splash and boots straight to the default screen. A press of
the action button skips it early.

The splash is cosmetic. ZMK boots behind it; the keyboard is usable as soon as
the radio is up, not when the timer expires (Section 18).

## The default

No `CONFIG_NEXUS_SPLASH_IMAGE`? `assets/splash_default.c` draws a mark with
compositor primitives -- a ring, two crossed bars and a node. It costs about
200 bytes instead of 51 KB, which is why it is drawn and not a bitmap. It also
follows the active theme, which a bitmap cannot.

Both paths define the same symbol, `struct nexus_splash_art nexus_splash_art`,
so exactly one is linked and `src/ui/splash.c` never learns which it got.

## Running the converter by hand

```
python scripts/png2c.py --max-size 160 splash.png splash.c
```

There is no byte-order flag. The generated array is native-endian `uint16_t`
and the compositor swaps once per band on flush, so the one thing that used to
be easy to get wrong by hand no longer exists.
