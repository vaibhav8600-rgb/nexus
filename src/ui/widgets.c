/*
 * Glass / neumorphic draw primitives (Sections 57-58). See widgets.h.
 */

#include <nexus/widgets.h>
#include <zephyr/kernel.h>

/*
 * A soft corner glow, as three concentric discs at low alpha. Three is where
 * the banding stops being visible on a 240px panel; more just costs fill rate.
 *
 * gfx_disc(), not gfx_round_rect(): a rounded rect anti-aliases its corner
 * arcs, and for a 90px "corner radius" that is the whole shape, so every one
 * of ~80,000 glow pixels per repaint paid an integer square root. That alone
 * was tens of milliseconds a frame and made repaints visibly crawl down the
 * panel. gfx_disc() does one square root per row instead, and on a low-alpha
 * blob the missing edge AA cannot be seen.
 */
static void glow(int cx, int cy, int r, gfx_color c, uint8_t alpha)
{
	if (alpha == 0) {
		return;
	}

	for (int i = 0; i < 3; i++) {
		gfx_disc(cx, cy, r - i * (r / 4), c, alpha);
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

int nexus_tracked_w(const char *text, int scale, int track)
{
	int n = 0;

	for (const char *p = text; p && *p; p++) {
		n++;
	}
	return n ? gfx_text_w(text, scale) + (n - 1) * track : 0;
}

void nexus_draw_tracked(int cx, int y, const char *text, int scale, int track,
			gfx_color c)
{
	int x = cx - nexus_tracked_w(text, scale, track) / 2;
	char one[2] = { 0, 0 };

	for (const char *p = text; p && *p; p++) {
		one[0] = *p;
		gfx_text(x, y, one, scale, c, GFX_OPAQUE);
		x += gfx_text_w(one, scale) + scale + track;
	}
}

#define WORDMARK_RULE_GAP 4
#define WORDMARK_RULE_H 3

int nexus_wordmark_h(int scale)
{
	return gfx_text_h(scale) + WORDMARK_RULE_GAP + WORDMARK_RULE_H;
}

void nexus_draw_wordmark_lit(int cx, int y, const char *text, int scale,
			     int lit)
{
	const struct nexus_theme *t = nexus_theme();
	int w = gfx_text_w(text, scale);
	int x = cx - w / 2;
	int n = 0;
	char one[2] = { 0, 0 };

	for (const char *p = text; p && *p; p++) {
		n++;
	}
	if (n == 0) {
		return;
	}

	/* One soft shadow for lift, not a stack of them. */
	gfx_text(x + 2, y + 3, text, scale, t->wordmark[4], 90);

	/*
	 * Each letter takes its colour from a ramp across the word, so the
	 * mark has movement standing still - a flat block of one colour was
	 * the thing that never looked finished. gfx_mix walks accent_alt to
	 * accent left to right.
	 *
	 * Faux bold on top: a 5x7 face keeps one-pixel strokes however far it
	 * scales, so stamping each glyph 2x2 is what actually makes it read as
	 * bold rather than as big-and-thin.
	 */
	int pen = x;

	for (int i = 0; i < n; i++) {
		uint8_t mixv = (uint8_t)((i * 255) / (n > 1 ? n - 1 : 1));
		gfx_color c = gfx_mix(t->wordmark[2], t->accent, mixv);

		if (i == lit) {
			c = t->value; /* the sweeping highlight */
		}

		one[0] = text[i];
		for (int dy = 0; dy <= 1; dy++) {
			for (int dx = 0; dx <= 1; dx++) {
				gfx_text(pen + dx, y + dy, one, scale, c,
					 GFX_OPAQUE);
			}
		}
		pen += gfx_text_w(one, scale) + scale;
	}

	/* Accent rule: what reads as "this is a product name". */
	gfx_round_rect(x, y + gfx_text_h(scale) + WORDMARK_RULE_GAP, w,
		       WORDMARK_RULE_H, 1, t->accent, GFX_OPAQUE);
}

void nexus_draw_wordmark(int cx, int y, const char *text, int scale)
{
	nexus_draw_wordmark_lit(cx, y, text, scale, -1);
}
