/*
 * Shared glass / neumorphic building blocks (Sections 57-58).
 *
 * These are draw calls, not object constructors: the compositor repaints from
 * the model every time, so there is no widget tree to keep in sync with the
 * status struct, nothing to free on a screen change, and no per-widget RAM.
 * Keeping every card in one place is what stops seven screens drifting apart.
 *
 * All of them clip to the live band, so calling them for a card that is
 * off-band costs a compare (Section 61).
 */
#ifndef NEXUS_WIDGETS_H_
#define NEXUS_WIDGETS_H_

#include <nexus/gfx.h>
#include <nexus/theme.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type sizes, as multiples of the 5x7 cell. Section 103 wants big numerals and
 * compact uppercase labels; these four cover the whole UI. */
#define NEXUS_TXT_CAPTION 1 /*  5x7  - dense lists, hints */
#define NEXUS_TXT_LABEL 2   /* 10x14 - LAYER, WPM, LEFT   */
#define NEXUS_TXT_BODY 2    /* 10x14 - values, menu rows  */
#define NEXUS_TXT_VALUE 3   /* 15x21 - headline values    */
#define NEXUS_TXT_BIG 4     /* 20x28 - battery percentage */

/** Gradient ground plus the two soft corner glows. Draw this first. */
void nexus_draw_ground(void);

/** Translucent rounded pane: tint, light pooling, lit top and shaded bottom. */
void nexus_draw_card(int x, int y, int w, int h);

/** As nexus_draw_card, plus an accent outline when @p selected. */
void nexus_draw_card_sel(int x, int y, int w, int h, bool selected);

/** Small uppercase caption in the theme caption colour. */
void nexus_draw_caption(int x, int y, const char *text);
void nexus_draw_caption_c(int cx, int y, const char *text);

/**
 * A card's label, in the caption colour but at NEXUS_TXT_LABEL.
 *
 * Separate from nexus_draw_caption() rather than a scale argument on it:
 * captions and labels want different sizes in different places, and the two
 * dozen existing caption call sites are all correct as they stand. Labels on
 * the dashboard are read across a desk; hints in a dense list are not.
 */
void nexus_draw_label(int x, int y, const char *text);

/**
 * The level badge every game's HUD carries, drawn from its right edge at
 * @p right so a two-digit level grows leftwards instead of into the score.
 *
 * One widget rather than seven hand-placed pairs of gfx_text calls: the games
 * disagree about everything else in their HUDs - lives, WALLS/WRAP, two
 * scores - and the one number they all now have should at least look the same
 * in all of them.
 *
 * @return the x it started drawing at, so a caller can place something to the
 *         left of it without measuring the text itself.
 */
int nexus_draw_level(int right, int y, uint8_t level);

/*
 * The header block every game draws, and the rect it owns.
 *
 * Seven games had seven headers. Snake and Pac-Man labelled the score,
 * Breakout and Invaders showed a bare number, Tetris put it in a side panel
 * entirely - so paging between them moved the furniture and the set read as
 * seven programs rather than one product with seven games.
 *
 * The rect is fixed. NEXUS_HUD_END is where it stops and the earliest a
 * playfield may begin, which is what Tetris's well had to move down to clear.
 * NEXUS_HUD_ROW is the part that changes while you are playing, so a score
 * that ticks over invalidates fourteen rows rather than the whole header.
 */
/*
 * 34, and the two rows inside it are tight against each other, because Tetris
 * decides this number rather than taste: its well is 20 rows of 10px plus a
 * 2px frame either side, which is 204 of the panel's 240 and leaves exactly
 * 36. At 36 the score sat flush on the well's top edge with no air at all, so
 * the header gives back two pixels and takes them out of its own margins.
 */
#define NEXUS_HUD_END 34
#define NEXUS_HUD_TITLE_Y 6
#define NEXUS_HUD_ROW_Y 20
#define NEXUS_HUD_ROW_H 14

struct nexus_hud {
	const char *title;
	uint32_t score;
	/* An opponent's score beside yours - Pong, and nothing else so far.
	 * Negative for none, so 0-0 is still a scoreline. */
	int32_t rival;
	/* One word of game-specific state: Snake's WRAP/WALLS. Right-aligned
	 * before the lives, because it is read once at a glance and then
	 * ignored for the rest of the round. */
	const char *note;
	uint8_t level; /* 0 draws no badge */
	uint8_t lives; /* 0 draws no pips */
	gfx_color life;
};

