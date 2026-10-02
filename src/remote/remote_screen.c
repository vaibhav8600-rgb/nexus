/*
 * Remote Input on the NEXUS screen: the pairing window, the passkey, the
 * outcome (PAIRED, CONNECTED for a paired phone back, or NO ROOM), and the
 * "this is the one" flash for Control 0x05.
 *
 * One screen with several views rather than several screens, because they
 * are one flow - window opens, phone asks, number appears, done - and each
 * step replaces the last in place instead of stacking.
 *
 * Requests come from Bluetooth callbacks, so show/hide only record what is
 * wanted and hand the screen work to the NEXUS queue.
 */

#include <nexus/nexus.h>
#include <nexus/remote.h>
#include <nexus/sound.h>
#include <zephyr/kernel.h>

#include "../nexus_priv.h"
#include "remote_priv.h"

#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)

#include <nexus/gfx.h>
#include <nexus/screen.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>

#define HELLO_MS 1500

#define CARD_Y 34
#define CARD_H 172
#define BIG_Y 84 /* the passkey or the countdown, 42px at scale 3 */

static enum nexus_remote_view g_view;
static uint32_t g_passkey;
static bool g_want;
static int64_t g_hello_until;
static uint32_t g_drawn_s;

/* Six digits, zero-padded: the phone asks for exactly six. */
static void big_number(const char *s, gfx_color c)
{
	int scale = 3;

	while (scale > 1 && gfx_face_w(s, scale) > NEXUS_CONTENT_W - 16) {
		scale--;
	}
	gfx_face_text((GFX_W - gfx_face_w(s, scale)) / 2, BIG_Y, s, scale, c,
		      GFX_OPAQUE);
}

static void draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (g_view == NEXUS_REMOTE_VIEW_HELLO) {
		/* The flash: the whole panel washed in the accent, so the
		 * right dongle is obvious from across the room. */
		gfx_rect(0, 0, GFX_W, GFX_H, t->accent, 110);
		nexus_draw_wordmark(GFX_W / 2, 88, NEXUS_PRODUCT, 2);
		nexus_draw_caption_c(GFX_W / 2, 150, "REMOTE");
		return;
	}

	if (g_view == NEXUS_REMOTE_VIEW_FULL) {
		nexus_draw_card(NEXUS_PAD, CARD_Y, NEXUS_CONTENT_W, CARD_H);
		nexus_draw_caption_c(GFX_W / 2, CARD_Y + 12, "PAIR A PHONE");
		gfx_text_c(GFX_W / 2, BIG_Y, "NO ROOM", NEXUS_TXT_VALUE,
			   t->warning, GFX_OPAQUE);
		nexus_draw_caption_c(GFX_W / 2, 140, "FORGET A PHONE, OR FREE");
		nexus_draw_caption_c(GFX_W / 2, 152, "A BLUETOOTH PROFILE");
		return;
	}

	if (g_view == NEXUS_REMOTE_VIEW_PAIRED ||
	    g_view == NEXUS_REMOTE_VIEW_CONNECTED) {
		bool paired = g_view == NEXUS_REMOTE_VIEW_PAIRED;

		nexus_draw_card(NEXUS_PAD, CARD_Y, NEXUS_CONTENT_W, CARD_H);
		nexus_draw_caption_c(GFX_W / 2, CARD_Y + 12, "PAIR A PHONE");
		gfx_text_c(GFX_W / 2, BIG_Y, paired ? "PAIRED" : "CONNECTED",
			   NEXUS_TXT_VALUE, t->success, GFX_OPAQUE);
		nexus_draw_caption_c(GFX_W / 2, 146, "THE PHONE IS READY");
		return;
	}

	nexus_draw_card(NEXUS_PAD, CARD_Y, NEXUS_CONTENT_W, CARD_H);
	nexus_draw_caption_c(GFX_W / 2, CARD_Y + 12, "PAIR A PHONE");

	if (g_view == NEXUS_REMOTE_VIEW_PASSKEY) {
		big_number(gfx_utoa(g_passkey, buf, sizeof(buf), 6), t->accent);
		nexus_draw_caption_c(GFX_W / 2, 146, "TYPE THIS ON THE PHONE");
	} else {
		g_drawn_s = nexus_remote_pair_remaining_s();
		big_number(gfx_utoa(g_drawn_s, buf, sizeof(buf), 0), t->value);
		bool again = g_passkey == NEXUS_REMOTE_WAIT_AGAIN;

		nexus_draw_caption_c(GFX_W / 2, 140, again ? "OLD PAIRING CLEARED"
							  : "OPEN THE NEXUS APP");
		nexus_draw_caption_c(GFX_W / 2, 152, again ? "TAP CONNECT AGAIN"
							  : "AND TAP CONNECT");
	}

	gfx_text_c(GFX_W / 2, CARD_Y + CARD_H - 18, "PRESS TO CANCEL",
		   NEXUS_TXT_CAPTION, t->muted, GFX_OPAQUE);
}

