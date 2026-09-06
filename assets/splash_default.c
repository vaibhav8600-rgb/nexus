/*
 * Default splash artwork (Section 21) - the badge composition.
 *
 * This is the supplied nexus_splash.c design, rebuilt on the NEXUS
 * compositor. The layout, the palette and the four steps of hierarchy are
 * that file's; none of its code is here, and that is deliberate:
 *
 *   - It builds an LVGL widget tree. NEXUS deleted its LVGL UI precisely to
 *     get the RAM back: CONFIG_LV_Z_VDB_SIZE is pinned at 10 and LVGL holds
 *     one empty object. Ten live objects with their own styles is the thing
 *     that was removed, not an addition to it.
 *   - It needs three lv_font_conv subsets (~2 KB flash plus an external
 *     toolchain step). The 10x14 face added for the wordmark already draws
 *     N-E-X-U-S at the sizes this asks for.
 *   - It calls nexus_status_screen_show() and owns its own teardown timer.
 *     NEXUS's screen stack already owns that; two things sequencing the same
 *     transition is how you get a splash that never leaves.
 *
 * So: same picture, no widget tree, no font conversion, no second lifecycle.
 * Every shape below is a solid fill, which is also what that file was after -
 * there is no gradient here, so there is no RGB565 banding to dither.
 *
 * Geometry is quoted from the original in its own coordinates. Positions are
 * top-left of the bounding box, as they were there.
 */

#include <nexus/gfx.h>
#include <zephyr/kernel.h>   /* ARG_UNUSED */
#include <nexus/splash.h>
#include <nexus/theme.h>
#include <nexus/nexus.h>
#include <nexus/widgets.h>

/* The badge palette, verbatim. */
#define COL_BG NEXUS_C(0x0A0912u)
#define COL_PURPLE NEXUS_C(0x3B2079u)
#define COL_WINE NEXUS_C(0x59203Eu)
#define COL_ACCENT NEXUS_C(0xFF4D9Eu)
#define COL_DISC_OUTER NEXUS_C(0x100E18u)
#define COL_DISC_INNER NEXUS_C(0x1A1822u)
#define COL_WHITE NEXUS_C(0xFFFFFFu)

#define HALO_OPA 46   /* 18% */
#define HAIRLINE_OPA 23 /* 9% */

/* The mark occupies the full panel: the two background circles are placed
 * past its edges on purpose and clip against it. */
#define MARK_W 240
#define MARK_H 240 /* the badge is the whole panel, type included */

/*
 * The badge, and the order of the type.
 *
 * The reference reads top to bottom as: disc, product name, subtitle, a thin
 * divider, then the brand. This used to set the brand ABOVE the name, which
 * puts the maker's name where the product's should be and leaves the subtitle
 * floating with nothing under it.
 */
#define DISC_CY 80
#define DISC_D 92
#define HALO_D 104
#define INNER_D 76

#define WORD_Y 138
#define WORD_SCALE 2
#define SUB_Y 180
#define RULE_Y 196
#define RULE_W 132
#define BRAND_Y 204

#define COL_BRAND NEXUS_C(0xE6E9F5u)
#define COL_PRODUCT NEXUS_C(0x7C87C4u)

#define N_SCALE 3 /* 10x14 face -> 30x42, the original's 38px cap */

/* gfx_disc takes a centre; the original works in top-left corners. */
static void disc_tl(int x, int y, int d, gfx_color c, uint8_t a)
{
	gfx_disc(x + d / 2, y + d / 2, d / 2, c, a);
}

static void ring_tl(int x, int y, int d, int width, gfx_color c, uint8_t a)
{
	for (int i = 0; i < width; i++) {
		int r = d / 2 - i;

		gfx_round_frame(x + d / 2 - r, y + d / 2 - r, r * 2, r * 2, r,
				c, a);
	}
}