/**
 * Title, HOLD=EXIT, score, level and lives, in the same place in every game.
 *
 * Everything but the title and the score is optional and simply absent when
 * it does not apply - a game with no lives does not get an empty row where
 * the pips would be.
 */
void nexus_draw_game_header(const struct nexus_hud *h);

/**
 * A playfield's ground: flat and opaque, deliberately not frosted.
 *
 * Everything else on this device is a translucent pane over a gradient with
 * two soft colour blobs behind it, and that is right for a dashboard - it is
 * wrong behind a game. The blob edge is a smooth curve crossing the play
 * area, which on a 240px panel reads as a tear in the image rather than as
 * decoration, and everywhere it does not tear it just lowers contrast on the
 * things you are trying to track.
 *
 * The decoration stays on the ground, the cards, the menus and the game
 * headers. It stops at the field border.
 */
void nexus_draw_field(int x, int y, int w, int h);

/** Horizontal capsule meter, 0-100, with a rounded cap at low values. */
void nexus_draw_meter(int x, int y, int w, int h, uint8_t pct, gfx_color fill);

/**
 * The shared solid: a raised block with a contact shadow, light pooling at the
 * top, a lit top-left bevel and a shaded bottom-right one.
 *
 * Every game draws anything solid through this, which is what makes five very
 * different games look like one product. Doing it per-game is how you end up
 * with a maze that looks nothing like a brick wall.
 *
 * @param r corner radius; 0 is a hard square, and squares tile seamlessly
 *          where rounded ones leave gaps between neighbours.
 */
void nexus_draw_block(int x, int y, int w, int h, int r, gfx_color c);

/**
 * The shared round solid: a lit sphere with a contact shadow and a specular
 * highlight up and to the left, matching the block's light direction.
 */
void nexus_draw_orb(int cx, int cy, int r, gfx_color c);


/**
 * Letter-spaced text, centred. Tracking is what makes a small uppercase label
 * read as a label rather than a cramped word - the reference leans on it hard.
 */
void nexus_draw_tracked(int cx, int y, const char *text, int scale, int track,
			gfx_color c);
int nexus_tracked_w(const char *text, int scale, int track);

/**
 * The product wordmark: a weighted face over a soft drop shadow, with an
 * accent rule beneath.
 *
 * The four-step extrusion this replaces was a poor trade at 240x240 - five
 * offset copies of a 5x7 face turn into mush at any size big enough to read,
 * and the "depth" just cost contrast. Stamping the glyphs 2x2 thickens every
 * stroke instead, which is what actually makes a bitmap face look bold, and
 * the rule under it does the job the extrusion was there for.
 */
void nexus_draw_wordmark(int cx, int y, const char *text, int scale);

/**
 * The wordmark without its accent rule.
 *
 * The rule is what makes the name read as a product lockup on its own. On the
 * badge splash the disc above IS the mark, and an underline as well made it
 * two logos stacked, so that layout asks for the letters only.
 */
void nexus_draw_wordmark_plain(int cx, int y, const char *text, int scale,
			       int lit);

/**
 * The pause / game-over modal every game shares.
 *
 * Dims the whole panel, then draws a centred card: a tracked title over an
 * accent rule, an optional SCORE / BEST row, and up to two hint lines at the
 * same size ranked by colour.
 *
 * Lives here rather than in a game because the alternative is three copies
 * that drift: the composition was tuned once (measured then centred, both
 * hints one size) and every game should get that, not just the one it was
 * tuned in.
 *
 * @param title    NULL draws nothing at all - that is the running state.
 * @param hint     primary instruction, in the value colour.
 * @param hint2    secondary, in the caption colour. May be NULL.
 * @param scores   draw the SCORE / BEST row.
 */
void nexus_draw_game_overlay(const char *title, const char *hint,
			     const char *hint2, bool scores, uint32_t score,
			     uint32_t best);

/**
 * Wordmark with a lit letter, for the splash.
 *
 * @param lit index of the letter to highlight, or negative for none. Advance
 *            it on a tick and the highlight sweeps along the word - one
 *            integer of animation state, no buffers, no easing tables.
 */
void nexus_draw_wordmark_lit(int cx, int y, const char *text, int scale,
			     int lit);

/** Total height of a wordmark, including its rule. */
int nexus_wordmark_h(int scale);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_WIDGETS_H_ */
