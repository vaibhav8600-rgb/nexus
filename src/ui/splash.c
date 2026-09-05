/*
 * Boot splash (Sections 18-22).
 *
 * The artwork and all three text lines come from the integrator's config:
 * CONFIG_NEXUS_SPLASH_IMAGE points at a PNG in their own repo, which the build
 * converts to RGB565 (see scripts/png2c.py). Nothing here is branded and no
 * user ever edits NEXUS source to change it (Requirements B and C).
 *
 * The splash is cosmetic only. ZMK finishes booting behind it and the keyboard
 * is usable the moment the radio is up, not when this timer expires
 * (Section 18).
 */

#include <nexus/gfx.h>
#include <nexus/nexus.h>
#include <nexus/screen.h>
#include <nexus/sound.h>
#include <nexus/splash.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>

/*
 * Badge layout, in the supplied design's own coordinates. The disc occupies
 * 24..120; these are the three lines below it.
 */
#define BRAND_Y 132
#define WORD_Y 152
#define WORD_SCALE 2
#define SUB_Y 192

#define SPLASH_COL_BRAND NEXUS_C(0x565D85u)
#define SPLASH_COL_PRODUCT NEXUS_C(0x7C87C4u)

#define MARK_GAP 12
#define BRAND_GAP 10   /* below the mark, above the wordmark */
#define SUB_GAP 10     /* below the wordmark                 */

/*
 * The old measure-then-centre helpers (brand_h, sub_h, mark_scale, block_h)
 * are gone with the flow layout they served. The badge is a fixed
 * composition; see splash_draw().
 */

/* Which letter the highlight is on. One integer of animation state. */
static int8_t g_lit;

int nexus_splash_phase(void)
{
	return g_lit;
}

static void splash_draw(void)
{
	const struct nexus_splash_art *art = &nexus_splash_art;

	/*
	 * Fixed positions, not a centred flow.
	 *
	 * The badge composition is a designed layout: the disc sits at 24..120
	 * and the three lines hang off it at 132 / 152 / 192. Re-centring the
	 * block would slide the text relative to the disc whenever a string
	 * length changed, which is exactly what the design does not want. The
	 * artwork paints the ground and everything above the text.
	 */
	if (art->h) {
		art->draw(0, 0);
	}

	if (sizeof(NEXUS_BRAND) > 1) {
		nexus_draw_tracked(GFX_W / 2, BRAND_Y, NEXUS_BRAND,
				   NEXUS_TXT_BODY, 2, SPLASH_COL_BRAND);
	}

	/*
	 * The wordmark in the display face, per-letter ramp, with the sweep on
	 * top. No accent rule under it - the disc above is the mark, and a
	 * second graphic element under the name made the badge read as two
	 * logos stacked.
	 */
	nexus_draw_wordmark_plain(GFX_W / 2, WORD_Y, NEXUS_PRODUCT,
				  WORD_SCALE, g_lit);

	if (sizeof(NEXUS_SUBTITLE) > 1) {
		nexus_draw_tracked(GFX_W / 2, SUB_Y, NEXUS_SUBTITLE,
				   NEXUS_TXT_BODY, 1, SPLASH_COL_PRODUCT);
	}
}

/* Sweep the highlight along the name, then pause on the far side so it reads
 * as a shimmer rather than a spinner. */
static void splash_tick(void)
{
	int n = (int)sizeof(NEXUS_PRODUCT) - 1;

	if (n < 1) {
		return;
	}
	g_lit = (int8_t)((g_lit + 1) % (n + 3));
	nexus_screen_invalidate();
}

static void splash_enter(void)
{
	g_lit = -1;

	if (IS_ENABLED(CONFIG_NEXUS_SOUND_STARTUP)) {
		nexus_sound_play(NEXUS_SOUND_STARTUP);
	}
}

static bool splash_action(enum nexus_action action)
{
	ARG_UNUSED(action);

	/* Any press skips the wait rather than doing nothing for four seconds.
	 * screen.c checks that the splash is still up before its timer fires,
	 * so this cannot be undone a moment later. */
	nexus_screen_replace(&nexus_screen_home_def);
	return true;
}

const struct nexus_screen nexus_screen_splash_def = {
	.name = "SPLASH",
	.enter = splash_enter,
	.draw = splash_draw,
	.action = splash_action,
	.tick = splash_tick,
	.refresh = NEXUS_REFRESH_NORMAL,
	.btn_short = NEXUS_ACTION_SELECT,
	.btn_long = NEXUS_ACTION_SELECT,
};
