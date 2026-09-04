/*
 * Default splash artwork (Section 21).
 *
 * Drawn with compositor primitives rather than shipped as a bitmap: a 160x160
 * RGB565 image costs 51 KB of flash to say "NEXUS", and the geometry below
 * costs a few hundred bytes. The integrator's own PNG replaces this file
 * entirely - see scripts/png2c.py, which emits the same symbol.
 *
 * The mark is four blocks on two levels: low, tall, low, tall. A nexus is a
 * junction, and that is what a signal crossing between two lanes looks like.
 *
 * It replaced a ringed cross, which was the problem - a circle with a plus in
 * it is the most drawn shape in the world and belongs to nobody. Blocks on a
 * grid are the one thing a 240px panel renders better than a print logo does,
 * so the mark leans on that instead of apologising for it: hard edges, a
 * colour ramp left to right, and the same highlight that sweeps the wordmark
 * running through it so the two read as one lockup.
 */

#include <nexus/gfx.h>
#include <nexus/splash.h>
#include <nexus/theme.h>

#define BLK 22               /* block edge                       */
#define BLK_GAP 5
#define BLK_N 4
#define MARK_W (BLK_N * BLK + (BLK_N - 1) * BLK_GAP) /* 103 */
#define MARK_H (BLK * 2 + BLK_GAP)                   /*  49 */
#define BLK_R 6

/* Vertical lane per block: 0 = top, 1 = bottom. Low, tall, low, tall. */
static const uint8_t lane[BLK_N] = { 1, 0, 1, 0 };

static void draw_default_mark(int x, int y)
{
	const struct nexus_theme *t = nexus_theme();
	int lit = nexus_splash_phase();

	for (int i = 0; i < BLK_N; i++) {
		int bx = x + i * (BLK + BLK_GAP);
		int by = y + (lane[i] ? BLK + BLK_GAP : 0);

		/*
		 * Same ramp the wordmark uses, so the mark and the name are
		 * visibly the same object rather than two things that happen
		 * to be stacked.
		 */
		uint8_t mixv = (uint8_t)((i * 255) / (BLK_N - 1));
		gfx_color c = gfx_mix(t->wordmark[2], t->accent, mixv);

		if (i == lit) {
			c = t->value;
		}

		/* A shadow block offset into the gap gives the pair of lanes
		 * some depth without a second colour. */
		gfx_round_rect(bx + 2, by + 3, BLK, BLK, BLK_R, t->wordmark[4],
			       70);
		gfx_round_rect(bx, by, BLK, BLK, BLK_R, c, GFX_OPAQUE);

		/* Specular top edge, the same trick the glass cards use. */
		gfx_hline(bx + BLK_R, by, BLK - 2 * BLK_R, t->edge_hi,
			  t->edge_hi_alpha);
	}

	/*
	 * The connector: a thin rail through the middle tying the two lanes
	 * together. Without it the blocks are four squares; with it they are
	 * a path.
	 */
	gfx_round_rect(x + BLK / 2, y + BLK + BLK_GAP / 2 - 1, MARK_W - BLK, 3,
		       1, t->accent, 130);
}

const struct nexus_splash_art nexus_splash_art = {
	.w = MARK_W,
	.h = MARK_H,
	.draw = draw_default_mark,
};
