/*
 * Default splash artwork (Section 21).
 *
 * Drawn with compositor primitives rather than shipped as a bitmap: a 160x160
 * RGB565 image costs 51 KB of flash to say "NEXUS", and the geometry below
 * costs a few hundred bytes. The integrator's own PNG replaces this file
 * entirely - see scripts/png2c.py, which emits the same symbol.
 *
 * A node with links running out of it, which is what "nexus" means.
 */

#include <nexus/gfx.h>
#include <nexus/splash.h>
#include <nexus/theme.h>

#define MARK 84
#define BAR_LONG 56
#define BAR_THIN 12
#define DOT 18

static void draw_default_mark(int x, int y)
{
	const struct nexus_theme *t = nexus_theme();
	int cx = x + MARK / 2;
	int cy = y + MARK / 2;

	/* Outer ring. */
	gfx_round_frame(x, y, MARK, MARK, MARK / 2, t->accent, 190);
	gfx_round_frame(x + 1, y + 1, MARK - 2, MARK - 2, MARK / 2 - 1,
			t->accent, 70);

	/* Two crossed bars through the centre. */
	gfx_round_rect(cx - BAR_LONG / 2, cy - BAR_THIN / 2, BAR_LONG, BAR_THIN,
		       BAR_THIN / 2, t->accent, GFX_OPAQUE);
	gfx_round_rect(cx - BAR_THIN / 2, cy - BAR_LONG / 2, BAR_THIN, BAR_LONG,
		       BAR_THIN / 2, t->accent_alt, GFX_OPAQUE);

	/* Centre node. */
	gfx_round_rect(cx - DOT / 2, cy - DOT / 2, DOT, DOT, DOT / 2, t->value,
		       GFX_OPAQUE);
}

const struct nexus_splash_art nexus_splash_art = {
	.w = MARK,
	.h = MARK,
	.draw = draw_default_mark,
};
