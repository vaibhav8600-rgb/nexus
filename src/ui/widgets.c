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

/*
 * The level badge.
 *
 * A caption "L" and the number at body size, on the accent, right-aligned.
 * Small on purpose: it is a thing you glance at between rounds, not something
 * you track while playing, and the games it sits in have already spent their
 * HUD on the score.
 */
int nexus_draw_level(int right, int y, uint8_t level)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[8];
	const char *num = gfx_utoa(level, buf, sizeof(buf), 0);
	int lw = gfx_text_w("L", NEXUS_TXT_BODY);
	int x = right - lw - gfx_text_w(num, NEXUS_TXT_BODY);

	/*
	 * One size, two colours. A caption-sized L beside a body numeral has
	 * to be baseline-aligned to not look dropped, and at 7px against 14px
	 * it still reads as a smudge; the same size in the caption colour says
	 * "label" just as clearly and takes one less measurement to place.
	 */
	gfx_text(x, y, "L", NEXUS_TXT_BODY, t->caption, GFX_OPAQUE);
	gfx_text(x + lw, y, num, NEXUS_TXT_BODY, t->accent, GFX_OPAQUE);

	return x;
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

/* ---- the shared solid --------------------------------------------------- */

/*
 * One light source, top-left, for the whole product.
 *
 * Every game used to shade its own pieces its own way - Tetris bevelled,
 * Snake had a single highlight line, the maze had nothing - and five games
 * lit five different ways is what makes a collection look assembled rather
 * than designed. These two functions are the entire vocabulary now: anything
 * solid and square goes through nexus_draw_block(), anything solid and round
 * through nexus_draw_orb(), and they agree about where the light is.
 *
 * The cost is four extra ops per piece over a flat rect. At the sizes these
 * games draw - a 16px cell, a 10px ball - that is nothing next to the blend
 * the fill itself already does.
 */
#define SHADOW NEXUS_C(0x000000u)
#define SPECULAR NEXUS_C(0xFFFFFFu)

void nexus_draw_block(int x, int y, int w, int h, int r, gfx_color c)
{
	if (w <= 0 || h <= 0) {
		return;
	}

	/* Contact shadow. This is what puts the piece ON the board instead of
	 * in it, and it is the single cheapest thing that reads as depth. */
	gfx_round_rect(x + 2, y + 2, w, h, r, SHADOW, 80);

	gfx_round_rect(x, y, w, h, r, c, GFX_OPAQUE);

	/* Light pooling down from the top edge, three steps. A real vertical
	 * gradient would mean a per-row blend; three bands are visually the
	 * same thing at this size for a fraction of the work. */
	int inset = r > 1 ? r : 1;

	for (int i = 0; i < 3 && i < h / 2; i++) {
		gfx_rect(x + inset, y + 1 + i, w - 2 * inset, 1, SPECULAR,
			 (uint8_t)(64 - i * 18));
	}

	/* The bevel: lit top and left, shaded bottom and right. */
	gfx_hline(x + inset, y, w - 2 * inset, SPECULAR, 120);
	gfx_vline(x, y + inset, h - 2 * inset, SPECULAR, 80);
	gfx_hline(x + inset, y + h - 1, w - 2 * inset, SHADOW, 120);
	gfx_vline(x + w - 1, y + inset, h - 2 * inset, SHADOW, 100);
}

void nexus_draw_orb(int cx, int cy, int r, gfx_color c)
{
	if (r <= 0) {
		return;
	}

	gfx_disc(cx + 1, cy + 2, r, SHADOW, 80);
	gfx_disc(cx, cy, r, c, GFX_OPAQUE);

	/*
	 * Specular up and to the left, matching the block. One disc at a third
	 * the radius is enough to turn a flat circle into a sphere - the eye
	 * needs the highlight to be off-centre far more than it needs it to be
	 * shaped correctly.
	 */
	if (r >= 3) {
		gfx_disc(cx - r / 3, cy - r / 3, r / 3, SPECULAR, 150);
	}
	/* A darker rim on the far side finishes the roundness. */
	if (r >= 4) {
		gfx_disc(cx + r / 3, cy + r / 3, r / 3, SHADOW, 45);
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

/*
 * Nothing under the name.
 *
 * It had an accent rule, which read as an underline drawn through the
 * composition, and then a tracked strapline, which is a second line of type in
 * a 50px card that already carries the title. The brand plate holds one thing
 * and the space around it is the design. The splash still sets the strapline,
 * because a splash has 240px of height to spend and a reason to introduce
 * itself; the home screen does not.
 */
#define WORDMARK_GLOW_CELLS 2
#define WORDMARK_GLOW_ALPHA 70

/*
 * The wordmark, made of the same glass as the cards under it.
 *
 * It has been through two wrong answers. Flat glyphs with a soft drop shadow
 * read as a caption set large. Blocky lettering with a hard accent outline and
 * an extrusion read as an arcade sticker - the one element on a screen of
 * frosted panes not made of the same material, which is exactly why it looked
 * cheap next to them.
 *
 * Glass takes its presence from light. The accent comes off the letter and
 * goes behind it as a soft halo; the letter itself is near-white with a
 * one-pixel specular above every edge and a shade below, which is the same
 * treatment nexus_draw_card() gives a pane. The title now looks cut from the
 * card it sits on rather than stuck to it.
 *
 * The theme's wordmark[] ramp, brightest to darkest: [0]/[1] shade the face,
 * [2] is the halo, [3] and [4] the two graded pixels of shade under every
 * edge. All five earn their place.
 */
/* Face plus the glow, which reaches two letter-pixels past it every way. */
static int wordmark_ink_h(int scale)
{
	int s = scale < 1 ? 1 : scale;

	return gfx_face_h(s) + 2 * WORDMARK_GLOW_CELLS * s;
}

int nexus_wordmark_h(int scale)
{
	return wordmark_ink_h(scale);
}

static void wordmark_letters(int cx, int y, const char *text, int scale,
			     int lit)
{
	const struct nexus_theme *t = nexus_theme();
	int s = scale < 1 ? 1 : scale;
	int w = gfx_face_w(text, s);
	int pen = cx - w / 2;
	char one[2] = { 0, 0 };

	/* y is the top of the INK and the glow owns the cells above the face.
	 * Callers centre with nexus_wordmark_h(), so the halo stays inside the
	 * box they measured rather than bleeding out of the card. */
	int fy = y + WORDMARK_GLOW_CELLS * s;

	for (const char *p = text; p && *p; p++) {
		gfx_color top = t->wordmark[0];
		gfx_color bot = t->wordmark[1];
		uint8_t glow = WORDMARK_GLOW_ALPHA;

		if (lit >= 0 && (p - text) == lit) {
			/* The splash walks this along the word: the lit letter
			 * goes flat white and its halo comes up. */
			top = bot = t->value;
			glow = 255;
		}

		one[0] = *p;
		gfx_face_text_glass(pen, fy, one, s, top, bot, t->edge_hi,
				    t->wordmark[3], t->wordmark[4],
				    t->wordmark[2], glow);
		pen += gfx_face_w(one, s) + s;
	}
}

void nexus_draw_wordmark_lit(int cx, int y, const char *text, int scale,
			     int lit)
{
	wordmark_letters(cx, y, text, scale, lit);
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
