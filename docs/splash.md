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
weighted wordmark over an accent rule, in the active theme's colours. Brand
and subtitle are drawn at body size, and the whole block is measured before
it is drawn so it stays centred whatever your strings and artwork come to.

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

## The default badge

The built-in artwork is the badge composition: two large circles hung off
opposite corners and clipped by the panel, a halo, a ringed disc with an inner
hairline, an angular N, and three lines of type at fixed positions (132 / 152 /
192).

It is a **fixed** layout, not a centred flow. The lines hang off the disc at
measured offsets, so re-centring the block whenever a string length changed
would slide the type against the artwork - which is the one thing the design
does not tolerate.

Everything is a solid fill. There is no gradient in the composition, so there
is no RGB565 banding to dither away.

### Why it is not LVGL

The design arrived as an LVGL widget tree. It is drawn with compositor
primitives instead, for three reasons that are worth stating because they will
come up again for any artwork someone wants to drop in:

- **RAM.** NEXUS removed its LVGL UI to reclaim it. `CONFIG_LV_Z_VDB_SIZE` is
  pinned at 10 and LVGL holds one empty object. Ten live objects with their own
  styles is the thing that was taken out, not something to add back.
- **Fonts.** The LVGL version needs three `lv_font_conv` subsets - about 2 KB
  of flash plus an external toolchain step in the build. The 10x14 display face
  added for the wordmark already draws these glyphs at these sizes.
- **Lifecycle.** It called the status screen directly and owned its own
  teardown timer. NEXUS's screen stack already sequences that transition, and
  two things driving the same transition is how you get a splash that never
  leaves.

Same picture, no widget tree, no font conversion, no second lifecycle.

## Using your own image

Drop a PNG in your config directory and point at it. The build converts it -
no C arrays to generate, no NEXUS source to edit.

```
# config/nexus_dongle.conf
CONFIG_NEXUS_SPLASH_IMAGE="nexus/splash/splash.png"
CONFIG_NEXUS_SPLASH_MAX_DIM=240
```

The path is relative to your zmk-config's `config/` directory. A missing file
is a `FATAL_ERROR` at configure time, not a silent fallback to the default.

**PNG, not JPEG.** `scripts/png2c.py` is stdlib-only on purpose - Pillow is not
in ZMK's build container, and adding a pip dependency to a firmware build to
read one image is a bad trade. It reads 8-bit grayscale, grayscale+alpha, RGB,
RGBA and palette PNGs with all five scanline filters. Interlaced PNGs are
rejected rather than mangled. Export or convert to PNG first; any image editor
will do it, and so will `magick photo.jpg splash.png`.

### What it costs

RGB565, so two bytes a pixel, uncompressed, in flash:

| `MAX_DIM` | size | flash | share of the 792 KB app partition |
| --- | --- | --- | --- |
| 160 | 160x160 | 51,200 B | 6.3% |
| 200 | 200x200 | 80,000 B | 9.9% |
| 240 | 240x240 | 115,200 B | 14.2% |

`MAX_DIM` is a ceiling on the longer side; the converter box-downscales to fit
and prints the final size and byte count during the build, so the number is
never a surprise. The built-in badge costs a few hundred bytes by comparison -
that is the whole reason it is drawn rather than shipped as a bitmap.

### Your image is the whole splash

Setting `NEXUS_SPLASH_IMAGE` turns `CONFIG_NEXUS_SPLASH_TEXT` off by default.

A supplied image is almost always a finished design with its own lettering, and
drawing `NEXUS_BRAND` and `NEXUS_PRODUCT` on top of that is not a splash
screen, it is two of them. With the text off the image is centred on a cleared
panel and nothing is drawn over it - an image smaller than 240x240 gets a clean
border instead of NEXUS's type crossing it.

Set `CONFIG_NEXUS_SPLASH_TEXT=y` if your image is a bare mark meant to sit
above the standard three lines.

### Photographs

A photograph will band. RGB565 has 5 bits of red and blue and 6 of green, so
smooth gradients - skies, studio backdrops, skin - quantise visibly, and there
is no dithering step here. Artwork with flat colour and hard edges survives the
format; a photo is worth previewing on the panel before committing to it.
