/*
 * Buddy's face, drawn: eyes, mouth and the little extras, from one frame.
 *
 * Procedural, not sprites. Nine emotions as bitmaps would be kilobytes of
 * flash per theme, and could not tween between each other; as geometry they
 * are a table row each and every in-between is free.
 *
 * Eyes are drawn a row at a time, with the lids and the happy crescent cut
 * out of each row rather than painted over it. Painting a lid would need the
 * colour of what is behind the eye, and behind it is the theme's gradient and
 * its glows - there is no one colour to paint. A row that simply stops where
 * the lid begins needs to know nothing about the ground at all.
 *
 * Every row's two ends are anti-aliased from 1/16 px coverage, which is what
 * keeps a 12px sleepy slit or a slanted angry lid from looking like stairs.
 */

#include <nexus/gfx.h>
#include <nexus/theme.h>
#include <zephyr/kernel.h>
#include <stdlib.h>

#include "buddy_priv.h"

static gfx_color theme_slot(const struct nexus_theme *t, uint8_t c)
{
	switch (c) {
	case FACE_C_GLOW_B:
		return t->glow_b;
	case FACE_C_ERROR:
		return t->error;
	case FACE_C_ACCENT_ALT:
		return t->accent_alt;
	case FACE_C_WARNING:
		return t->warning;
	case FACE_C_MUTED:
		return t->muted;
	default:
		return t->accent;
	}
}

/* Rough brightness of an RGB565 colour, 0-255. */
static int luma(gfx_color c)
{
	int r = (c >> 11) * 255 / 31;
	int g = ((c >> 5) & 63) * 255 / 63;
	int b = (c & 31) * 255 / 31;

	return (2 * r + 5 * g + b) / 8;
}

/*
 * A slot's colour, or the text colour where the slot would vanish. A theme
 * with no glows sets glow_b to black - AMOLED, CLAY - and a sad face drawn
 * in it disappears into the ground. Too close to the middle of the gradient,
 * where the face sits, it is drawn in the theme's text colour instead.
 */
static gfx_color slot(const struct nexus_theme *t, uint8_t c)
{
	gfx_color col = theme_slot(t, c);
	int d = luma(col) - luma(gfx_mix(t->bg_top, t->bg_bot, 128));

	return (d < 0 ? -d : d) < 48 ? t->value : col;
}

/*
 * gfx.c has one of these, static. A second copy of ten lines costs less than
 * making that one public would cost the builds without Buddy, which are meant
 * to come out exactly as they did.
 */
