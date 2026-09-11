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

/*
 * Vertical budget.
 *
 * Card headings went to scale 2 for one round and came back: 14px of "LAYER"
 * next to a 21px value reads as two competing headlines, and every card had
 * to grow to hold it, which squeezed the whole dashboard. Headings are
 * supporting text and are sized like it - the numbers you actually read from
 * across a desk carry the size instead.
 *
 * Gutters stay at 5 rather than NEXUS_GAP's 7, which is what buys ROW1 the
 * height for a proper link cluster.
 */
#define ROW_GAP 5

#define BRAND_Y NEXUS_PAD                        /*   9 */
#define BRAND_H 50                               /* ends at  59 */
#define ROW1_Y (BRAND_Y + BRAND_H + ROW_GAP)     /*  64 */
#define ROW1_H 50                                /* ends at 114 */
#define ROW2_Y (ROW1_Y + ROW1_H + ROW_GAP)       /* 119 */
#define ROW2_H 44                                /* ends at 163 */
#define BAT_Y (ROW2_Y + ROW2_H + ROW_GAP)        /* 168 */
#define BAT_H 63                                 /* ends at 231 */

#define INNER 8 /* padding inside a card */

/*
 * Modifier symbols, 11x11, drawn at scale 2 so they land at 22x22 - the same
 * size snake-module uses, and about four times the linear size of the 5x7
 * icons that were here. Four of them plus 3px gaps come to 97px, which is
 * exactly the width inside a column card.
 *
 * Row-major, bit N = column N. 88 bytes of flash, no RAM.
 */
#define MOD_GLYPH_W 11
#define MOD_GLYPH_H 11
#define MOD_SCALE 2

/*
 * Transport symbols, 9x15, straight off snake-module's model - a USB plug
 * whose body says whether HID is actually up, and the Bluetooth rune. Drawn
 * at scale 2 (18x30), which is what snake uses and what makes them readable.
 *
 * Row-major, bit N = column N, same as mod_glyphs below.
 */
#define TR_W 9
#define TR_H 15
#define TR_SCALE 2

/* USB plug, HID ready: the arrow inside is live. */
static const uint16_t tr_usb_ready[TR_H] = {
	0x0FE, 0x0FE, 0x092, 0x092, 0x092, 0x0FE, 0x1FF, 0x101,
	0x141, 0x161, 0x175, 0x11D, 0x109, 0x101, 0x1FF,
};
/* Same plug, HID not ready: the arrow becomes a cross. */
static const uint16_t tr_usb_idle[TR_H] = {
	0x0FE, 0x0FE, 0x092, 0x092, 0x092, 0x0FE, 0x1FF, 0x101,
	0x145, 0x129, 0x111, 0x129, 0x145, 0x101, 0x1FF,
};
static const uint16_t tr_ble[TR_H] = {
	0x010, 0x030, 0x070, 0x0D2, 0x092, 0x0EC, 0x078, 0x030,
	0x078, 0x0EC, 0x092, 0x0D2, 0x070, 0x030, 0x010,
};

/*
 * The status box: a bordered 9x9 tile that answers "is this profile usable",
 * separately from which transport is selected. Snake keeps these two facts on
 * two different elements for good reason - a single highlighted icon cannot
 * say "BLE is selected but that profile has never paired".
 */
#define ST_W 9
#define ST_H 9
/* Scale 3, not 2. At 2 the tile was 18x18 beside 18x30 transports - the same
 * width but two thirds the height, which reads as a smaller-class icon rather
 * than a peer. 27x27 is what snake-module uses and it sits properly. */
#define ST_SCALE 3

static const uint16_t st_ok[ST_H] = {   /* tick: bonded and connected */
	0x1FF, 0x101, 0x141, 0x161, 0x175, 0x11D, 0x109, 0x101, 0x1FF,
};
static const uint16_t st_down[ST_H] = { /* cross: bonded, not connected */
	0x1FF, 0x101, 0x145, 0x129, 0x111, 0x129, 0x145, 0x101, 0x1FF,
};
static const uint16_t st_open[ST_H] = { /* dashed: open, waiting to pair */
	0x155, 0x000, 0x101, 0x000, 0x101, 0x000, 0x101, 0x000, 0x155,
};

static const uint16_t mod_glyphs[4][MOD_GLYPH_H] = {
	/* CTRL: the caret, widening downward. */
	{ 0x000, 0x000, 0x020, 0x070, 0x0D8, 0x18C, 0x306, 0x603, 0x000,
	  0x000, 0x000 },
	/* SHIFT: filled up arrow, head over a stem. */
	{ 0x020, 0x070, 0x0F8, 0x1FC, 0x3FE, 0x7FF, 0x070, 0x070, 0x070,
	  0x070, 0x000 },
	/* ALT: the option stroke - bar over a stepped diagonal. */
	{ 0x7CF, 0x7DF, 0x018, 0x038, 0x030, 0x070, 0x060, 0x0E0, 0x0C0,
	  0x7C0, 0x780 },
	/* GUI: four panes. The Windows key is what is printed on the key. */
	{ 0x7DF, 0x7DF, 0x7DF, 0x7DF, 0x7DF, 0x000, 0x7DF, 0x7DF, 0x7DF,
	  0x7DF, 0x7DF },
};

