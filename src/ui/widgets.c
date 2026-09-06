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

void nexus_draw_label(int x, int y, const char *text)
{
	gfx_text(x, y, text, NEXUS_TXT_LABEL, nexus_theme()->caption,
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

/*
 * Extruded arcade lettering, per the supplied references.
 *
 * The old wordmark was flat glyphs over a soft 90-alpha drop shadow, with the
 * colour ramping left-to-right across the word. That reads as a caption set
 * large. The references read as OBJECTS: each letter is a solid block with a
 * bright edge, shaded top-to-bottom, sitting on its own extrusion.
 *
 * Three things do that, and the ramp direction is one of them - shading down
 * the letter is light falling on it, shading across the word is a gradient
 * applied to text. The horizontal ramp is gone.
 *
 * The palette is the theme's existing wordmark[] with no new fields:
 *
 *   [1]  the bright edge      lightest stop in every theme
 *   [2]  face, top
 *   [3]  face, bottom
 *   [4]  the extrusion        darkest stop in every theme
 *
 * ([0] is each theme's ground and stays unused, as it was before.)
 */
static int wordmark_depth(int scale)
{
	/* One pixel of extrusion per scale step: a fixed depth would vanish on
	 * the big About wordmark and swamp a small one. */
	return scale < 1 ? 1 : scale;
}

/* Face, plus the halo cell above and below, plus the extrusion. */
static int wordmark_ink_h(int scale)
{
	int s = scale < 1 ? 1 : scale;

	return gfx_face_h(s) + 2 * s + wordmark_depth(s);
}

int nexus_wordmark_h(int scale)
{
	return wordmark_ink_h(scale) + WORDMARK_RULE_GAP + WORDMARK_RULE_H;
}

static void wordmark_letters(int cx, int y, const char *text, int scale,
			     int lit)
{
	const struct nexus_theme *t = nexus_theme();
	int s = scale < 1 ? 1 : scale;
	int w = gfx_face_w(text, s);
	int pen = cx - w / 2;
	char one[2] = { 0, 0 };

	/* y is the top of the INK, and the halo owns the cell above the face.
	 * Callers centre with nexus_wordmark_h(), so the outline and the
	 * extrusion are inside the box they measured rather than bleeding out
	 * of the card the wordmark sits in. */
	int fy = y + s;

	for (const char *p = text; p && *p; p++) {
		gfx_color top = t->wordmark[2];
		gfx_color bot = t->wordmark[3];

		if (lit >= 0 && (p - text) == lit) {
			/* The splash walks this along the word. Flat, not
			 * shaded: the point is that one letter is lit. */
			top = bot = t->value;
		}

		one[0] = *p;
		gfx_face_text_3d(pen, fy, one, s, top, bot, t->wordmark[1],
				 t->wordmark[4], wordmark_depth(s));
		pen += gfx_face_w(one, s) + s;
	}
}

void nexus_draw_wordmark_lit(int cx, int y, const char *text, int scale,
			     int lit)
{
	const struct nexus_theme *t = nexus_theme();
	int w = gfx_face_w(text, scale);

	wordmark_letters(cx, y, text, scale, lit);

	/* Accent rule: what reads as "this is a product name". Below the ink,
	 * not below the face - otherwise the extrusion lands on it. */
	gfx_round_rect(cx - w / 2, y + wordmark_ink_h(scale) + WORDMARK_RULE_GAP,
		       w, WORDMARK_RULE_H, 1, t->accent, GFX_OPAQUE);
}

void nexus_draw_wordmark_plain(int cx, int y, const char *text, int scale,
			       int lit)
{
	wordmark_letters(cx, y, text, scale, lit);
}

void nexus_draw_wordmark(int cx, int y, const char *text, int scale)
{
	nexus_draw_wordmark_lit(cx, y, text, scale, -1);
}

/* ---- the shared game modal --------------------------------------------- */

#define OVER_X 18
#define OVER_Y 56
#define OVER_W 204
#define OVER_H 128
#define STAT_GAP 18
#define HINT_GAP 6

void nexus_draw_game_overlay(const char *title, const char *hint,
			     const char *hint2, bool scores, uint32_t score,
			     uint32_t best)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (title == NULL || !gfx_hits(0, GFX_H)) {
		return;
	}

	/* Dim the whole panel, not a band behind the card: a band leaves the
	 * playfield bright either side and the modal reads as a sticker. */
	gfx_rect(0, 0, GFX_W, GFX_H, t->bg_bot, 215);

	nexus_draw_card(OVER_X, OVER_Y, OVER_W, OVER_H);
	gfx_round_frame(OVER_X, OVER_Y, OVER_W, OVER_H, t->radius, t->accent,
			GFX_OPAQUE);

	/*
	 * Measured, then centred. Top-anchoring left the pause modal with a
	 * gap at the bottom and the game-over one pressed against the frame,
	 * so the two looked like different dialogs; the eye reads that as
	 * unfinished long before it reads any of the words.
	 */
	int title_h = gfx_text_h(NEXUS_TXT_VALUE);
	int hint_h = gfx_text_h(NEXUS_TXT_BODY);
	int block = title_h + 6 + 2;

	if (scores) {
		block += 14 + STAT_GAP;
	}
	block += 14;
	if (hint) {
		block += hint_h;
	}
	if (hint2) {
		block += HINT_GAP + hint_h;
	}

	int y = OVER_Y + (OVER_H - block) / 2;
	int tw = nexus_tracked_w(title, NEXUS_TXT_VALUE, 2);

	nexus_draw_tracked(GFX_W / 2, y, title, NEXUS_TXT_VALUE, 2, t->accent);
	y += title_h + 6;
	gfx_round_rect((GFX_W - tw) / 2, y, tw, 2, 1, t->accent, 150);
	y += 2 + 14;

	if (scores) {
		nexus_draw_caption_c(GFX_W / 2 - 48, y, "SCORE");
		nexus_draw_caption_c(GFX_W / 2 + 48, y, "BEST");
		gfx_text_c(GFX_W / 2 - 48, y + 12,
			   gfx_utoa(score, buf, sizeof(buf), 0),
			   NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
		gfx_text_c(GFX_W / 2 + 48, y + 12,
			   gfx_utoa(best, buf, sizeof(buf), 0),
			   NEXUS_TXT_BODY, t->accent, GFX_OPAQUE);
		y += 14 + STAT_GAP;
	}

	/*
	 * Both hints at the same size. They are two halves of one instruction
	 * - tap does this, hold does that - and printing one at twice the
	 * other made the second look like a footnote you were not meant to
	 * read, which is the opposite of what a modal explaining the only
	 * button needs to do. Rank is carried by colour instead.
	 */
	if (hint) {
		gfx_text_c(GFX_W / 2, y, hint, NEXUS_TXT_BODY, t->value,
			   GFX_OPAQUE);
		y += hint_h + HINT_GAP;
	}
	if (hint2) {
		gfx_text_c(GFX_W / 2, y, hint2, NEXUS_TXT_BODY, t->caption,
			   GFX_OPAQUE);
	}
}
