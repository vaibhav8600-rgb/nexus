# Splash artwork

Drop a `splash.png` here and point `CONFIG_NEXUS_SPLASH_IMAGE` at it. The build
converts it to RGB565 automatically -- you never generate a C array by hand.

- **Format:** 8-bit PNG. Grayscale, RGB, RGBA and palette all work.
  Interlaced ("progressive") PNGs are rejected; re-save without interlacing.
- **Size:** anything. Larger images are box-downscaled to
  `CONFIG_NEXUS_SPLASH_MAX_DIM` (default 160 px) so the flash cost stays sane.
  A 160x160 image is 51 KB of firmware; 240x240 is 115 KB.
- **Transparency:** flattened onto black. The splash sits on the theme
  background, so design for a dark ground unless you use a light theme.

No file here? NEXUS draws its built-in mark instead, which costs nothing.