static const uint8_t mod_bits[4] = {
	NEXUS_MOD_CTRL, NEXUS_MOD_SHIFT, NEXUS_MOD_ALT, NEXUS_MOD_GUI
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
	 * The display face, so the plate carries the same letterforms as the
	 * splash rather than a scaled-up body font. Scale 2 is 108x28 for five
	 * characters, which fits the 50px plate with its rule; a longer
	 * CONFIG_NEXUS_PRODUCT steps down from there.
	 */
	int avail = NEXUS_CONTENT_W - 2 * INNER - 4;
	int scale = 2;

	while (scale > 1 && gfx_face_w(NEXUS_PRODUCT, scale) > avail) {
		scale--;
	}

	nexus_draw_wordmark(GFX_W / 2,
			    BRAND_Y + (BRAND_H - nexus_wordmark_h(scale)) / 2,
			    NEXUS_PRODUCT, scale);
}

static void draw_link(const struct nexus_status *st)
{
	const struct nexus_theme *t = nexus_theme();
	bool on_usb = (st->endpoint == NEXUS_ENDPOINT_USB);

	nexus_draw_card(COL_L, ROW1_Y, COL_W, ROW1_H);

	/*
	 * Snake-module's layout, which separates two questions NEXUS was
	 * previously cramming into one row of colour:
	 *
	 *   which transport am I typing through   -> the lit symbol
	 *   is that endpoint actually usable      -> the status tile
	 *
	 * Selection is brightness (value vs muted), exactly as snake does it,
	 * so the transport you are on is obvious at a glance and the tile is
	 * free to carry health on its own.
	 */
	int y = ROW1_Y + (ROW1_H - TR_H * TR_SCALE) / 2;
	int usb_x = COL_L + 6;
	int ble_x = usb_x + TR_W * TR_SCALE + 5;

	gfx_glyph(usb_x, y, st->usb_present ? tr_usb_ready : tr_usb_idle,
		  TR_W, TR_H, TR_SCALE,
		  on_usb ? t->value : t->muted, GFX_OPAQUE);
	gfx_glyph(ble_x, y, tr_ble, TR_W, TR_H, TR_SCALE,
		  on_usb ? t->muted : t->value, GFX_OPAQUE);

	/* Profile number, big - it is the thing you check after BT_SEL. */
	char prof[4];
	int num_x = ble_x + TR_W * TR_SCALE + 4;

	gfx_utoa((uint32_t)st->bt_profile + 1U, prof, sizeof(prof), 0);
	gfx_text(num_x, y + 1, prof, NEXUS_TXT_BIG,
		 on_usb ? t->caption : t->accent, GFX_OPAQUE);

	/*
	 * Status tile, on the same line rather than under it. Two 30px rows
	 * plus a lock row does not fit 50, and shrinking the symbols is what
	 * made this cluster unreadable in the first place. Across the card:
	 * USB, BLE, profile number, tile - 87px inside a 107px card.
	 */
	const uint16_t *tile;
	gfx_color tile_c;

	if (!st->bt_profile_bonded) {
		tile = st_open;                 /* open, waiting to pair */
		tile_c = t->warning;
	} else if (st->bt_connected) {
		tile = st_ok;                   /* bonded and connected  */
		tile_c = t->success;
	} else {
		tile = st_down;                 /* bonded, not connected */
		tile_c = t->error;
	}
	gfx_glyph(num_x + gfx_text_w("0", NEXUS_TXT_BIG) + 5,
		  y + (TR_H * TR_SCALE - ST_H * ST_SCALE) / 2, tile,
		  ST_W, ST_H, ST_SCALE, tile_c, GFX_OPAQUE);

}

/*
 * One dot: the mouse jiggler, on the corner of the brand plate.
 *
 * The lock dots that were beside it are gone. Three outlined circles sat on
 * the plate permanently, and on any desktop keyboard Num Lock is simply ON,
 * so one of them was a solid amber dot that never changed and never meant
 * anything - it read as a warning light stuck on rather than as status. A
 * lock state you cannot act on is not worth a permanent fixture on a 240px
 * panel; the host already shows it.
 *
 * The jiggler is different: it is a mode YOU turned on, it is invisible
 * otherwise, and forgetting it is running is the actual failure mode. So it
 * gets the corner, green, with a soft halo - lit when active, and nothing at
 * all when it is not.
 */
