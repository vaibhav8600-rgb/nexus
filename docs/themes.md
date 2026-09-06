# Themes

```
CONFIG_NEXUS_THEME="NEXUS"
```

Seven palettes ship. An unknown name falls back to `NEXUS` rather than failing
a build over a typo. The Settings screen cycles through them at runtime; the
build-time value is what you get on boot.

Open [`theme-preview.html`](theme-preview.html) in a browser to see them.

| Name | |
| --- | --- |
| `NEXUS` | Aurora glass. Near-black ground, magenta and cyan corner glows, mint accent. The default and the one the reference design was drawn in. |
| `AMOLED` | Same language on true black with the glows removed. An OLED panel burns no power on `#000000`. |
| `DAYLIGHT` | Frosted light theme. White cards at 62% over a pale blue ground. |
| `CLAY` | Neumorphic. Cards are the same colour as the ground and read only through their edges. |
| `ESPRESSO` | Warm browns, amber accent. |
| `MINT` | Monochrome green on near-black. |
| `SUNSET` | The only gradient ground: deep violet falling to warm orange. |

## What a theme controls

Every colour in NEXUS comes from `struct nexus_theme`. No file outside
`src/ui/theme.c` contains a hex colour except Tetris's seven piece hues, which
are gameplay data rather than styling.

Colours are `gfx_color` (RGB565). Write them as RGB888 literals through the
`NEXUS_C()` macro, which packs at compile time.

| Token | Used for |
| --- | --- |
| `bg_top`, `bg_bot` | The ground gradient. Keep them different: the tint over a gradient is what makes the glass correct rather than approximate. |
| `glow_a`, `glow_b` + `glow_alpha` | The two soft corner blobs. `glow_alpha = 0` removes them. |
| `panel` + `panel_alpha` | Card tint. |
| `border` + `border_alpha` | Card hairline and inactive pill outlines. |
| `edge_hi` + `edge_hi_alpha` | Specular top edge, and the light pooling below it. |
| `edge_lo` + `edge_lo_alpha` | Shaded bottom edge, for thickness. |
| `caption` | Small uppercase labels. |
| `value` | Big numerals and primary text. |
| `accent` | WPM, battery meters, selection outlines. |
| `accent_alt` | Active modifier pills. |
| `track` | Battery trough, Tetris well background. |
| `muted` | Inactive glyphs and unknown values. |
| `success` / `warning` / `error` | Battery bands and lock indicators. |
| `wordmark[5]` | The title, brightest to darkest: `[0]`/`[1]` shade the face, `[2]` is the halo behind it, `[3]`/`[4]` the two graded pixels of shade under every edge. |
| `radius` | Card corner radius. |

## Why it looks like glass without blur

Section 57 permits faking it. NEXUS does not have to.

Blurring a linear gradient returns the same linear gradient. The NEXUS ground
*is* a linear vertical gradient, so an alpha tint drawn over it is not an
imitation of frosted glass -- it is arithmetically the result a real backdrop
blur would produce. No blur pass, no full-frame read-modify-write, no second
buffer.

What each pane is made of:

- **The tint.** `panel` at `panel_alpha`, blended per pixel into the live band.
- **Light pooling.** Four two-pixel steps at falling alpha down from the top
  edge, so the surface reads as curved rather than flat.
- **A specular top edge** (`edge_hi`) and a **shaded bottom edge** (`edge_lo`).
  This pair is the single detail that reads as "lit glass"; on the neumorphic
  CLAY theme it is the *only* thing separating a card from the ground.
- **Corner glows.** Three concentric discs per corner at low alpha, centred
  just off-screen so only the bright inner part lands. Set `glow_alpha = 0` to
  drop them entirely, as AMOLED and CLAY do.

Total extra RAM: none. It is all composited into the 5,760-byte band that was
going to be pushed anyway.

## Adding your own

`src/ui/theme.c`, one entry in `themes[]`. Copy the closest existing palette and
change the tokens. That is the only file to touch, and every screen picks it up.

If you want a theme without editing NEXUS source, say so on the issue tracker --
a `CONFIG_NEXUS_THEME_CUSTOM_*` surface is straightforward, it just was not
worth building speculatively (Section 78 allows build-time only for v1).
