/*
 * NEXUS strip compositor (Sections 7-9, 57, 72, 100).
 *
 * The panel takes opaque pixels, so the "glass" look has to be composited
 * somewhere we own. A full 240x240 RGB565 framebuffer is 115,200 B, which does
 * not fit next to ZMK, BLE and Studio on an nRF52840. Instead we composite one
 * 240 x GFX_STRIP_H band at a time (5,760 B), push it, and move down.
 *
 * A shorter band costs nothing extra per full repaint - the same pixels go
 * down the bus - but it halves the buffer and makes partial repaints finer,
 * so both axes improve together.
 *
 * gfx_render() calls your draw function once per band. Draw the whole scene
 * every time: every op clips itself to the live band, so the ones off-band
 * cost a compare and return.
 *
 * Nothing here names ST7789, a GPIO or a SPI bus - it talks to whatever
 * `zephyr,display` points at (Requirement D, Section 72).
 */
#ifndef NEXUS_GFX_H_
#define NEXUS_GFX_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_W 240
#define GFX_H 240
/* 240 % 12 == 0, so bands tile the panel exactly. 5,760 B of static RAM. */
#define GFX_STRIP_H 12

typedef uint16_t gfx_color;

/** Alpha is 0-255; 255 is opaque. */
#define GFX_OPAQUE 255u

static inline gfx_color gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return (gfx_color)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static inline gfx_color gfx_hex(uint32_t rgb888)
{
	return gfx_rgb((uint8_t)(rgb888 >> 16), (uint8_t)(rgb888 >> 8),
		       (uint8_t)rgb888);
}

/** Blend @p b over @p a by @p alpha. Used for theme-derived tints. */
gfx_color gfx_mix(gfx_color a, gfx_color b, uint8_t alpha);

/* ---- lifecycle --------------------------------------------------------- */

/** @return 0 when the panel is usable. Non-zero means the UI stays dark and
 *          ZMK carries on regardless (Section 87). */
int gfx_init(void);
bool gfx_ready(void);

typedef void (*gfx_draw_fn)(void *ctx);

/** Repaint the whole panel. */
void gfx_render(gfx_draw_fn draw, void *ctx);

/**
 * Repaint only the bands touching [y0, y1). @p draw is still called with the
 * full scene - ops clip themselves - but bands outside the range are never
 * composited or pushed. One status row changing costs 2 bands over SPI instead
 * of 20 (Section 61).
 */
void gfx_render_range(gfx_draw_fn draw, void *ctx, int y0, int y1);

/** Band currently being composited, so a caller can cull whole subtrees. */
int gfx_band_y0(void);
int gfx_band_y1(void); /* exclusive */

/** True when [y, y+h) overlaps the live band. */
static inline bool gfx_hits(int y, int h)
{
	return !(y + h <= gfx_band_y0() || y >= gfx_band_y1());
}

/* ---- primitives (all clip to the live band) ---------------------------- */
void gfx_clear(gfx_color c);
void gfx_vgrad(int y0, int y1, gfx_color top, gfx_color bot);
void gfx_rect(int x, int y, int w, int h, gfx_color c, uint8_t a);
void gfx_hline(int x, int y, int w, gfx_color c, uint8_t a);
void gfx_vline(int x, int y, int h, gfx_color c, uint8_t a);

/** Anti-aliased rounded rectangle. r == 0 gives a plain rect. */
void gfx_round_rect(int x, int y, int w, int h, int r, gfx_color c, uint8_t a);

/** 1px rounded outline. */
void gfx_round_frame(int x, int y, int w, int h, int r, gfx_color c, uint8_t a);

/**
 * Filled circle, one integer square root per row rather than per pixel.
 *
 * gfx_round_rect() anti-aliases its corners, which is right for a 7px card
 * radius and ruinous for a 90px glow: there the "corners" are the entire
 * shape, so every pixel pays a sqrt. Use this for large soft shapes - on a
 * low-alpha blob the missing edge AA is invisible, and a full repaint drops
 * from ~80,000 square roots to a couple of hundred.
 */
