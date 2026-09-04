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

#define MARK_GAP 10
#define BRAND_H 13 /* caption + its gap */
#define SUB_H 13

static int text_height(void)
{
	int h = gfx_text_h(NEXUS_TXT_BIG);

	if (sizeof(NEXUS_BRAND) > 1) {
		h += BRAND_H;
	}
	if (sizeof(NEXUS_SUBTITLE) > 1) {
		h += SUB_H;
	}
	return h;
}

static void splash_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	const struct nexus_splash_art *art = &nexus_splash_art;
	int block = art->h + (art->h ? MARK_GAP : 0) + text_height();
	int y = (GFX_H - block) / 2;

	if (y < 0) {
		y = 0;
	}

	if (art->h) {
		art->draw((GFX_W - art->w) / 2, y);
		y += art->h + MARK_GAP;
	}

	if (sizeof(NEXUS_BRAND) > 1) {
		nexus_draw_caption_c(GFX_W / 2, y, NEXUS_BRAND);
		y += BRAND_H;
	}

	nexus_draw_wordmark(GFX_W / 2, y, NEXUS_PRODUCT, NEXUS_TXT_BIG);
	y += gfx_text_h(NEXUS_TXT_BIG);

	if (sizeof(NEXUS_SUBTITLE) > 1) {
		gfx_text_c(GFX_W / 2, y + 6, NEXUS_SUBTITLE, NEXUS_TXT_CAPTION,
			   t->caption, GFX_OPAQUE);
	}
}

static void splash_enter(void)
{
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
	.refresh = NEXUS_REFRESH_IDLE,
	.btn_short = NEXUS_ACTION_SELECT,
	.btn_long = NEXUS_ACTION_SELECT,
};
