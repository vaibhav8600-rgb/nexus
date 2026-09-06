/*
 * Theme tokens (Sections 57-58, 77-78, 103).
 *
 * Colour lives here and nowhere else, so retuning the look is one table and
 * never a hunt through screens.
 *
 * "Glass" here is real compositing, not a picture of glass: the ground is a
 * linear vertical gradient, and blurring a linear gradient returns the same
 * gradient - so an alpha tint drawn over it IS the correct frosted result,
 * with no blur pass and no second framebuffer. That is what Section 57 permits
 * and what an nRF52840 can actually afford.
 */
#ifndef NEXUS_THEME_H_
#define NEXUS_THEME_H_

#include <nexus/gfx.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** RGB888 literal -> RGB565, as a compile-time constant expression. */
#define NEXUS_C(v)                                                             \
	((gfx_color)((((v) >> 8) & 0xF800u) | (((v) >> 5) & 0x07E0u) |         \
		     (((v) >> 3) & 0x001Fu)))

struct nexus_theme {
	const char *name;

	/* Ground. */
	gfx_color bg_top;
	gfx_color bg_bot;
	gfx_color glow_a;   /* soft blob, top-left */
	gfx_color glow_b;   /* soft blob, bottom-right */
	uint8_t glow_alpha; /* 0 turns the blobs off entirely */

	/* Glass panes. */
	gfx_color panel;
	uint8_t panel_alpha;
	gfx_color edge_hi; /* specular top edge - this is what sells it */
	uint8_t edge_hi_alpha;
	gfx_color edge_lo; /* shaded bottom edge, for thickness */
	uint8_t edge_lo_alpha;
	gfx_color border;
	uint8_t border_alpha;

	/* Type and accents. */
	gfx_color caption;    /* small uppercase labels */
	gfx_color value;      /* numerals and primary text */
	gfx_color accent;     /* WPM, meters, selection */
	gfx_color accent_alt; /* active modifier pills */
	gfx_color muted;      /* inactive glyphs */
	gfx_color track;      /* meter trough, game well */
	gfx_color success;
	gfx_color warning;
	gfx_color error;

	/*
	 * The extruded wordmark, brightest to darkest:
	 *   [0] unused (each theme's ground, kept for palette symmetry)
	 *   [1] the bright outline edge
	 *   [2] face, top       [3] face, bottom
	 *   [4] the extrusion
	 * The signature element of the reference design.
	 */
	gfx_color wordmark[5];

	uint8_t radius; /* card corner radius */
};

const struct nexus_theme *nexus_theme(void);
int nexus_theme_set(const char *name);
/** Index of the active theme, for persisting a choice across reboots. */
uint8_t nexus_theme_index(void);
int nexus_theme_set_index(uint8_t index);
/** Step through the palettes, wrapping. @p delta is normally +1 or -1. */
void nexus_theme_cycle(int delta);
const char *nexus_theme_name_at(uint8_t index);
uint8_t nexus_theme_count(void);

/* Shared layout metrics, so screens agree without copying numbers around. */
#define NEXUS_PAD 9 /* screen edge padding */
#define NEXUS_GAP 7 /* between panels */
#define NEXUS_CONTENT_W (GFX_W - 2 * NEXUS_PAD)

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_THEME_H_ */
