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
	 * The artwork is the entire splash. This function does not draw one
	 * pixel of its own.
	 *
	 * The brand, product and subtitle lines used to be drawn here, over
	 * whatever the artwork had painted. That is correct for the built-in
	 * badge, where the type belongs to the composition - and wrong for
	 * every supplied image, which is normally a finished design with its
	 * own lettering already in it. The result was NEXUS printing its
	 * wordmark across somebody's artwork.
	 *
	 * A Kconfig switch would have papered over it. This is structural
	 * instead: the badge draws its own text (assets/splash_default.c), a
	 * converted PNG draws only its pixels (scripts/png2c.py), and there is
	 * no setting anybody can get wrong. "Nothing is painted over a custom
	 * splash" stops being a promise and becomes a property of the code.
	 */
	gfx_rect(0, 0, GFX_W, GFX_H, nexus_theme()->bg_bot, GFX_OPAQUE);

	if (art->h) {
		art->draw((GFX_W - art->w) / 2, (GFX_H - art->h) / 2);
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
