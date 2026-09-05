/*
 * Default splash artwork (Section 21).
 *
 * Drawn with compositor primitives rather than shipped as a bitmap: a 160x160
 * RGB565 image costs 51 KB of flash to say "NEXUS", and the geometry below
 * costs a few hundred bytes. The integrator's own PNG replaces this file
 * entirely - see scripts/png2c.py, which emits the same symbol.
 *
 * An angular N inside a lit ring, with a dotted halo around it. Built to the
 * supplied reference, and built out of the things this panel is good at:
 * hard-edged geometry, flat fills and a colour ramp. The reference's blur and
 * its fine circuit tracery are not reproduced - a 240px panel has no pixels
 * to spend on either, and a smudged approximation of a soft glow reads as a
 * rendering fault rather than as atmosphere. What carries over is the part
 * that survives the resolution: the ring, the mark, the halo, the ramp.
 */

#include <nexus/gfx.h>
#include <nexus/splash.h>
#include <nexus/theme.h>

#define MARK 96                 /* the ring's bounding box   */
#define RING_R (MARK / 2)
#define GLYPH_W 13
#define GLYPH_H 13
#define GLYPH_SCALE 3           /* 39x39 inside a 96px ring  */

/*
 * The mark: an angular N. Two uprights joined by a diagonal, with the corners
 * cut back so it reads as machined rather than typed - the letter N is the
 * one glyph in "NEXUS" that survives being drawn as geometry.
 *
 * Row-major, bit N = column N.
 */
static const uint16_t n_mark[GLYPH_H] = {
	0x1C1F, /* #####.....### */
	0x1C3F, /* ######....### */
	0x1C3F, /* ######....### */
	0x1C77, /* ###.###...### */
	0x1CE7, /* ###..###..### */
	0x1CE7, /* ###..###..### */
	0x1DC7, /* ###...###.### */
	0x1F87, /* ###....###### */
	0x1F87, /* ###....###### */
	0x1F07, /* ###.....##### */
	0x1E07, /* ###......#### */
	0x1E07, /* ###......#### */
	0x1C07, /* ###.......### */
};

/*
 * The halo, as 24 dots on a circle of radius 46. Precomputed because the
 * alternative is 24 sin/cos calls per band per frame on a chip with no FPU,
 * for a ring that never moves. 96 bytes of flash buys all of it.
 */
static const int8_t halo[24][2] = {
	{  46,   0 }, {  44,  12 }, {  40,  23 }, {  33,  33 }, {  23,  40 }, {  12,  44 },
	{   0,  46 }, { -12,  44 }, { -23,  40 }, { -33,  33 }, { -40,  23 }, { -44,  12 },
	{ -46,   0 }, { -44, -12 }, { -40, -23 }, { -33, -33 }, { -23, -40 }, { -12, -44 },
	{   0, -46 }, {  12, -44 }, {  23, -40 }, {  33, -33 }, {  40, -23 }, {  44, -12 },
};

static void draw_default_mark(int x, int y)
{
	const struct nexus_theme *t = nexus_theme();
	int cx = x + MARK / 2;
	int cy = y + MARK / 2;
	int lit = nexus_splash_phase();

	/*
	 * Halo first, so the ring overlaps it rather than the other way round.
	 * The dot under the sweep brightens, which is what ties the mark to the
	 * wordmark's highlight - one light travelling through the whole lockup
	 * instead of two things animating independently.
	 */
	for (int i = 0; i < 24; i++) {
		int dx = halo[i][0];
		int dy = halo[i][1];
		bool hot = (lit >= 0) && (i / 3 == lit % 8);
		gfx_color c = gfx_mix(t->wordmark[2], t->accent,
				      (uint8_t)((i * 255) / 23));

		if (hot) {
			gfx_disc(cx + dx, cy + dy, 3, t->value, GFX_OPAQUE);
		} else {
			gfx_disc(cx + dx, cy + dy, 2, c, 170);
		}
	}

	/* Soft interior, so the N sits on something rather than on the ground. */
	gfx_disc(cx, cy, RING_R - 4, t->accent_alt, 40);

	/*
	 * The ring, as three concentric frames with the colour walking from
	 * accent_alt to accent. Three is where the banding stops showing at
	 * this radius; a fourth is fill rate for nothing.
	 */
	for (int i = 0; i < 3; i++) {
		int r = RING_R - i;
		gfx_color c = gfx_mix(t->wordmark[2], t->accent,
				      (uint8_t)(60 + i * 90));

		gfx_round_frame(cx - r, cy - r, r * 2, r * 2, r, c, GFX_OPAQUE);
	}

	/* The mark itself, with a shadow for lift. */
	int gw = GLYPH_W * GLYPH_SCALE;
	int gh = GLYPH_H * GLYPH_SCALE;

	gfx_glyph(cx - gw / 2 + 2, cy - gh / 2 + 3, n_mark, GLYPH_W, GLYPH_H,
		  GLYPH_SCALE, t->wordmark[4], 90);
	gfx_glyph(cx - gw / 2, cy - gh / 2, n_mark, GLYPH_W, GLYPH_H,
		  GLYPH_SCALE, t->value, GFX_OPAQUE);
}

const struct nexus_splash_art nexus_splash_art = {
	.w = MARK,
	.h = MARK,
	.draw = draw_default_mark,
};