void gfx_disc(int cx, int cy, int r, gfx_color c, uint8_t a);

/**
 * Blit a raw RGB565 image (native endian, row-major) with no scaling.
 * Used only by the splash; everything else is drawn.
 */
void gfx_blit565(int x, int y, int w, int h, const uint16_t *px);

/* ---- text (5x7 packed bitmap font, integer scaled) --------------------- */
int gfx_text_w(const char *s, int scale); /* pixels, no trailing gap */
int gfx_text_h(int scale);
void gfx_text(int x, int y, const char *s, int scale, gfx_color c, uint8_t a);
/** Centred on @p cx. */
void gfx_text_c(int cx, int y, const char *s, int scale, gfx_color c, uint8_t a);

/* ---- icons ------------------------------------------------------------- */

/**
 * Draw an arbitrary 1-bit bitmap, row-major, bit N of a row = column N.
 *
 * The 5x7 icon table above is sized for things that sit inline with text. A
 * modifier symbol is not one of those: at 5x7 the caret, the option stroke and
 * the four panes are all a smudge, and no amount of scaling fixes a shape that
 * was never drawn. This takes the real 11x11 forms instead.
 *
 * @p w may be up to 16. Rows outside the current band are skipped, so calling
 * it once per band costs nothing for the bands it does not touch.
 */
void gfx_glyph(int x, int y, const uint16_t *rows, int w, int h, int scale,
	       gfx_color c, uint8_t a);

/**
 * Draw text in the 10x14 display face (src/ui/font10x14.h).
 *
 * A real display face rather than the 5x7 body font scaled up: two-pixel
 * stems and flat terminals, so the wordmark has weight instead of looking
 * like magnified pixel art. Uppercase and digits only (ASCII 32..90) -
 * anything outside that range is drawn from the 5x7 font at the same nominal
 * height, so a product name with lowercase in it still renders.
 */
/**
 * One glyph with a vertical gradient, mixed per source row.
 *
 * gfx_glyph() is this with both stops the same colour.
 */
void gfx_glyph_grad(int x, int y, const uint16_t *rows, int w, int h,
		    int scale, gfx_color top, gfx_color bot, uint8_t a);

/**
 * The display face as glass: a soft two-step @p glow behind the letter, a
 * one-pixel @p hi rim above every edge, two graded pixels of @p lo_near then
 * @p lo_far below it, and a @p top to @p bot gradient across the face.
 *
 * Advances exactly as gfx_face_text() does, so gfx_face_w() still measures the
 * letters - but the glow reaches two letter-pixels past them on every side,
 * which the caller must leave room for.
 */
/**
 * The display face as glass: graded fill, a one-pixel bevel, halo behind.
 *
 * @p glow_a of 0 does not mean "no glow" - it means outline: @p glow is drawn
 * opaque one pixel around the letterform instead of as a halo. A light theme
 * needs that edge definition without the blur a halo becomes on it.
 */
void gfx_face_text_glass(int x, int y, const char *s, int scale, gfx_color top,
			 gfx_color bot, gfx_color hi, gfx_color lo_near,
			 gfx_color lo_far, gfx_color glow, uint8_t glow_a);

void gfx_face_text(int x, int y, const char *s, int scale, gfx_color c,
		   uint8_t a);
/** Width of @p s in the display face, matching gfx_face_text(). */
int gfx_face_w(const char *s, int scale);
/** Height of one line of the display face. */
int gfx_face_h(int scale);

/**
 * Unsigned to decimal, without printf.
 *
 * picolibc's "%u" drags in the double-capable formatter, which can burn 1-2 KB
 * of stack per call - on a display thread whose whole stack is
 * CONFIG_ZMK_DISPLAY_DEDICATED_THREAD_STACK_SIZE. Overflowing it panics the
 * kernel and takes the keyboard down with it. This needs 12 bytes.
 *
 * @param pad minimum digits, zero-filled ("072"); 0 means no padding.
 */
const char *gfx_utoa(uint32_t v, char *buf, int len, int pad);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_GFX_H_ */
