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

#include "buddy_priv.h"

static gfx_color slot(const struct nexus_theme *t, uint8_t c)
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

static void extras(const struct face_frame *f, gfx_color c, gfx_color dim)
{
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
		for (int i = 0; i < 3; i++) {
			gfx_disc(202 + i * 12, FACE_EXTRA_Y0 + 10, 4,
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
	extras(f, c, gfx_mix(t->bg_bot, c, 110));
}