static uint32_t isqrt(uint32_t n)
{
	uint32_t r = 0;
	uint32_t bit = 1u << 30;

	while (bit > n) {
		bit >>= 2;
	}
	while (bit) {
		if (n >= r + bit) {
			n -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* Row @p y covered over [l, r), in 1/16 px. */
static void span(int y, int32_t l, int32_t r, gfx_color c)
{
	if (l < 0) {
		l = 0;
	}
	if (r > GFX_W * 16) {
		r = GFX_W * 16;
	}
	if (r <= l) {
		return;
	}

	int xl = (int)(l >> 4);
	int xr = (int)(r >> 4);

	if (xl == xr) {
		gfx_hline(xl, y, 1, c, (uint8_t)((r - l) * 255 / 16));
		return;
	}
	if (l & 15) {
		gfx_hline(xl, y, 1, c, (uint8_t)((16 - (l & 15)) * 255 / 16));
		xl++;
	}
	gfx_hline(xl, y, xr - xl, c, GFX_OPAQUE);
	if (r & 15) {
		gfx_hline(xr, y, 1, c, (uint8_t)((r & 15) * 255 / 16));
	}
}

/* The live band's rows of [top, bot). */
static bool band_rows(int top, int bot, int *y0, int *y1)
{
	*y0 = MAX(top, gfx_band_y0());
	*y1 = MIN(bot, gfx_band_y1());
	return *y0 < *y1;
}

/* Half-width of a circle of radius @p r16 at @p dy16 from its centre, or -1
 * outside it. All 1/16 px. */
static int32_t chord(int32_t r16, int32_t dy16)
{
	if (dy16 <= -r16 || dy16 >= r16) {
		return -1;
	}
	return (int32_t)isqrt((uint32_t)(r16 * r16 - dy16 * dy16));
}

/*
 * One eye. @p side is +1 for the right eye and -1 for the left, which is only
 * a question of which edge is "outer" for the lids.
 */
static void eye(const struct face_frame *f, int cx, int cy, int side,
		gfx_color c)
{
	int w = f->g[F_W];
	int h = f->g[F_H];
	int r = MIN(f->g[F_R], MIN(w / 2, h / 2));
	int top = cy - h / 2;
	int bot = top + h;
	int y0, y1;

	if (w <= 0 || h <= 0 || !band_rows(top, bot, &y0, &y1)) {
		return;
	}

	int32_t x0 = cx * 16 - w * 8;
	int32_t x1 = x0 + w * 16;
	/* The lid's drop at the left and right edges. */
	int32_t a = side > 0 ? f->g[F_LID_IN] : f->g[F_LID_OUT];
	int32_t b = side > 0 ? f->g[F_LID_OUT] : f->g[F_LID_IN];
	/* The crescent: a circle cut from below, its top lid_bot up. */
	int32_t cut_r = (int32_t)w * 3 / 4 * 16;
	int32_t cut_y = (int32_t)(bot - f->g[F_LID_BOT]) * 16 + cut_r;

	for (int y = y0; y < y1; y++) {
		int32_t yq = y * 16 + 8;
		int32_t l = x0;
		int32_t rr = x1;
		int32_t dy = 0;

		if (yq < (top + r) * 16) {
			dy = (top + r) * 16 - yq;
		} else if (yq > (bot - r) * 16) {
			dy = yq - (bot - r) * 16;
		}
		if (dy) {
			int32_t half = chord(r * 16, dy);

			if (half < 0) {
				continue;
			}
			l += r * 16 - half;
			rr -= r * 16 - half;
		}

		/* The lid: a straight edge from depth a at x0 to b at x1. Below
		 * it is eye; above it is not. Solved for x, so a row costs a
		 * divide rather than a test per pixel. */
		int32_t d = yq - top * 16;

		if (a == b) {
			if (d < a * 16) {
				continue;
			}
		} else if (b > a) {
			int32_t lim = x0 + (d - a * 16) * w / (b - a);

			rr = MIN(rr, lim);
		} else {
			int32_t lim = x0 + (a * 16 - d) * w / (a - b);

			l = MAX(l, lim);
		}
		if (rr <= l) {
			continue;
		}

		if (f->g[F_LID_BOT] > 0) {
			int32_t half = chord(cut_r, yq - cut_y);

			if (half >= 0) {
				span(y, l, MIN(rr, cx * 16 - half), c);
				span(y, MAX(l, cx * 16 + half), rr, c);
				continue;
			}
		}
		span(y, l, rr, c);
	}
}

/* Love: two lobes and a point, sized from the eye's width. */
static void heart(int cx, int cy, int w, gfx_color c)
{
	int r = w / 4;
	int lobe_y = cy - r / 2;
	int tip = cy + w / 2 - 2;
	int y0, y1;

	if (r <= 0 || !band_rows(lobe_y - r, tip, &y0, &y1)) {
		return;
	}

	for (int y = y0; y < y1; y++) {
		int32_t yq = y * 16 + 8;
		int32_t half = chord(r * 16, yq - lobe_y * 16);

		if (half >= 0) {
			span(y, (cx - r) * 16 - half, (cx - r) * 16 + half, c);
			span(y, (cx + r) * 16 - half, (cx + r) * 16 + half, c);
		}
		if (yq > lobe_y * 16) {
			/* The point narrows from the lobes' full width to 0. */
			int32_t w16 = (int32_t)(2 * r) * (tip * 16 - yq) /
				      (tip - lobe_y);

			span(y, cx * 16 - w16, cx * 16 + w16, c);
		}
	}
}

#define ARC_LOW 1
#define ARC_HIGH 2

/* A ring of radius @p rad and thickness @p th, its lower or upper half. */
static void arc(int cx, int cy, int rad, int th, int which, gfx_color c)
{
	int y0, y1;
	int top = (which & ARC_HIGH) ? cy - rad : cy;
	int bot = (which & ARC_LOW) ? cy + rad : cy;

	if (!band_rows(top, bot, &y0, &y1)) {
		return;
	}
	for (int y = y0; y < y1; y++) {
		int32_t dy = y * 16 + 8 - cy * 16;
		int32_t out = chord(rad * 16, dy);
		int32_t in = chord((rad - th) * 16, dy);

		if (out < 0) {
			continue;
		}
		if (in < 0) {
			span(y, cx * 16 - out, cx * 16 + out, c);
		} else {
			span(y, cx * 16 - out, cx * 16 - in, c);
			span(y, cx * 16 + in, cx * 16 + out, c);
		}
	}
}

static void mouth(const struct face_frame *f, gfx_color c)
{
	int cx = FACE_CX + f->g[F_GAZE_X] / 3;
	int my = FACE_MOUTH_Y;

	switch (f->mouth) {
	case FACE_M_LINE:
		gfx_round_rect(cx - 14, my - 2, 28, 5, 2, c, GFX_OPAQUE);
		break;
	case FACE_M_SMILE:
		arc(cx, my - 8, 15, 5, ARC_LOW, c);
		break;
	case FACE_M_FROWN:
		arc(cx, my + 10, 15, 5, ARC_HIGH, c);
		break;
	case FACE_M_O:
		arc(cx, my + 2, 9, 4, ARC_LOW | ARC_HIGH, c);
		break;
	case FACE_M_TALK: {
		int h = 4 + f->open * 16 / 255;

		gfx_round_rect(cx - 11, my + 2 - h / 2, 22, h, MIN(h / 2, 6), c,
			       GFX_OPAQUE);
		break;
	}
	default:
		break;
	}
}

/* Three small bars that rise and fall: listening. Where the mouth would be. */
static const uint8_t k_bars[8][3] = {
	{ 6, 14, 8 },  { 10, 18, 6 }, { 14, 10, 12 }, { 8, 6, 18 },
	{ 12, 14, 10 }, { 18, 8, 6 }, { 6, 12, 14 }, { 10, 16, 8 },
};

/* ---- props: what the tricks hold ---------------------------------------- */

/* An open hand, fingers up, its top-left at (x, y): 26 x 36 px. */
static void hand(int x, int y, gfx_color c)
{
	for (int i = 0; i < 4; i++) {
		gfx_round_rect(x + 1 + i * 6, y + (i == 0 || i == 3 ? 4 : 0), 5, 18,
			       2, c, GFX_OPAQUE);
	}
	gfx_round_rect(x, y + 14, 26, 22, 7, c, GFX_OPAQUE);
}

/* A quaver, its head at (x, y). */
static void note(int x, int y, gfx_color c)
{
	gfx_disc(x, y, 4, c, GFX_OPAQUE);
	gfx_rect(x + 2, y - 12, 2, 12, c, GFX_OPAQUE);
	gfx_rect(x + 2, y - 12, 6, 2, c, GFX_OPAQUE);
}

/* A 2 px line, for the crack. */
static void line2(int x0, int y0, int x1, int y1, gfx_color c)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int n = MAX(abs(dx), abs(dy));

	for (int i = 0; i <= n; i++) {
		gfx_rect(x0 + dx * i / MAX(n, 1), y0 + dy * i / MAX(n, 1), 2, 2, c,
			 GFX_OPAQUE);
	}
}

/*
 * The crack: jagged rays from a hit at (CRACK_X, CRACK_Y), and a few
 * cross-pieces between them for the web. Segments, relative to the hit.
 * Everything stays between the header and the mouth band's bottom.
 */
#define CRACK_X 70
#define CRACK_Y 96
static const int16_t k_crack[][4] = {
	{ 0, 0, 40, -10 },   { 40, -10, 85, -6 },  { 85, -6, 150, -30 },
	{ 0, 0, 30, 18 },    { 30, 18, 70, 30 },   { 70, 30, 130, 70 },
	{ 0, 0, 8, 30 },     { 8, 30, -4, 60 },    { -4, 60, 10, 84 },
	{ 0, 0, -30, 20 },   { -30, 20, -50, 52 }, { -50, 52, -64, 80 },
	{ 0, 0, -34, -12 },  { -34, -12, -60, -10 },
	{ 0, 0, -10, -30 },  { -10, -30, 4, -52 }, { 4, -52, -6, -64 },
	{ 0, 0, 26, -34 },   { 26, -34, 30, -56 },
	{ 40, -10, 26, -34 }, { 30, 18, 40, -10 }, { 8, 30, 30, 18 },
	{ -30, 20, 8, 30 },  { -34, -12, -30, 20 }, { -10, -30, -34, -12 },
	{ 85, -6, 70, 30 },  { -4, 60, -50, 52 },
};

static void extras(const struct face_frame *f, gfx_color c, gfx_color dim,
		   gfx_color glass)
{
	int p = f->phase;

	switch (f->extra) {
	case FACE_X_ZZZ:
		/* Three Zs climbing to the upper right, the newest one lit. */
		for (int i = 0; i < 3; i++) {
			int s = i == 1 ? 3 : 2;
			int x = 188 + i * 13;
			int y = FACE_EXTRA_Y1 - 20 - i * 15 - (f->phase & 1);

			gfx_text(x, y, "Z", s, (f->phase / 2) % 3 == i ? c : dim,
				 GFX_OPAQUE);
		}
		break;
	case FACE_X_DOTS:
		/* "..." where the mouth would be, one dot lit at a time. */
		for (int i = 0; i < 3; i++) {
			gfx_disc(FACE_CX - 14 + i * 14, FACE_MOUTH_Y, 4,
				 (f->phase % 3) == i ? c : dim, GFX_OPAQUE);
		}
		break;
	case FACE_X_BARS:
		for (int i = 0; i < 3; i++) {
			int h = k_bars[f->phase][i];

			gfx_round_rect(FACE_CX - 17 + i * 13, FACE_MOUTH_Y + 2 - h / 2,
				       8, h, 3, c, GFX_OPAQUE);
		}
		break;
	case FACE_X_MIC: {
		int y = 152 + ((p & 2) ? 1 : 0);

		gfx_disc(FACE_CX, y, 11, c, GFX_OPAQUE);
		for (int i = -1; i <= 1; i++) {
			gfx_rect(FACE_CX - 8, y + i * 4, 16, 1, dim, GFX_OPAQUE);
		}
		gfx_round_rect(FACE_CX - 5, y + 10, 10, 20, 4, c, GFX_OPAQUE);
		/* Two notes taking turns to bob up beside it. */
		if (p < 4) {
			note(62, 170 - p * 4, c);
		} else {
			note(172, 170 - (p - 4) * 4, c);
		}
		break;
	}
	case FACE_X_HAND:
		hand(170 + (p & 1) * 3, 144 - ((p & 2) ? 2 : 0), c);
		break;
	case FACE_X_HANDS:
		hand(26 + (p & 1), 144, c);
		hand(188 - (p & 1), 144, c);
		break;
	case FACE_X_THUMB:
		gfx_round_rect(160, 160, 30, 22, 7, c, GFX_OPAQUE);
		gfx_round_rect(166, 141 - (p & 1), 10, 22, 4, c, GFX_OPAQUE);
		gfx_rect(163, 167, 24, 1, dim, GFX_OPAQUE);
		gfx_rect(163, 173, 24, 1, dim, GFX_OPAQUE);
		break;
	case FACE_X_PAD:
		gfx_round_rect(70, 152, 100, 30, 13, c, GFX_OPAQUE);
		gfx_rect(84, 165, 16, 4, dim, GFX_OPAQUE);
		gfx_rect(90, 159, 4, 16, dim, GFX_OPAQUE);
		/* The buttons being mashed, one lit at a time. */
		gfx_disc(146, 163, 4, (p & 1) ? glass : dim, GFX_OPAQUE);
		gfx_disc(156, 171, 4, (p & 1) ? dim : glass, GFX_OPAQUE);
		break;
	case FACE_X_CRACK:
		for (size_t i = 0; i < ARRAY_SIZE(k_crack); i++) {
			line2(CRACK_X + k_crack[i][0], CRACK_Y + k_crack[i][1],
			      CRACK_X + k_crack[i][2], CRACK_Y + k_crack[i][3], glass);
		}
		gfx_disc(CRACK_X, CRACK_Y, 5, glass, GFX_OPAQUE);
		break;
	default:
		break;
	}
}

void face_draw(const struct face_frame *f)
{
	const struct nexus_theme *t = nexus_theme();
	gfx_color c = gfx_mix(slot(t, f->colour_prev), slot(t, f->colour),
			      f->colour_mix);
	int cy = FACE_EYE_Y + f->g[F_Y] + f->g[F_GAZE_Y];
	int sp = f->g[F_SPACING];
	int gx = f->g[F_GAZE_X];
	int ey0, ey1;

	face_eye_rows(f, &ey0, &ey1);
	if (gfx_hits(ey0, ey1 - ey0)) {
		if (f->extra == FACE_X_HEARTS) {
			heart(FACE_CX - sp + gx, cy, f->g[F_W], c);
			heart(FACE_CX + sp + gx, cy, f->g[F_W], c);
		} else {
			eye(f, FACE_CX - sp + gx, cy, -1, c);
			eye(f, FACE_CX + sp + gx, cy, 1, c);
		}
	}

	if (gfx_hits(FACE_MOUTH_Y0, FACE_MOUTH_Y1 - FACE_MOUTH_Y0)) {
		mouth(f, c);
	}
	extras(f, c, gfx_mix(t->bg_bot, c, 110), t->value);
}