/* Views that say something and go, rather than wait for an answer. */
static bool transient(void)
{
	return g_view == NEXUS_REMOTE_VIEW_HELLO ||
	       g_view == NEXUS_REMOTE_VIEW_FULL ||
	       g_view == NEXUS_REMOTE_VIEW_PAIRED ||
	       g_view == NEXUS_REMOTE_VIEW_CONNECTED;
}

static void tick(void)
{
	if (transient()) {
		if (k_uptime_get() >= g_hello_until) {
			g_want = false;
			nexus_screen_pop();
		}
	} else if (g_view == NEXUS_REMOTE_VIEW_WAIT &&
		   nexus_remote_pair_remaining_s() != g_drawn_s) {
		nexus_screen_invalidate_rows(BIG_Y, BIG_Y + 42);
	}
}

static bool action(enum nexus_action a)
{
	if (a != NEXUS_ACTION_BACK && a != NEXUS_ACTION_SELECT) {
		return false;
	}
	if (!transient()) {
		remote_pair_cancel();
	}
	g_want = false;
	nexus_sound_play(NEXUS_SOUND_BACK);
	nexus_screen_pop();
	return true;
}

/* Gone is gone: covered or popped, a later request pushes a fresh one
 * rather than finding a stale one underneath. */
static void screen_exit(void)
{
	g_want = false;
}

static const struct nexus_screen k_def = {
	.name = "REMOTE",
	.exit = screen_exit,
	.draw = draw,
	.action = action,
	.tick = tick,
	.refresh = NEXUS_REFRESH_NORMAL,
	.btn_short = NEXUS_ACTION_BACK,
	.btn_long = NEXUS_ACTION_BACK,
};

static void apply_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	bool open = nexus_screen_current() == &k_def;

	if (g_want && !open) {
		nexus_screen_push(&k_def);
	} else if (g_want) {
		nexus_screen_invalidate();
	} else if (open) {
		nexus_screen_pop();
	}
}
static K_WORK_DEFINE(g_apply, apply_fn);

void nexus_remote_screen_show(enum nexus_remote_view view, uint32_t passkey)
{
	g_view = view;
	g_passkey = passkey;
	/* Words to read get longer than the identify flash. */
	g_hello_until = k_uptime_get() +
			(view == NEXUS_REMOTE_VIEW_HELLO ? HELLO_MS : 2500);
	g_want = true;
	k_work_submit_to_queue(nexus_workq(), &g_apply);
}

void nexus_remote_screen_hide(enum nexus_remote_view view)
{
	if (g_view == view) {
		g_want = false;
		k_work_submit_to_queue(nexus_workq(), &g_apply);
	}
}

#else /* no display: nothing to show, and pairing works regardless */

void nexus_remote_screen_show(enum nexus_remote_view view, uint32_t passkey)
{
	ARG_UNUSED(view);
	ARG_UNUSED(passkey);
}

void nexus_remote_screen_hide(enum nexus_remote_view view)
{
	ARG_UNUSED(view);
}

#endif /* CONFIG_NEXUS_DISPLAY */