static void draw_default_mark(int x, int y)
{
	int lit = nexus_splash_phase();

	/* Full-panel composition: it ignores the centring offset splash.c
	 * offers, because there is nothing to centre. */
	ARG_UNUSED(x);
	ARG_UNUSED(y);

	/*
	 * Flat ground first. The screen stack paints the themed gradient
	 * before any screen draws, and this composition is explicitly not a
	 * gradient - covering it is cheaper than teaching the stack about
	 * screens that do not want it.
	 */
	gfx_rect(0, 0, GFX_W, GFX_H, COL_BG, GFX_OPAQUE);

	/* Two large circles hung off the corners, clipped by the panel. */
	disc_tl(-58, -40, 168, COL_PURPLE, GFX_OPAQUE);
	disc_tl(128, 146, 176, COL_WINE, GFX_OPAQUE);

	/* Halo, outer disc with its accent ring, inner disc with a hairline. */
	disc_tl(120 - HALO_D / 2, DISC_CY - HALO_D / 2, HALO_D, COL_ACCENT,
		HALO_OPA);

	disc_tl(120 - DISC_D / 2, DISC_CY - DISC_D / 2, DISC_D, COL_DISC_OUTER,
		GFX_OPAQUE);
	ring_tl(120 - DISC_D / 2, DISC_CY - DISC_D / 2, DISC_D, 2, COL_ACCENT,
		GFX_OPAQUE);

	disc_tl(120 - INNER_D / 2, DISC_CY - INNER_D / 2, INNER_D,
		COL_DISC_INNER, GFX_OPAQUE);
	ring_tl(120 - INNER_D / 2, DISC_CY - INNER_D / 2, INNER_D, 1, COL_WHITE,
		HAIRLINE_OPA);

	/*
	 * The N: accent copy 1px right of a white one, as in the original. The
	 * splash highlight lands on it when the sweep reaches the wordmark's
	 * first letter, so the badge pulses with the name rather than beside
	 * it.
	 */
	int nw = gfx_face_w("N", N_SCALE);
	int nh = gfx_face_h(N_SCALE);
	int nx = 120 - nw / 2;
	int ny = DISC_CY - nh / 2;

	gfx_face_text(nx + 1, ny, "N", N_SCALE, COL_ACCENT, GFX_OPAQUE);
	gfx_face_text(nx, ny, "N", N_SCALE, lit == 0 ? COL_ACCENT : COL_WHITE,
		      GFX_OPAQUE);

	/*
	 * The type, drawn here rather than by splash.c, because it belongs to
	 * this composition and to nothing else. The lines hang off the disc at
	 * measured offsets, so they are fixed rather than centred as a block:
	 * re-centring would slide them against the disc whenever a string
	 * length changed.
	 *
	 * A supplied PNG replaces this whole function, so it replaces the text
	 * with it. That is the point.
	 */
	nexus_draw_wordmark_plain(GFX_W / 2, WORD_Y, NEXUS_PRODUCT, WORD_SCALE,
				  lit);

	if (sizeof(NEXUS_SUBTITLE) > 1) {
		nexus_draw_tracked(GFX_W / 2, SUB_Y, NEXUS_SUBTITLE,
				   NEXUS_TXT_CAPTION, 2, COL_PRODUCT);
	}

	if (sizeof(NEXUS_BRAND) > 1) {
		/* A hairline, then the maker. The divider is what separates
		 * "what this is" from "who made it" - without it the brand
		 * reads as a third line of product name. */
		gfx_rect(GFX_W / 2 - RULE_W / 2, RULE_Y, RULE_W, 1, COL_ACCENT,
			 110);
		nexus_draw_tracked(GFX_W / 2, BRAND_Y, NEXUS_BRAND,
				   NEXUS_TXT_BODY, 2, COL_BRAND);
	}
}

const struct nexus_splash_art nexus_splash_art = {
	.w = MARK_W,
	.h = MARK_H,
	.draw = draw_default_mark,
};
