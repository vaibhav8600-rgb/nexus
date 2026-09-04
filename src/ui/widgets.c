/*
 * Glass / neumorphic draw primitives (Sections 57-58). See widgets.h.
 */

#include <nexus/widgets.h>
#include <zephyr/kernel.h>

/*
 * A soft corner glow, as three concentric discs at low alpha. Three is where
 * the banding stops being visible on a 240px panel; more just costs fill rate,
 * and a true radial falloff would want a square root per pixel.
 */
static void glow(int cx, int cy, int r, gfx_color c, uint8_t alpha)
{
	if (alpha == 0) {
		return;
	}

	for (int i = 0; i < 3; i++) {
		int rr = r - i * (r / 4);

		gfx_round_rect(cx - rr, cy - rr, 2 * rr, 2 * rr, rr, c, alpha);
	}
}

void nexus_draw_ground(void)
{
	const struct nexus_theme *t = nexus_theme();

	/* The gradient clips to the band internally, so this only ever touches
	 * GFX_STRIP_H rows however many times it is called. */
	gfx_vgrad(0, GFX_H, t->bg_top, t->bg_bot);

	if (t->glow_alpha == 0) {
		return;
	}

	/* Off-canvas centres, so only the bright inner part of each disc lands
	 * on screen - the same trick the CSS reference uses with its blurred
	 * blobs pushed past the corners. */
	glow(20, 6, 92, t->glow_a, t->glow_alpha);
	glow(GFX_W - 20, GFX_H - 6, 94, t->glow_b, t->glow_alpha);
}

void nexus_draw_card(int x, int y, int w, int h)
{
	const struct nexus_theme *t = nexus_theme();
	int r = t->radius;

	/* 1. The tint. Over a linear gradient this is a true frosted composite,
	 *    which is the whole reason the ground is a gradient. */
	gfx_round_rect(x, y, w, h, r, t->panel, t->panel_alpha);

	/* 2. Light pooling toward the top of the pane. Four short steps keep
	 *    the ramp cheap and still read as a curved surface. */
	for (int i = 0; i < 4; i++) {
		uint8_t a = (uint8_t)((t->edge_hi_alpha / 6) - i * 3);

		if (a == 0 || a > 200) { /* underflowed past zero */
			break;
		}
		gfx_rect(x + r, y + 1 + i * 2, w - 2 * r, 2, t->edge_hi, a);
	}

	/* 3. The specular top edge - this is what sells it as glass. */
	gfx_hline(x + r, y, w - 2 * r, t->edge_hi, t->edge_hi_alpha);

	/* 4. Shaded bottom edge, for thickness. */
	gfx_hline(x + r, y + h - 1, w - 2 * r, t->edge_lo, t->edge_lo_alpha);
}

void nexus_draw_card_sel(int x, int y, int w, int h, bool selected)
{
	const struct nexus_theme *t = nexus_theme();

	nexus_draw_card(x, y, w, h);

	if (selected) {
		gfx_round_frame(x, y, w, h, t->radius, t->accent, GFX_OPAQUE);
	}
}

void nexus_draw_caption(int x, int y, const char *text)
{
	gfx_text(x, y, text, NEXUS_TXT_CAPTION, nexus_theme()->caption,
		 GFX_OPAQUE);
}

void nexus_draw_caption_c(int cx, int y, const char *text)
{
	gfx_text_c(cx, y, text, NEXUS_TXT_CAPTION, nexus_theme()->caption,
		   GFX_OPAQUE);
}

void nexus_draw_meter(int x, int y, int w, int h, uint8_t pct, gfx_color fill)
{
	const struct nexus_theme *t = nexus_theme();
	int r = h / 2;

	if (pct > 100) {
		pct = 100;
	}

	gfx_round_rect(x, y, w, h, r, t->track, 190);

	int fw = (w * pct) / 100;

	/* Below one capsule width there is no room for two round caps, so snap
	 * up: a 2% battery should still show something, not a smear. */
	if (fw > 0 && fw < h) {
		fw = h;
	}
	if (fw > 0) {
		gfx_round_rect(x, y, fw, h, r, fill, GFX_OPAQUE);
	}
}

void nexus_draw_pill(int x, int y, int w, int h, bool active, const char *text)
{
	const struct nexus_theme *t = nexus_theme();
	int r = h / 3;

	if (active) {
		gfx_round_rect(x, y, w, h, r, t->accent_alt, 96);
		gfx_round_frame(x, y, w, h, r, t->accent_alt, 200);
	} else {
		gfx_round_rect(x, y, w, h, r, t->panel, t->panel_alpha / 2);
		gfx_round_frame(x, y, w, h, r, t->border, t->border_alpha / 2);
	}

	if (text) {
		gfx_text_c(x + w / 2,
			   y + (h - gfx_text_h(NEXUS_TXT_CAPTION)) / 2, text,
			   NEXUS_TXT_CAPTION, active ? t->value : t->muted,
			   GFX_OPAQUE);
	}
}

void nexus_draw_wordmark(int cx, int y, const char *text, int scale)
{
	const struct nexus_theme *t = nexus_theme();

	/* Back to front: shadow steps first, face last. Same stacking the CSS
	 * reference does with layered text-shadows. */
	for (int i = 4; i >= 1; i--) {
		gfx_text_c(cx, y + i, text, scale, t->wordmark[i], GFX_OPAQUE);
	}
	gfx_text_c(cx, y, text, scale, t->wordmark[0], GFX_OPAQUE);
}