static void draw_flags(const struct nexus_status *st)
{
	if (IS_ENABLED(CONFIG_NEXUS_ANTI_IDLE_STATUS)) {
		const struct nexus_theme *t = nexus_theme();
		const int r = 4;
		int y = BRAND_Y + 9;
		int x = COL_L + NEXUS_CONTENT_W - 10;

		if (st->anti_idle) {
			gfx_disc(x, y, r + 3, t->success, 60);
			gfx_disc(x, y, r, t->success, GFX_OPAQUE);
		}
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

	/*
	 * Capped at NEXUS_TXT_BODY, and this is the point of the cap: with a
	 * ceiling of NEXUS_TXT_VALUE, names of four or five characters fitted
	 * at scale 3 while DEFAULT (seven) had to drop to scale 2, so the
	 * layer name visibly changed size as you moved around the keymap.
	 * fit_scale exists to stop a long name leaving the card, not to give
	 * short ones a bigger typeface than long ones.
	 *
	 * A 5px inset instead of INNER buys the eight characters that make
	 * FUNCTION and DEFAULT render identically; only nine or more drop a
	 * size, and at that length something has to give.
	 */
	int avail = COL_RW - 10;

	gfx_text(COL_R + 5, ROW1_Y + 22, name,
		 fit_scale(name, avail, NEXUS_TXT_BODY), t->value, GFX_OPAQUE);
}

static void draw_mods(const struct nexus_status *st)
{
	const struct nexus_theme *t = nexus_theme();
	const int gw = MOD_GLYPH_W * MOD_SCALE; /* 22 */
	const int gh = MOD_GLYPH_H * MOD_SCALE; /* 22 */
	const int gap = 2;
	const int slot_w = gw + 2;      /* 24 */
	const int slot_h = gh + 6;      /* 28 */
	int x = COL_L + 3;              /* 4*24 + 3*2 = 102 in a 107 card */
	int y = ROW2_Y + (ROW2_H - slot_h) / 2;

	nexus_draw_card(COL_L, ROW2_Y, COL_W, ROW2_H);

	for (int i = 0; i < 4; i++) {
		bool on = (st->modifiers & mod_bits[i]) != 0;

		/*
		 * Each glyph sits in a recessed slot rather than floating on
		 * the card. Four bare symbols on a flat panel read as
		 * unfinished - the eye needs to see four *places*, so that an
		 * unheld modifier is visibly an empty slot rather than
		 * something that failed to draw.
		 */
		/*
		 * Held: the accent, the colour every other live thing on
		 * this screen uses. It was accent_alt at 110, which over a
		 * dark card is the pink diluted down to a muddy maroon - the
		 * one colour in the palette that looked like a mistake.
		 *
		 * 170 is as far as it can go and stay a tint: the glyph on
		 * top keeps at least 89 luma of contrast in every theme,
		 * Sunset being the tightest.
		 */
		gfx_round_rect(x, y, slot_w, slot_h, 4,
			       on ? t->accent : t->track, on ? 170 : 150);
		gfx_round_frame(x, y, slot_w, slot_h, 4,
				on ? t->accent : t->border,
				on ? GFX_OPAQUE : t->border_alpha);

		gfx_glyph(x + (slot_w - gw) / 2, y + (slot_h - gh) / 2,
			  mod_glyphs[i], MOD_GLYPH_W, MOD_GLYPH_H, MOD_SCALE,
			  on ? t->value : t->muted, GFX_OPAQUE);

		x += slot_w + gap;
	}
}

static void draw_wpm(const struct nexus_status *st)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[8];

	nexus_draw_card(COL_R, ROW2_Y, COL_RW, ROW2_H);
	nexus_draw_caption(COL_R + INNER, ROW2_Y + 6, "WPM");

	/* Zero-padded to three digits so the numerals never shift sideways as
	 * the value crosses 10 or 100 (Section 27). */
	gfx_utoa(st->wpm, buf, sizeof(buf), 3);

	/* The one number on this row worth reading from across the desk, so
	 * it gets NEXUS_TXT_BIG while its heading stays a caption. */
	int right = COL_R + COL_RW - INNER;

	gfx_text(right - gfx_text_w(buf, NEXUS_TXT_BIG), ROW2_Y + 14, buf,
		 NEXUS_TXT_BIG, t->accent, GFX_OPAQUE);
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

	nexus_draw_meter(x + INNER, BAT_Y + BAT_H - 11, w - 2 * INNER, 8,
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
		draw_flags(st);
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

	/* Locks and the jiggler live on the brand band now, not row 1. Marking
	 * the wrong band is not a crash, it is worse: the dot simply never
	 * updates and the bug looks like the feature is broken. */
	if (changed & (NEXUS_STATUS_LOCKS | NEXUS_STATUS_JIGGLE)) {
		nexus_screen_invalidate_rows(BRAND_Y, BRAND_Y + BRAND_H);
	}
	if (changed & (NEXUS_STATUS_ENDPOINT | NEXUS_STATUS_LAYER)) {
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
