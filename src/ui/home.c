/*
 * Home dashboard (Sections 23-25, 61, 103-104).
 *
 * Layout follows the supplied reference: a wordmark plate, two rows of paired
 * glass cards, and the two battery cards along the bottom.
 *
 * The status callback does not draw. It marks the row range that changed and
 * lets the compositor repaint exactly those bands, so a WPM tick costs four
 * bands over SPI (23 KB) instead of the whole panel (115 KB). That is the
 * entire point of Section 61, and it is why every panel here has a Y constant.
 */

#include <nexus/gfx.h>
#include <nexus/nexus.h>
#include <nexus/screen.h>
#include <nexus/sound.h>
#include <nexus/status.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>

#include "../nexus_priv.h"

/* ---- layout ------------------------------------------------------------ */
/* 240 wide, 9px margins, 7px gutters: two 107/108px columns. */
#define COL_L NEXUS_PAD                          /*   9 */
#define COL_W 107
#define COL_R (COL_L + COL_W + NEXUS_GAP)        /* 123 */
#define COL_RW (GFX_W - NEXUS_PAD - COL_R)       /* 108 */

#define BRAND_Y NEXUS_PAD                        /*   9 */
#define BRAND_H 56
#define ROW1_Y (BRAND_Y + BRAND_H + NEXUS_GAP)   /*  72 */
#define ROW1_H 38
#define ROW2_Y (ROW1_Y + ROW1_H + NEXUS_GAP)     /* 117 */
#define ROW2_H 38
#define BAT_Y (ROW2_Y + ROW2_H + NEXUS_GAP)      /* 162 */
#define BAT_H 69                                 /* ends at 231 */

#define INNER 8 /* padding inside a card */

static const uint8_t mod_bits[4] = {
	NEXUS_MOD_CTRL, NEXUS_MOD_SHIFT, NEXUS_MOD_ALT, NEXUS_MOD_GUI
};
/* The symbols printed on the keys themselves. "C S A G" needed a legend;
 * a caret, an up arrow, an option stroke and four panes do not. */
static const enum gfx_icon mod_icons[4] = {
	GFX_ICON_CTRL, GFX_ICON_SHIFT, GFX_ICON_ALT, GFX_ICON_GUI
};

/* ---- drawing ----------------------------------------------------------- */

/*
 * Layer names come from the user's keymap, so they can be any length and NEXUS
 * must not assume otherwise (Section 28). Drop a size rather than clip: seven
 * characters fit at body size, fifteen at caption size, and beyond that the
 * name is truncated where it would leave the card.
 */
static int fit_scale(const char *s, int avail, int max_scale)
{
	for (int scale = max_scale; scale > 1; scale--) {
		if (gfx_text_w(s, scale) <= avail) {
			return scale;
		}
	}
	return 1;
}

static gfx_color batt_color(const struct nexus_theme *t, uint8_t pct)
{
	switch (nexus_battery_band(pct)) {
	case NEXUS_BATT_CRITICAL:
		return t->error;
	case NEXUS_BATT_LOW:
		return t->warning;
	case NEXUS_BATT_UNKNOWN:
		return t->muted;
	default:
		return t->accent;
	}
}

static void draw_brand(void)
{
	nexus_draw_card(COL_L, BRAND_Y, NEXUS_CONTENT_W, BRAND_H);

	/*
	 * Scale 5 where it fits: at NEXUS_TXT_BIG the wordmark was a caption
	 * with a shadow rather than the product's name. Five characters come
	 * to 145x35, which fills the plate the way the reference does, and
	 * fit_scale() still steps down for a longer CONFIG_NEXUS_PRODUCT.
	 */
	int scale = fit_scale(NEXUS_PRODUCT, NEXUS_CONTENT_W - 2 * INNER - 4, 5);

	nexus_draw_wordmark(GFX_W / 2,
			    BRAND_Y + (BRAND_H - nexus_wordmark_h(scale)) / 2,
			    NEXUS_PRODUCT, scale);
}

