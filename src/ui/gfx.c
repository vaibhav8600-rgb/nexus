/*
 * Strip compositor. See include/nexus/gfx.h for the why.
 *
 * SPDX-License-Identifier: MIT
 */

#include <nexus/gfx.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "font5x7.h"
#include "font10x14.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* 240 * 12 * 2 = 5,760 B. Static, so a failed allocation can never take the
 * dongle down at boot the way an unchecked k_malloc would (Section 9). */
static uint16_t band[GFX_W * GFX_STRIP_H];
static const struct device *disp;
static int band_y0;
static bool ready;

int gfx_band_y0(void) { return band_y0; }
int gfx_band_y1(void) { return band_y0 + GFX_STRIP_H; }
bool gfx_ready(void) { return ready; }

int gfx_init(void)
{
	disp = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		LOG_ERR("no display; NEXUS UI stays dark, ZMK is unaffected");
		ready = false;
		return -ENODEV;
	}

	ready = true;
	return 0;
}

/* ---- blending ---------------------------------------------------------- */

/* x/255 without a divide: (t + (t>>8) + 1) >> 8. Off by at most 1/255, which
 * is invisible in RGB565, and roughly 3x cheaper - this runs per pixel per
 * band, so the divides cost real milliseconds per repaint. */
static inline uint32_t div255(uint32_t t)
{
	return (t + (t >> 8) + 1u) >> 8;
}

static inline uint16_t mix(uint16_t d, uint16_t s, uint8_t a)
{
	if (a == 0) {
		return d;
	}
	if (a >= 255) {
		return s;
	}

	uint32_t ia = 255u - a;
	uint32_t r = div255((((s >> 11) & 0x1Fu) * a) + (((d >> 11) & 0x1Fu) * ia));
	uint32_t g = div255((((s >> 5) & 0x3Fu) * a) + (((d >> 5) & 0x3Fu) * ia));
	uint32_t b = div255(((s & 0x1Fu) * a) + ((d & 0x1Fu) * ia));

	return (uint16_t)((r << 11) | (g << 5) | b);
}

gfx_color gfx_mix(gfx_color a, gfx_color b, uint8_t alpha)
{
	return mix(a, b, alpha);
}

static inline void px(int x, int y, gfx_color c, uint8_t a)
{
	if (x < 0 || x >= GFX_W) {
		return;
	}

	int by = y - band_y0;

	if (by < 0 || by >= GFX_STRIP_H) {
		return;
	}

	uint16_t *p = &band[by * GFX_W + x];

	*p = mix(*p, c, a);
}

/* ---- render pass ------------------------------------------------------- */
/*
 * Logical rotation lives here and nowhere else (Section 8). Drawing code is
 * always in logical coordinates with the origin top-left; only the flush knows
 * the panel may be turned. Rotating in the panel itself (the ST7789 `mdac`
 * byte in the shield overlay) is free - use this when you cannot.
 */
#define ROT CONFIG_NEXUS_DISPLAY_ROTATION

#if ROT == 90 || ROT == 270
/* 90 and 270 transpose the band into a different shape, so they need a
 * destination buffer. 0 and 180 do not: 180 maps index i to N-1-i, which is a
 * plain in-place reversal, so the commonest correction for an upside-down
 * panel costs no extra RAM at all. */
static uint16_t obuf[GFX_W * GFX_STRIP_H];
#endif

