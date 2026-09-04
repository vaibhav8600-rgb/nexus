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
static const uint8_t icons[GFX_ICON_COUNT][FONT_W] = {
	/* Bluetooth rune: vertical stem with the two crossed triangles. */
	[GFX_ICON_BT]   = { 0x14, 0x08, 0x7F, 0x2A, 0x14 },
	/* Monitor on a stand - "there is a host on the other end of a wire". */
	[GFX_ICON_USB]  = { 0x0F, 0x49, 0x79, 0x49, 0x0F },
	/* Padlock: shackle over a solid body. */
	[GFX_ICON_LOCK] = { 0x7E, 0x79, 0x71, 0x79, 0x7E },
	/* Modifiers, drawn as the symbols the keys are actually printed with
	 * rather than the initials C/S/A/G, which needed a legend to read. */
	[GFX_ICON_CTRL]  = { 0x10, 0x08, 0x04, 0x08, 0x10 }, /* caret     */
	[GFX_ICON_SHIFT] = { 0x04, 0x06, 0x3F, 0x06, 0x04 }, /* up arrow  */
	[GFX_ICON_ALT]   = { 0x30, 0x18, 0x0D, 0x07, 0x03 }, /* option    */
	[GFX_ICON_GUI]   = { 0x36, 0x36, 0x00, 0x36, 0x36 }, /* four panes*/
};

int gfx_icon_w(int scale)
{
	return FONT_W * (scale < 1 ? 1 : scale);
}

void gfx_icon(int x, int y, enum gfx_icon icon, int scale, gfx_color c,
	      uint8_t a)
{
	if (icon >= GFX_ICON_COUNT) {
		return;
	}
	if (scale < 1) {
		scale = 1;
	}
	if (y + FONT_H * scale <= band_y0 || y >= gfx_band_y1()) {
		return;
	}

	const uint8_t *g = icons[icon];

	for (int col = 0; col < FONT_W; col++) {
		uint8_t bits = g[col];

		for (int row = 0; row < FONT_H; row++) {
			if (!(bits & (1u << row))) {
				continue;
			}
			gfx_rect(x + col * scale, y + row * scale, scale, scale,
				 c, a);
		}
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