static void draw_link(const struct nexus_status *st)
{
	const struct nexus_theme *t = nexus_theme();
	bool on_usb = (st->endpoint == NEXUS_ENDPOINT_USB);
	bool on_ble = (st->endpoint == NEXUS_ENDPOINT_BLE);

	nexus_draw_card(COL_L, ROW1_Y, COL_W, ROW1_H);

	/*
	 * Snake's model, which is better than "show the winner": each
	 * transport is coloured by ITS OWN state, so the cluster answers
	 * "is USB live?" and "is my BLE profile bonded and connected?"
	 * independently. A single highlighted label could not.
	 *
	 *   green  connected        amber  bonded but not connected, or
	 *   grey   nothing there           an unpaired (open) profile
	 *
	 * The active endpoint gets an underline instead of a colour, so
	 * "which one am I typing through" never competes with "which one is
	 * healthy" for the same channel.
	 */
	bool usb_live = st->usb_present;
	gfx_color usb_c = usb_live ? t->success : t->muted;
	gfx_color ble_c;

	if (!st->bt_profile_bonded) {
		ble_c = t->warning;                 /* open, waiting to pair */
	} else if (on_ble && st->link_host == NEXUS_LINK_CONNECTED) {
		ble_c = t->success;
	} else {
		ble_c = t->error;                   /* bonded, not connected */
	}

	int y = ROW1_Y + 5;
	int usb_x = COL_L + INNER;
	int ble_x = usb_x + gfx_icon_w(2) + 10;

	gfx_icon(usb_x, y, GFX_ICON_USB, 2, usb_c, GFX_OPAQUE);
	gfx_icon(ble_x, y, GFX_ICON_BT, 2, ble_c, GFX_OPAQUE);

	char prof[4];

	gfx_utoa((uint32_t)st->bt_profile + 1U, prof, sizeof(prof), 0);
	gfx_text(ble_x + gfx_icon_w(2) + 3, y, prof, NEXUS_TXT_BODY, ble_c,
		 GFX_OPAQUE);

	/* Underline marks the endpoint actually in use. */
	int uy = y + gfx_text_h(NEXUS_TXT_BODY) + 1;

	if (on_usb) {
		gfx_rect(usb_x, uy, gfx_icon_w(2), 2, t->accent, GFX_OPAQUE);
	} else if (on_ble) {
		gfx_rect(ble_x, uy, gfx_icon_w(2), 2, t->accent, GFX_OPAQUE);
	}

	/* Locks and the jiggler along the bottom. The padlock IS caps. */
	int lx = COL_L + INNER;
	int ly = ROW1_Y + ROW1_H - 6 - gfx_text_h(NEXUS_TXT_CAPTION);

	gfx_icon(lx, ly, GFX_ICON_LOCK, 1,
		 st->caps_lock ? t->warning : t->muted, GFX_OPAQUE);
	gfx_text(lx + 9, ly, "N", NEXUS_TXT_CAPTION,
		 st->num_lock ? t->warning : t->muted, GFX_OPAQUE);
	gfx_text(lx + 16, ly, "S", NEXUS_TXT_CAPTION,
		 st->scroll_lock ? t->warning : t->muted, GFX_OPAQUE);

	if (IS_ENABLED(CONFIG_NEXUS_ANTI_IDLE_STATUS)) {
		gfx_icon(lx + 25, ly, GFX_ICON_MOUSE, 1,
			 st->anti_idle ? t->accent : t->muted, GFX_OPAQUE);
	}
}

static void draw_layer(const struct nexus_status *st)
{
	const struct nexus_theme *t = nexus_theme();
	char fallback[8];
	const char *name = st->layer_name;

	nexus_draw_card(COL_R, ROW1_Y, COL_RW, ROW1_H);
	nexus_draw_caption(COL_R + INNER, ROW1_Y + 6, "LAYER");

	if (name == NULL || name[0] == '\0') {
		/* An unnamed layer shows its index. Never invent "DEFAULT" for
		 * a keymap that never said so (Section 28). */
		fallback[0] = 'L';
		gfx_utoa(st->layer_index, &fallback[1], sizeof(fallback) - 1, 0);
		name = fallback;
	}

	int avail = COL_RW - 2 * INNER;

	gfx_text(COL_R + INNER, ROW1_Y + 17, name,
		 fit_scale(name, avail, NEXUS_TXT_BODY), t->value, GFX_OPAQUE);
}

static void draw_mods(const struct nexus_status *st)
{
	const int pw = 19;
	const int ph = 22;
	const int gap = 4;
	int x = COL_L + INNER;
	int y = ROW2_Y + (ROW2_H - ph) / 2;

	nexus_draw_card(COL_L, ROW2_Y, COL_W, ROW2_H);

	for (int i = 0; i < 4; i++) {
		bool on = (st->modifiers & mod_bits[i]) != 0;

		nexus_draw_pill(x, y, pw, ph, on, NULL);
		gfx_icon(x + (pw - gfx_icon_w(1)) / 2,
			 y + (ph - gfx_text_h(1)) / 2, mod_icons[i], 1,
			 on ? nexus_theme()->value : nexus_theme()->muted,
			 GFX_OPAQUE);
		x += pw + gap;
	}
}