static void flush(void)
{
#if ROT == 90
	for (int ly = 0; ly < GFX_STRIP_H; ly++) {
		for (int lx = 0; lx < GFX_W; lx++) {
			obuf[lx * GFX_STRIP_H + (GFX_STRIP_H - 1 - ly)] =
				__builtin_bswap16(band[ly * GFX_W + lx]);
		}
	}

	struct display_buffer_descriptor d = {
		.buf_size = sizeof(obuf), .width = GFX_STRIP_H,
		.height = GFX_H, .pitch = GFX_STRIP_H,
	};

	display_write(disp, GFX_H - GFX_STRIP_H - band_y0, 0, &d, (uint8_t *)obuf);

#elif ROT == 180
	/*
	 * A half turn sends band index i to (STRIP_H-1-ly)*W + (W-1-lx), which
	 * is exactly N-1-i - so the whole band is just reversed. Doing it in
	 * place saves the 5,760-byte destination buffer that 90 and 270 need,
	 * and an upside-down panel is by far the most common thing anyone has
	 * to correct.
	 */
	for (int i = 0, j = GFX_W * GFX_STRIP_H - 1; i < j; i++, j--) {
		uint16_t a = __builtin_bswap16(band[i]);

		band[i] = __builtin_bswap16(band[j]);
		band[j] = a;
	}

	struct display_buffer_descriptor d = {
		.buf_size = sizeof(band), .width = GFX_W,
		.height = GFX_STRIP_H, .pitch = GFX_W,
	};

	display_write(disp, 0, GFX_H - GFX_STRIP_H - band_y0, &d, (uint8_t *)band);

#elif ROT == 270
	for (int ly = 0; ly < GFX_STRIP_H; ly++) {
		for (int lx = 0; lx < GFX_W; lx++) {
			obuf[(GFX_W - 1 - lx) * GFX_STRIP_H + ly] =
				__builtin_bswap16(band[ly * GFX_W + lx]);
		}
	}

	struct display_buffer_descriptor d = {
		.buf_size = sizeof(obuf), .width = GFX_STRIP_H,
		.height = GFX_H, .pitch = GFX_STRIP_H,
	};

	display_write(disp, band_y0, 0, &d, (uint8_t *)obuf);

#else /* 0 - no logical rotation */
	for (int i = 0; i < GFX_W * GFX_STRIP_H; i++) {
		band[i] = __builtin_bswap16(band[i]); /* panel reads high byte first */
	}

	struct display_buffer_descriptor d = {
		.buf_size = sizeof(band), .width = GFX_W,
		.height = GFX_STRIP_H, .pitch = GFX_W,
	};

	display_write(disp, 0, band_y0, &d, (uint8_t *)band);
#endif
}

