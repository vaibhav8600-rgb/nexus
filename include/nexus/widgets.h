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

/** Horizontal capsule meter, 0-100, with a rounded cap at low values. */
void nexus_draw_meter(int x, int y, int w, int h, uint8_t pct, gfx_color fill);


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