static void draw_wpm(const struct nexus_status *st)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[8];

	nexus_draw_card(COL_R, ROW2_Y, COL_RW, ROW2_H);
	nexus_draw_caption(COL_R + INNER, ROW2_Y + 5, "WPM");

	/* Zero-padded to three digits so the numerals never shift sideways as
	 * the value crosses 10 or 100 (Section 27). */
	gfx_utoa(st->wpm, buf, sizeof(buf), 3);

	int right = COL_R + COL_RW - INNER;

	gfx_text(right - gfx_text_w(buf, NEXUS_TXT_VALUE), ROW2_Y + 14, buf,
		 NEXUS_TXT_VALUE, t->accent, GFX_OPAQUE);
}

static void draw_battery(int x, int w, const char *label, uint8_t pct)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[8];
	bool known = (pct != NEXUS_BATTERY_UNKNOWN && pct <= 100);

	nexus_draw_card(x, BAT_Y, w, BAT_H);
	nexus_draw_caption(x + INNER, BAT_Y + 7, label);

	if (known) {
		gfx_utoa(pct, buf, sizeof(buf), 0);
	} else {
		/* "--", never "0": an unknown half and a flat half are very
		 * different facts (Section 26). */
		buf[0] = '-';
		buf[1] = '-';
		buf[2] = '\0';
	}

	gfx_text(x + INNER, BAT_Y + 19, buf, NEXUS_TXT_BIG,
		 known ? t->value : t->muted, GFX_OPAQUE);

	if (known) {
		gfx_text(x + INNER + gfx_text_w(buf, NEXUS_TXT_BIG) + 4,
			 BAT_Y + 19 + gfx_text_h(NEXUS_TXT_BIG) -
				 gfx_text_h(NEXUS_TXT_BODY),
			 "%", NEXUS_TXT_BODY, t->caption, GFX_OPAQUE);
	}

	nexus_draw_meter(x + INNER, BAT_Y + BAT_H - 15, w - 2 * INNER, 7,
			 known ? pct : 0, batt_color(t, pct));
}

static void home_draw(void)
{
	const struct nexus_status *st = nexus_status_get();

	/*
	 * draw() runs once per band, so culling here is not an optimisation -
	 * without it every card would be re-rendered twenty times per full
	 * repaint for the two bands it actually covers.
	 */
	if (gfx_hits(BRAND_Y, BRAND_H)) {
		draw_brand();
	}
	if (gfx_hits(ROW1_Y, ROW1_H)) {
		draw_link(st);
		draw_layer(st);
	}
	if (gfx_hits(ROW2_Y, ROW2_H)) {
		draw_mods(st);
		draw_wpm(st);
	}
	if (gfx_hits(BAT_Y, BAT_H)) {
		draw_battery(COL_L, COL_W, "LEFT", st->battery_left);
		draw_battery(COL_R, COL_RW, "RIGHT", st->battery_right);
	}
}

/* ---- model ------------------------------------------------------------- */

static void on_status(const struct nexus_status *st, uint32_t changed)
{
	ARG_UNUSED(st);

	if (changed & (NEXUS_STATUS_ENDPOINT | NEXUS_STATUS_LOCKS |
		       NEXUS_STATUS_LAYER | NEXUS_STATUS_JIGGLE)) {
		nexus_screen_invalidate_rows(ROW1_Y, ROW1_Y + ROW1_H);
	}
	if (changed & (NEXUS_STATUS_MODS | NEXUS_STATUS_WPM)) {
		nexus_screen_invalidate_rows(ROW2_Y, ROW2_Y + ROW2_H);
	}
	if (changed & (NEXUS_STATUS_BATTERY | NEXUS_STATUS_LINKS)) {
		nexus_screen_invalidate_rows(BAT_Y, BAT_Y + BAT_H);
	}
}

static void home_enter(void)
{
	nexus_status_subscribe(on_status);
}

static void home_exit(void)
{
	nexus_status_unsubscribe(on_status);
}

static bool home_action(enum nexus_action action)
{
	switch (action) {
	case NEXUS_ACTION_HOME:
		return true; /* already here */
	case NEXUS_ACTION_SELECT:
#if IS_ENABLED(CONFIG_NEXUS_GAME_CENTER)
		nexus_sound_play(NEXUS_SOUND_MENU_OPEN);
		nexus_screen_push(&nexus_screen_game_center_def);
		return true;
#else
		return false;
#endif
	default:
		return false;
	}
}

const struct nexus_screen nexus_screen_home_def = {
	.name = "HOME",
	.enter = home_enter,
	.exit = home_exit,
	.draw = home_draw,
	.action = home_action,
	.refresh = NEXUS_REFRESH_NORMAL,
	.btn_short = NEXUS_ACTION_SELECT, /* -> Game Center */
	.btn_long = NEXUS_ACTION_MENU,    /* -> Settings    */
};
