# Splash screen

Changing the boot artwork must never mean editing NEXUS source. It doesn't.

## How it works

Three sources, in priority order. `CMakeLists.txt` picks the first that is
set and compiles **exactly one** of them:

```
  1. CONFIG_NEXUS_SPLASH_IMAGE          your PNG, from your config repo
  2. CONFIG_NEXUS_SPLASH_DEFAULT_IMAGE  a PNG inside the module
  3. (neither set)                      assets/splash_default.c, drawn
            │
            │  1 and 2: build time, scripts/png2c.py
            ▼
   nexus_splash_user.c   (uint16_t[] + struct nexus_splash_art)
            │
            │  link time
            ▼
   src/ui/splash.c reads nexus_splash_art  ->  gfx_blit565()
```

**As shipped, only the third applies**: both string options are empty and the
module carries no PNG, so the drawn badge is what you boot into.

`splash.c` never learns which one it got -- all three define the same symbol,
`nexus_splash_art`, carrying a width, a height and a draw function. So the
priority rule in Section 21 is settled by the linker, with no runtime branch
and no `#ifdef` in the screen.

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
CONFIG_NEXUS_PRODUCT="NEXUS"           # the wordmark
CONFIG_NEXUS_SUBTITLE="SMART ZMK DONGLE" # what it is, under the wordmark
CONFIG_NEXUS_BRAND="VAIBHAV TECH"      # who made it, smallest, at the foot
```

Set `BRAND` or `SUBTITLE` to `""` to hide that line. `PRODUCT` is drawn as
glass in the active theme's colours -- a near-white face with a soft accent
halo behind it and a one-pixel bevel, the same treatment the cards get. The
subtitle sits under it at body size, and the brand sits below a hairline --
the smallest and quietest text on the screen.

That order is deliberate: what the device *is* comes before who made it. The
brand used to be twice the subtitle's size and the brightest text on the
screen, 164px wide under a 108px wordmark, so it was the second thing you read
and it argued with the first. A subtitle too long for body size (more than 18
characters) steps down to caption size rather than running off the panel --
the same rule the wordmark follows.

These strings apply to the **drawn badge only**. A supplied PNG replaces the
function that draws them; see "Your image is the whole splash" below.

## Timing

```
CONFIG_NEXUS_SPLASH_DURATION_MS=3500   # the Kconfig default is 3000
```

`0` disables the splash and boots straight to the default screen. A press of
the action button skips it early.

The splash is cosmetic. ZMK boots behind it; the keyboard is usable as soon as
the radio is up, not when the timer expires (Section 18).

## The default badge

`assets/splash_default.c` draws the whole 240x240 composition with compositor
primitives -- no bitmap, no font conversion, no widget tree:

```
   two large discs hung off the corners, clipped by the panel
   a soft accent halo, a dark disc with an accent ring,
     an inner disc with a white hairline
   the N, centred in it
   NEXUS            the glass wordmark
   SMART ZMK DONGLE at body size, bright
   ----             a hairline
   VAIBHAV TECH     tracked, caption size, muted
```

It costs a few hundred bytes of code against 115,200 for the same picture as a
240x240 PNG -- about 14% of the 792 KB app partition -- which is the entire
argument for drawing it. It also follows the active theme, which a bitmap
cannot.

The module deliberately ships **no** default PNG. `NEXUS_SPLASH_DEFAULT_IMAGE`
exists for someone shipping NEXUS pre-branded with artwork the drawing cannot
express; pointing it at a full-screen image costs that 14% on every build,
whether or not anyone waits around to look at the splash.

`tests/splash/test_badge_layout.py` asserts the stacking order and that the
default stays empty, because the failure mode of getting that wrong is a
silent 115 KB, not a broken build.

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

### Your image is the whole splash, structurally

There is no setting for this and there does not need to be one.

`src/ui/splash.c` clears the panel, centres `nexus_splash_art` and calls its
`draw()`. That is the entire function - it draws no text of its own. The
badge's brand, wordmark and subtitle live inside
`assets/splash_default.c`'s draw function, because they belong to that
composition and to nothing else.

`CMakeLists.txt` compiles **exactly one** of `assets/splash_default.c` or a
PNG converted by `scripts/png2c.py`, both providing the same
`nexus_splash_art` symbol. So supplying an image does not disable the badge's
text; it replaces the function that drew it. The default splash is not merely
hidden, it is not in the binary -- and neither are `NEXUS_BRAND`,
`NEXUS_PRODUCT` or `NEXUS_SUBTITLE`, which is why setting them has no effect
on a custom splash.

An earlier version had a `CONFIG_NEXUS_SPLASH_TEXT` switch for this. A switch
can be set wrong, and "nothing is painted over a custom splash" is better as a
property of the code than as a promise in a config file.

### Photographs

A photograph will band. RGB565 has 5 bits of red and blue and 6 of green, so
smooth gradients - skies, studio backdrops, skin - quantise visibly, and there
is no dithering step here. Artwork with flat colour and hard edges survives the
format; a photo is worth previewing on the panel before committing to it.
