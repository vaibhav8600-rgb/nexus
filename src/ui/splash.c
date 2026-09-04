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

#define MARK_GAP 12
#define BRAND_GAP 10   /* below the mark, above the wordmark */
#define SUB_GAP 10     /* below the wordmark                 */

/*
 * Everything except the wordmark used to be caption-sized, so on hardware the
 * brand and subtitle simply did not register - the splash read as "logo, then
 * NEXUS, then two smudges". They are body size now, and the block is measured
 * before it is drawn so the whole thing stays centred whatever the strings and
 * the artwork add up to.
 */
static int brand_h(void)
{
	return sizeof(NEXUS_BRAND) > 1
		       ? gfx_text_h(NEXUS_TXT_BODY) + BRAND_GAP
		       : 0;
}

static int sub_h(void)
{
	return sizeof(NEXUS_SUBTITLE) > 1
		       ? gfx_text_h(NEXUS_TXT_BODY) + SUB_GAP
		       : 0;
}

/* Wordmark scale that fits the panel width, and the whole block's height. */
static int mark_scale(void)
{
	for (int sc = 5; sc > 1; sc--) {
		if (gfx_text_w(NEXUS_PRODUCT, sc) <= GFX_W - 2 * NEXUS_PAD - 8) {
			return sc;
		}
	}
	return 1;
}

static int block_h(const struct nexus_splash_art *art)
{
	return (art->h ? art->h + MARK_GAP : 0) + brand_h() +
	       nexus_wordmark_h(mark_scale()) + sub_h();
}

static void splash_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	const struct nexus_splash_art *art = &nexus_splash_art;
	int scale = mark_scale();
	int y = (GFX_H - block_h(art)) / 2;

	if (y < NEXUS_PAD) {
		y = NEXUS_PAD;
	}

	if (art->h) {
		art->draw((GFX_W - art->w) / 2, y);
		y += art->h + MARK_GAP;
	}

	if (sizeof(NEXUS_BRAND) > 1) {
		gfx_text_c(GFX_W / 2, y, NEXUS_BRAND, NEXUS_TXT_BODY,
			   t->caption, GFX_OPAQUE);
		y += gfx_text_h(NEXUS_TXT_BODY) + BRAND_GAP;
	}

	nexus_draw_wordmark(GFX_W / 2, y, NEXUS_PRODUCT, scale);
	y += nexus_wordmark_h(scale);

	if (sizeof(NEXUS_SUBTITLE) > 1) {
		gfx_text_c(GFX_W / 2, y + SUB_GAP, NEXUS_SUBTITLE,
			   NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
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
