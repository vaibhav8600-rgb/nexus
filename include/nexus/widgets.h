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
#define NEXUS_TXT_CAPTION 1 /*  5x7  - LAYER, WPM, LEFT   */
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

/** Horizontal capsule meter, 0-100, with a rounded cap at low values. */
void nexus_draw_meter(int x, int y, int w, int h, uint8_t pct, gfx_color fill);

/** Rounded key-cap style indicator; filled with the accent when @p active. */
void nexus_draw_pill(int x, int y, int w, int h, bool active, const char *text);

/**
 * The extruded wordmark from the reference: four shadow steps drawn one pixel
 * lower each, then the face on top. Costs five text passes and no asset.
 */
void nexus_draw_wordmark(int cx, int y, const char *text, int scale);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_WIDGETS_H_ */