void gfx_render_range(gfx_draw_fn draw, void *ctx, int y0, int y1)
{
	if (!ready || !draw) {
		return;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (y1 > GFX_H) {
		y1 = GFX_H;
	}
	if (y1 <= y0) {
		return;
	}

	/* Snap to band boundaries - a band is the smallest unit we can push. */
	int first = (y0 / GFX_STRIP_H) * GFX_STRIP_H;

	for (band_y0 = first; band_y0 < y1; band_y0 += GFX_STRIP_H) {
		draw(ctx);
		flush();
	}
}

void gfx_render(gfx_draw_fn draw, void *ctx)
{
	gfx_render_range(draw, ctx, 0, GFX_H);
}

/* ---- primitives -------------------------------------------------------- */

void gfx_clear(gfx_color c)
{
	for (int i = 0; i < GFX_W * GFX_STRIP_H; i++) {
		band[i] = c;
	}
}

void gfx_vgrad(int y0, int y1, gfx_color top, gfx_color bot)
{
	if (y1 <= y0) {
		return;
	}

	int span = (y1 - y0) > 1 ? (y1 - y0 - 1) : 1;
	int a = MAX(y0, band_y0);
	int b = MIN(y1, gfx_band_y1());

	for (int y = a; y < b; y++) {
		uint32_t t = (uint32_t)(y - y0) * 255u / (uint32_t)span;
		uint16_t c = mix(top, bot, (uint8_t)(t > 255u ? 255u : t));
		uint16_t *row = &band[(y - band_y0) * GFX_W];

		for (int x = 0; x < GFX_W; x++) {
			row[x] = c;
		}
	}
}

void gfx_rect(int x, int y, int w, int h, gfx_color c, uint8_t a)
{
	if (w <= 0 || h <= 0 || a == 0) {
		return;
	}

	int y_a = MAX(y, band_y0);
	int y_b = MIN(y + h, gfx_band_y1());
	int x_a = MAX(x, 0);
	int x_b = MIN(x + w, GFX_W);

	for (int yy = y_a; yy < y_b; yy++) {
		uint16_t *row = &band[(yy - band_y0) * GFX_W];

		if (a >= 255) {
			for (int xx = x_a; xx < x_b; xx++) {
				row[xx] = c;
			}
		} else {
			for (int xx = x_a; xx < x_b; xx++) {
				row[xx] = mix(row[xx], c, a);
			}
		}
	}
}

void gfx_hline(int x, int y, int w, gfx_color c, uint8_t a)
{
	gfx_rect(x, y, w, 1, c, a);
}

void gfx_vline(int x, int y, int h, gfx_color c, uint8_t a)
{
	gfx_rect(x, y, 1, h, c, a);
}

/* Integer sqrt, only ever called on corner pixels. */
static uint32_t isqrt32(uint32_t n)
{
	if (n == 0) {
		return 0;
	}

	uint32_t x = n;
	uint32_t y = (x + 1u) / 2u;

	while (y < x) {
		x = y;
		y = (x + n / x) / 2u;
	}
	return x;
}

/*
 * Coverage of a corner pixel, 0..255, from its distance to the arc centre.
 * Distance is carried in 1/16 px so the edge gets real anti-aliasing instead
 * of the stair-step you otherwise see at this panel size.
 */
static inline uint8_t corner_cov(int dx, int dy, int r)
{
	uint32_t d16 = isqrt32(((uint32_t)(dx * dx + dy * dy)) * 256u);
	int32_t cov = (int32_t)(r * 16) + 8 - (int32_t)d16;

	if (cov <= 0) {
		return 0;
	}
	if (cov >= 16) {
		return 255;
	}
	return (uint8_t)(cov * 255 / 16);
}

void gfx_round_rect(int x, int y, int w, int h, int r, gfx_color c, uint8_t a)
{
	if (w <= 0 || h <= 0 || a == 0) {
		return;
	}
	if (r <= 0) {
		gfx_rect(x, y, w, h, c, a);
		return;
	}
	if (r > w / 2) {
		r = w / 2;
	}
	if (r > h / 2) {
		r = h / 2;
	}

	/* Middle band: full width, no corners involved. */
	gfx_rect(x, y + r, w, h - 2 * r, c, a);

	int y_a = MAX(y, band_y0);
	int y_b = MIN(y + h, gfx_band_y1());

	for (int yy = y_a; yy < y_b; yy++) {
		int top = yy - (y + r);         /* < 0 above the top arc centre    */
		int bot = yy - (y + h - 1 - r); /* > 0 below the bottom arc centre */
		int dy;

		if (top < 0) {
			dy = -top;
		} else if (bot > 0) {
			dy = bot;
		} else {
			continue; /* covered by the middle rect */
		}

		for (int i = 0; i < r; i++) {
			int dx = r - i; /* distance from the arc centre */
			uint8_t cov = corner_cov(dx, dy, r);

			if (cov == 0) {
				continue;
			}

			uint8_t aa = (uint8_t)(((uint32_t)cov * a) / 255u);

			px(x + i, yy, c, aa);
			px(x + w - 1 - i, yy, c, aa);
		}

		/* The flat span between the two arcs on this row. */
		gfx_rect(x + r, yy, w - 2 * r, 1, c, a);
	}
}

void gfx_round_frame(int x, int y, int w, int h, int r, gfx_color c, uint8_t a)
{
	if (w <= 2 || h <= 2 || a == 0) {
		return;
	}

	gfx_hline(x + r, y, w - 2 * r, c, a);
	gfx_hline(x + r, y + h - 1, w - 2 * r, c, a);
	gfx_vline(x, y + r, h - 2 * r, c, a);
	gfx_vline(x + w - 1, y + r, h - 2 * r, c, a);

	if (r <= 0) {
		return;
	}

	/*
	 * Corners: the difference between full coverage and one-pixel-smaller
	 * coverage is exactly the arc, so the outline is anti-aliased by the
	 * same function the filled version uses, with no second geometry path.
	 */
	int y_a = MAX(y, band_y0);
	int y_b = MIN(y + h, gfx_band_y1());

	for (int yy = y_a; yy < y_b; yy++) {
		int top = yy - (y + r);
		int bot = yy - (y + h - 1 - r);
		int dy;

		if (top < 0) {
			dy = -top;
		} else if (bot > 0) {
			dy = bot;
		} else {
			continue;
		}

		for (int i = 0; i < r; i++) {
			int dx = r - i;
			int cov = (int)corner_cov(dx, dy, r) -
				  (int)corner_cov(dx, dy, r - 1);

			if (cov <= 0) {
				continue;
			}

			uint8_t aa = (uint8_t)(((uint32_t)cov * a) / 255u);

			px(x + i, yy, c, aa);
			px(x + w - 1 - i, yy, c, aa);
		}
	}
}

void gfx_disc(int cx, int cy, int r, gfx_color c, uint8_t a)
{
	if (r <= 0 || a == 0) {
		return;
	}

	int y_a = MAX(cy - r, band_y0);
	int y_b = MIN(cy + r + 1, gfx_band_y1());

	for (int y = y_a; y < y_b; y++) {
		int dy = y - cy;
		/* dy is bounded by the loop, so r*r - dy*dy is never negative. */
		int half = (int)isqrt32((uint32_t)(r * r - dy * dy));

		gfx_rect(cx - half, y, 2 * half + 1, 1, c, a);
	}
}

void gfx_blit565(int x, int y, int w, int h, const uint16_t *src)
{
	if (!src || w <= 0 || h <= 0) {
		return;
	}

	int y_a = MAX(y, band_y0);
	int y_b = MIN(y + h, gfx_band_y1());
	int x_a = MAX(x, 0);
	int x_b = MIN(x + w, GFX_W);

	for (int yy = y_a; yy < y_b; yy++) {
		const uint16_t *srow = &src[(size_t)(yy - y) * (size_t)w];
		uint16_t *drow = &band[(yy - band_y0) * GFX_W];

		for (int xx = x_a; xx < x_b; xx++) {
			drow[xx] = srow[xx - x];
		}
	}
}

/* ---- text -------------------------------------------------------------- */

int gfx_text_h(int scale)
{
	return FONT_H * scale;
}

int gfx_text_w(const char *s, int scale)
{
	int n = 0;

	for (const char *p = s; p && *p; p++) {
		n++;
	}
	if (n == 0) {
		return 0;
	}
	return n * (FONT_W + 1) * scale - scale; /* no gap after the last glyph */
}

static const uint8_t *glyph(char ch)
{
	if (ch >= 'a' && ch <= 'z') {
		ch = (char)(ch - 'a' + 'A');
	}
	if (ch < FONT_FIRST || ch > FONT_LAST) {
		ch = '?';
	}
	return &font5x7[(ch - FONT_FIRST) * FONT_W];
}

void gfx_text(int x, int y, const char *s, int scale, gfx_color c, uint8_t a)
{
	if (!s) {
		return;
	}
	if (scale < 1) {
		scale = 1;
	}

	/* Whole line off-band? Nothing to do. */
	if (y + FONT_H * scale <= band_y0 || y >= gfx_band_y1()) {
		return;
	}

	int pen = x;

	for (const char *p = s; *p; p++) {
		const uint8_t *g = glyph(*p);

		for (int col = 0; col < FONT_W; col++) {
			uint8_t bits = g[col];

			for (int row = 0; row < FONT_H; row++) {
				if (!(bits & (1u << row))) {
					continue;
				}
				gfx_rect(pen + col * scale, y + row * scale,
					 scale, scale, c, a);
			}
		}
		pen += (FONT_W + 1) * scale;
	}
}

void gfx_text_c(int cx, int y, const char *s, int scale, gfx_color c, uint8_t a)
{
	gfx_text(cx - gfx_text_w(s, scale) / 2, y, s, scale, c, a);
}

/* ---- icons ------------------------------------------------------------- */

/* Same column-major, bit-0-is-top layout as font5x7. */
/*
 * The 5x7 icon table used to live here: a Bluetooth rune, a USB monitor, a
 * padlock and a cursor. Every one of them has been replaced by something the
 * screen can actually carry - the transports by 9x15 glyphs on the link card,
 * the locks and the jiggler by corner dots on the brand plate - so the table,
 * gfx_icon() and gfx_icon_w() are gone with them.
 */



void gfx_glyph_grad(int x, int y, const uint16_t *rows, int w, int h,
		    int scale, gfx_color top, gfx_color bot, uint8_t a)
{
	if (rows == NULL || w <= 0 || h <= 0) {
		return;
	}
	if (scale < 1) {
		scale = 1;
	}
	if (y + h * scale <= band_y0 || y >= gfx_band_y1()) {
		return;
	}

	for (int row = 0; row < h; row++) {
		uint16_t bits = rows[row];

		if (bits == 0) {
			continue;
		}

		/*
		 * The ramp is per SOURCE row, so it is the letterform that
		 * shades rather than the screen: the same glyph gets the same
		 * gradient at every scale, and it does not shift when the
		 * wordmark straddles two compositor bands.
		 */
		gfx_color c = top;

		if (top != bot) {
			c = gfx_mix(top, bot,
				    (uint8_t)(h > 1 ? row * 255 / (h - 1) : 0));
		}

		/*
		 * Coalesce horizontal runs into one gfx_rect instead of one
		 * per pixel. These shapes are mostly solid bars - the Windows
		 * flag is two 5-wide runs per row - so this is roughly a 5x
		 * cut in blend calls for the same output.
		 */
		int col = 0;

		while (col < w) {
			if (!(bits & (1u << col))) {
				col++;
				continue;
			}

			int run = 0;

			while (col + run < w && (bits & (1u << (col + run)))) {
				run++;
			}
			gfx_rect(x + col * scale, y + row * scale, run * scale,
				 scale, c, a);
			col += run;
		}
	}
}

void gfx_glyph(int x, int y, const uint16_t *rows, int w, int h, int scale,
	       gfx_color c, uint8_t a)
{
	gfx_glyph_grad(x, y, rows, w, h, scale, c, c, a);
}

int gfx_face_h(int scale)
{
	return FACE_H * (scale < 1 ? 1 : scale);
}

int gfx_face_w(const char *s, int scale)
{
	int n = 0;

	for (const char *p = s; p && *p; p++) {
		n++;
	}
	if (scale < 1) {
		scale = 1;
	}
	/* One column of air between glyphs, none after the last. */
	return n ? n * (FACE_W + 1) * scale - scale : 0;
}

void gfx_face_text(int x, int y, const char *s, int scale, gfx_color c,
		   uint8_t a)
{
	if (scale < 1) {
		scale = 1;
	}
	if (y + FACE_H * scale <= band_y0 || y >= gfx_band_y1()) {
		return;
	}

	for (const char *p = s; p && *p; p++) {
		unsigned char ch = (unsigned char)*p;

		if (ch >= FACE_FIRST && ch <= FACE_LAST) {
			gfx_glyph(x, y, font10x14[ch - FACE_FIRST], FACE_W,
				  FACE_H, scale, c, a);
		} else {
			/*
			 * Outside the face. Drawn from the 5x7 font, scaled to
			 * the same cap height and centred in the cell, so a
			 * lowercase product name degrades to "one odd glyph"
			 * rather than to a hole in the word.
			 */
			char one[2] = { *p, 0 };
			int fs = (FACE_H * scale) / FONT_H;

			if (fs < 1) {
				fs = 1;
			}
			gfx_text(x + (FACE_W * scale - FONT_W * fs) / 2, y, one,
				 fs, c, a);
		}
		x += (FACE_W + 1) * scale;
	}
}

/*
 * A 3x3 dilation of one glyph, shifted one column right into a (w+2) field.
 *
 * This is how the outline is drawn: one extra pass over a fattened copy of the
 * bitmap, rather than four passes at +/-1 offsets. It is both cheaper and more
 * correct for this artwork - dilating at SOURCE resolution makes the outline
 * exactly one letter-pixel thick, so it grows with the wordmark instead of
 * staying a hairline, which is what the blocky arcade references actually do.
 *
 * Bit 0 is the leftmost column, so << moves right. The glyph sits at offset 1
 * and the caller draws at x - scale to put it back where it belongs.
 */
static void dilate_rows(const uint16_t *src, int n, uint16_t *dst)
{
	for (int r = 0; r < n + 2; r++) {
		uint16_t m = 0;

		/*
		 * Output row r holds source row r-1, so the three rows that
		 * dilate into it are r-2, r-1 and r. Growing the field by a
		 * row and a column on every side is not cosmetic: a letter
		 * whose stem reaches row 0 - N, E, U - would otherwise have
		 * nothing along its cap or its foot, and the glow would stop
		 * dead at the letter's widest point.
		 *
		 * Run twice, it gives a two-step falloff for a few dozen
		 * cycles, which is what a soft halo costs here. A real blur
		 * would need a scratch buffer the size of the band.
		 */
		for (int k = 0; k < 3; k++) {
			int sr = r - 2 + k;

			if (sr >= 0 && sr < n) {
				m |= src[sr];
			}
		}
		dst[r] = (uint16_t)(m | (m << 1) | (m << 2));
	}
}

/*
 * The wordmark as glass, not as arcade lettering.
 *
 * The previous version put the accent in the letter's FILL and ringed it with
 * a hard one-cell outline over a chunky extrusion. That is a sticker: it is
 * the one element on a screen of frosted panes that is not made of the same
 * material, and saturated pink on a blocky silhouette reads as a game rather
 * than as a product.
 *
 * Glass gets its presence from light, not from colour:
 *
 *   glow    the accent moves OFF the letter and behind it, two dilation
 *           steps of falloff at low alpha - a backlit sign, not a highlighter
 *   bevel   one pixel of specular above every edge and one of shade below,
 *           drawn before the face so only that pixel survives. This is the
 *           same trick nexus_draw_card() uses on a pane, which is why the
 *           title now looks like it is cut from the card it sits on
 *   face    near-white, shading down - the letter catches the light the
 *           panes catch
 *
 * The bevel is one PIXEL, not one cell, at every scale: a bevel that grows
 * with the type stops being a bevel and becomes an outline again.
 */
void gfx_face_text_glass(int x, int y, const char *s, int scale, gfx_color top,
			 gfx_color bot, gfx_color hi, gfx_color lo_near,
			 gfx_color lo_far, gfx_color glow, uint8_t glow_a)
{
	uint16_t halo[FACE_H + 2];
	uint16_t wide[FACE_H + 4];

	if (scale < 1) {
		scale = 1;
	}

	for (const char *p = s; p && *p; p++) {
		unsigned char ch = (unsigned char)*p;

		if (ch < FACE_FIRST || ch > FACE_LAST) {
			/* Outside the display face. One flat glyph rather than
			 * a hole in the word - the same fallback
			 * gfx_face_text() makes. */
			char one[2] = { *p, 0 };
			int fs = (FACE_H * scale) / FONT_H;

			gfx_text(x + (FACE_W * scale - FONT_W * (fs ? fs : 1)) / 2,
				 y, one, fs ? fs : 1, top, GFX_OPAQUE);
			x += (FACE_W + 1) * scale;
			continue;
		}

		const uint16_t *g = font10x14[ch - FACE_FIRST];

		dilate_rows(g, FACE_H, halo);
		dilate_rows(halo, FACE_H + 2, wide);

		if (glow_a) {
			gfx_glyph(x - 2 * scale, y - 2 * scale, wide,
				  FACE_W + 4, FACE_H + 4, scale, glow,
				  (uint8_t)(glow_a / 2));
			gfx_glyph(x - scale, y - scale, halo, FACE_W + 2,
				  FACE_H + 2, scale, glow, glow_a);
		}

		/*
		 * Two pixels of shade below every edge, graded, then one of
		 * specular above. All before the face, so the face covers all
		 * but the pixel each pass is there for.
		 *
		 * The second step is what makes the letter sit ON the card
		 * rather than in it: one pixel alone is an engraving, two that
		 * fade are a raised edge catching light from above.
		 */
		gfx_glyph(x, y + 2, g, FACE_W, FACE_H, scale, lo_far, 90);
		gfx_glyph(x, y + 1, g, FACE_W, FACE_H, scale, lo_near, 200);
		gfx_glyph(x, y - 1, g, FACE_W, FACE_H, scale, hi, 220);

		gfx_glyph_grad(x, y, g, FACE_W, FACE_H, scale, top, bot,
			       GFX_OPAQUE);

		x += (FACE_W + 1) * scale;
	}
}

const char *gfx_utoa(uint32_t v, char *buf, int len, int pad)
{
	char tmp[12];
	int i = 0;

	if (len < 2) {
		if (len > 0) {
			buf[0] = '\0';
		}
		return buf;
	}

	do {
		tmp[i++] = (char)('0' + (v % 10u));
		v /= 10u;
	} while (v && i < (int)sizeof(tmp));

	while (i < pad && i < (int)sizeof(tmp)) {
		tmp[i++] = '0';
	}

	int j = 0;

	while (i > 0 && j < len - 1) {
		buf[j++] = tmp[--i];
	}
	buf[j] = '\0';
	return buf;
}
