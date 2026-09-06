/*
 * Settings, Diagnostics and About (Sections 62-63, 86).
 *
 * One generic list screen serves Settings and Diagnostics. Rows that can
 * change at runtime do; the rest report their build-time value rather than
 * pretending to be editable (Section 62 allows build-time config in v1).
 *
 * Values are formatted once per tick into a small cache, never inside draw().
 * draw() runs once per compositor band, so formatting there would run every
 * row twenty times per repaint - and formatting is also the one thing on this
 * thread that can eat stack (see gfx_utoa).
 */

#include <nexus/display.h>
#include <nexus/game.h>
#include <nexus/gfx.h>
#include <nexus/nexus.h>
#include <nexus/screen.h>
#include <nexus/settings.h>
#include <nexus/sound.h>
#include <nexus/status.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "../nexus_priv.h"

#define TITLE_Y 8
#define LIST_Y 26
/*
 * Body text, not caption. At 5x7 the list was legible with your nose on the
 * panel and not from across a desk, which is the distance a dongle is
 * actually read from. 10x14 costs rows, so the list scrolls instead.
 */
#define ROW_TEXT NEXUS_TXT_BODY
#define ROW_H 24
#define ROW_PITCH 27
#define VIS_ROWS 7                 /* 26 + 7*27 = 215, hint sits below */
#define HINT_Y 220
#define MAX_ROWS 14                /* total items; VIS_ROWS are on screen */
#define VAL_MAX 16
#define INNER 8

/* ---- tiny formatters (no printf on the display thread) ----------------- */

struct buf {
	char *p;
	char *end;
};

static struct buf buf_init(char *dst, size_t len)
{
	dst[0] = '\0';
	return (struct buf){ .p = dst, .end = dst + len - 1 };
}

static void put(struct buf *b, const char *s)
{
	while (s && *s && b->p < b->end) {
		*b->p++ = *s++;
	}
	*b->p = '\0';
}

static void put_u(struct buf *b, uint32_t v)
{
	char tmp[12];

	put(b, gfx_utoa(v, tmp, sizeof(tmp), 0));
}

/* ---- generic list screen ----------------------------------------------- */

struct row {
	const char *label;
	/** Fill @p out with the current value; NULL means no value column. */
	void (*value)(char *out, size_t len);
	/** NULL means the row is informational and SELECT does nothing. */
	void (*activate)(void);
};

static const struct row *g_rows;
static const char *g_title;
static uint8_t g_row_count;
static uint8_t g_cursor;
static char g_val[MAX_ROWS][VAL_MAX];

/* Returns true if any rendered value actually changed. */
static bool refresh_values(void)
{
	char prev[VAL_MAX];
	bool changed = false;

	for (uint8_t i = 0; i < g_row_count; i++) {
		memcpy(prev, g_val[i], VAL_MAX);

		if (g_rows[i].value) {
			g_rows[i].value(g_val[i], VAL_MAX);
		} else {
			g_val[i][0] = '\0';
		}
		if (memcmp(prev, g_val[i], VAL_MAX) != 0) {
			changed = true;
		}
	}
	return changed;
}

/* Keep the cursor near the middle of the window, clamped at both ends. */
static uint8_t first_visible(void)
{
	if (g_row_count <= VIS_ROWS) {
		return 0;
	}

	int f = (int)g_cursor - VIS_ROWS / 2;

	if (f < 0) {
		f = 0;
	}
	if (f > g_row_count - VIS_ROWS) {
		f = g_row_count - VIS_ROWS;
	}
	return (uint8_t)f;
}

static void list_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	uint8_t first = first_visible();

	if (gfx_hits(TITLE_Y, gfx_text_h(NEXUS_TXT_LABEL))) {
		nexus_draw_label(NEXUS_PAD, TITLE_Y, g_title);

		/* Position in the list, so a scrolled window does not look
		 * like the whole list. */
		if (g_row_count > VIS_ROWS) {
			char pos[10];
			int n;

			gfx_utoa(g_cursor + 1U, pos, sizeof(pos), 0);
			n = 0;
			while (pos[n]) {
				n++;
			}
			pos[n++] = '/';
			gfx_utoa(g_row_count, &pos[n], (int)sizeof(pos) - n, 0);

			gfx_text(NEXUS_PAD + NEXUS_CONTENT_W -
					 gfx_text_w(pos, NEXUS_TXT_LABEL),
				 TITLE_Y, pos, NEXUS_TXT_LABEL, t->caption,
				 GFX_OPAQUE);
		}
	}

	for (uint8_t v = 0; v < VIS_ROWS; v++) {
		uint8_t i = first + v;

		if (i >= g_row_count) {
			break;
		}

		int y = LIST_Y + v * ROW_PITCH;

		if (!gfx_hits(y, ROW_H)) {
			continue;
		}

		bool sel = (i == g_cursor);

		nexus_draw_card_sel(NEXUS_PAD, y, NEXUS_CONTENT_W, ROW_H, sel);

		int ty = y + (ROW_H - gfx_text_h(ROW_TEXT)) / 2;

		gfx_text(NEXUS_PAD + INNER, ty, g_rows[i].label, ROW_TEXT,
			 sel ? t->value : t->caption, GFX_OPAQUE);

		if (g_val[i][0]) {
			int right = NEXUS_PAD + NEXUS_CONTENT_W - INNER;
			int room = NEXUS_CONTENT_W - 2 * INNER - 6 -
				   gfx_text_w(g_rows[i].label, ROW_TEXT);
			int vs = ROW_TEXT;

			/*
			 * Drop a size rather than clip. Every value on this
			 * hardware fits at 10x14, but CONFIG_BOARD is whatever
			 * board someone built for - "promicro_nrf52840" is
			 * already too wide beside its label - and a truncated
			 * diagnostic is worse than a small one.
			 */
			if (gfx_text_w(g_val[i], vs) > room) {
				vs = NEXUS_TXT_CAPTION;
			}

			gfx_text(right - gfx_text_w(g_val[i], vs),
				 y + (ROW_H - gfx_text_h(vs)) / 2, g_val[i], vs,
				 t->accent, GFX_OPAQUE);
		}
	}

	/*
	 * The button means the same thing on every list: tap moves, hold acts.
	 * Saying so is what makes a one-button menu usable, and it is why BACK
	 * is a row rather than a second meaning for hold.
	 */
	/*
	 * Shorter than "HOLD=SELECT" because it is now twice the size: the
	 * old string is 22 characters, which at NEXUS_TXT_LABEL is 262px on
	 * a 240px panel. A legible hint that fits beats a precise one that
	 * runs off the screen.
	 */
	if (gfx_hits(HINT_Y, gfx_text_h(NEXUS_TXT_LABEL))) {
		gfx_text_c(GFX_W / 2, HINT_Y, "TAP=NEXT  HOLD=OK",
			   NEXUS_TXT_LABEL, t->caption, GFX_OPAQUE);
	}
}

static void list_enter(const char *title, const struct row *rows, uint8_t count)
{
	g_title = title;
	g_rows = rows;
	g_row_count = MIN(count, MAX_ROWS);
	if (g_cursor >= g_row_count) {
		g_cursor = 0;
	}
	refresh_values();
}

static void list_exit(void)
{
	g_rows = NULL;
	g_row_count = 0;
}

static bool list_action(enum nexus_action action)
{
	switch (action) {
	case NEXUS_ACTION_NEXT:
	case NEXUS_ACTION_DOWN:
		if (g_row_count) {
			g_cursor = (uint8_t)((g_cursor + 1) % g_row_count);
			nexus_sound_play(NEXUS_SOUND_SELECT);
			nexus_screen_invalidate();
		}
		return true;
	case NEXUS_ACTION_PREVIOUS:
	case NEXUS_ACTION_UP:
		if (g_row_count) {
			g_cursor = (uint8_t)((g_cursor + g_row_count - 1) %
					     g_row_count);
			nexus_sound_play(NEXUS_SOUND_SELECT);
			nexus_screen_invalidate();
		}
		return true;
	case NEXUS_ACTION_SELECT:
		if (g_row_count && g_rows[g_cursor].activate) {
			g_rows[g_cursor].activate();
			nexus_sound_play(NEXUS_SOUND_MENU_SELECT);
			refresh_values();
			nexus_screen_invalidate();
		}
		return true;
	default:
		return false;
	}
}

static void list_tick(void)
{
	/*
	 * Only the diagnostics list ticks, and most of its readings hold
	 * still for seconds at a time. Repainting all 240 rows five times a
	 * second to redraw identical text is fill rate taken from whatever
	 * the user is actually doing.
	 */
	if (refresh_values()) {
		nexus_screen_invalidate();
	}
}

/* ---- settings ---------------------------------------------------------- */

#if IS_ENABLED(CONFIG_NEXUS_GAMES)
static void v_game_speed(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, nexus_game_speed_name());
}

static void a_game_speed(void)
{
	/* Wraps, like every other cycling row here: five values do not want a
	 * separate "back" gesture to get from INSANE to SLOW. */
	uint8_t next = nexus_game_speed() + 1;

	if (next > NEXUS_GAME_SPEED_MAX) {
		next = NEXUS_GAME_SPEED_MIN;
	}
	nexus_game_speed_set(next);
	nexus_settings_save_deferred();
}
#endif

#if IS_ENABLED(CONFIG_NEXUS_SNAKE)
static void v_walls(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	/* Named for the walls, not for the wrap, because "WALLS ON" is what
	 * the player is choosing - the torus is the implementation. */
	put(&b, nexus_snake_wrap() ? "OFF" : "ON");
}

static void a_walls(void)
{
	nexus_snake_wrap_set(!nexus_snake_wrap());
	nexus_settings_save_deferred();
}
#endif

static void v_sound(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, nexus_sound_enabled() ? "ON" : "OFF");
}

static void a_sound(void)
{
	nexus_sound_set_enabled(!nexus_sound_enabled());
	nexus_settings_touch();
}

static void v_theme(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, nexus_theme()->name);
}

static void a_theme(void)
{
	/* Same helper the encoder uses, so the two paths cannot drift. */
	nexus_theme_cycle(1);
	nexus_settings_touch();
}

/* No local copy of the level: the backlight HAL owns it, so a value restored
 * from saved settings shows correctly here without a second thing to sync. */
static void v_brightness(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	if (nexus_display_backlight_mode() == NEXUS_BACKLIGHT_FIXED) {
		/* BL strapped to VCC. Say so rather than offering a slider that
		 * does nothing (Section 10). */
		put(&b, "FIXED");
		return;
	}
	if (!nexus_display_backlight_has_brightness()) {
		put(&b, nexus_display_backlight_level() ? "ON" : "OFF");
		return;
	}

	put_u(&b, nexus_display_backlight_level());
	put(&b, "%");
}

static void a_brightness(void)
{
	if (nexus_display_backlight_mode() == NEXUS_BACKLIGHT_FIXED) {
		return; /* the hardware has no say in this; do not fake it */
	}

	uint8_t level = nexus_display_backlight_level();

	if (nexus_display_backlight_has_brightness()) {
		level = (level >= 100) ? 25 : (uint8_t)(level + 25);
	} else {
		level = level ? 0 : 100;
	}
	nexus_display_backlight_set(level);
	nexus_settings_touch();
}

static void v_anim(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, IS_ENABLED(CONFIG_NEXUS_ANIMATIONS) ? "ON" : "OFF");
}

static void v_splash(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

#if IS_ENABLED(CONFIG_NEXUS_SPLASH)
	put_u(&b, CONFIG_NEXUS_SPLASH_DURATION_MS);
	put(&b, " MS");
#else
	put(&b, "OFF");
#endif
}

static void v_games(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

#if IS_ENABLED(CONFIG_NEXUS_GAMES)
	put_u(&b, nexus_game_count());
#else
	put(&b, "OFF");
#endif
}

static void a_diagnostics(void)
{
	nexus_screen_push(&nexus_screen_diagnostics_def);
}

static void a_about(void)
{
	nexus_screen_push(&nexus_screen_about_def);
}

static void v_save(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	/* The row doubles as the unsaved-changes indicator, so there is no
	 * separate dirty marker to notice or miss. */
	put(&b, nexus_settings_dirty() ? "*" : "OK");
}

static void a_save(void)
{
	if (nexus_settings_save() == 0) {
		nexus_sound_play(NEXUS_SOUND_MENU_SELECT);
	} else {
		nexus_sound_play(NEXUS_SOUND_BACK);
	}
}

/* BACK is a row, not a gesture. With one button you need three verbs from two
 * gestures, and making hold mean "activate" here but "go back" everywhere else
 * is how a menu stops being predictable. */
static void a_back(void)
{
	nexus_screen_pop();
}

static const struct row settings_rows[] = {
	{ "SOUND", v_sound, a_sound },
	{ "BRIGHT", v_brightness, a_brightness },
	{ "THEME", v_theme, a_theme },
	{ "ANIM", v_anim, NULL },
#if IS_ENABLED(CONFIG_NEXUS_GAMES)
	{ "SPEED", v_game_speed, a_game_speed },
#endif
#if IS_ENABLED(CONFIG_NEXUS_SNAKE)
	{ "SNAKE WALL", v_walls, a_walls },
#endif
	{ "SPLASH", v_splash, NULL },
	{ "GAMES", v_games, NULL },
	{ "DIAG", NULL, a_diagnostics },
	{ "ABOUT", NULL, a_about },
	{ "SAVE", v_save, a_save },
	{ "BACK", NULL, a_back },
};

static void settings_enter(void)
{
	list_enter("SETTINGS", settings_rows, ARRAY_SIZE(settings_rows));
}

const struct nexus_screen nexus_screen_settings_def = {
	.name = "SETTINGS",
	.enter = settings_enter,
	.exit = list_exit,
	.draw = list_draw,
	.action = list_action,
	.refresh = NEXUS_REFRESH_IDLE,
	.btn_short = NEXUS_ACTION_NEXT,
	.btn_long = NEXUS_ACTION_SELECT,
};

/* ---- diagnostics ------------------------------------------------------- */

/* Two characters, because at 10x14 "RECON/RECON" plus its label overruns the
 * 206px content width and clipped text is worse than terse text. */
static const char *link_text(enum nexus_link_state s)
{
	switch (s) {
	case NEXUS_LINK_CONNECTED:
		return "OK";
	case NEXUS_LINK_CONNECTING:
		return "CN";
	case NEXUS_LINK_RECONNECTING:
		return "RC";
	default:
		return "--";
	}
}

static void v_version(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, "V" NEXUS_VERSION_STR);
}

static void v_board(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, nexus_board_name());
}

static void v_display(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, nexus_health()->display ? "OK" : "FAIL");
}

static void v_buzzer(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, nexus_health()->buzzer ? "OK" : "N/A");
}

static void v_button(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put(&b, nexus_health()->button ? "OK" : "N/A");
}

static void v_backlight(char *out, size_t len)
{
	static const char *const modes[] = { "OFF", "ON", "FIXED" };
	struct buf b = buf_init(out, len);

	put(&b, modes[nexus_display_backlight_mode()]);
}

static void v_host(char *out, size_t len)
{
	const struct nexus_status *st = nexus_status_get();
	struct buf b = buf_init(out, len);

	put(&b, st->endpoint == NEXUS_ENDPOINT_USB   ? "USB "
		: st->endpoint == NEXUS_ENDPOINT_BLE ? "BLE "
						     : "--- ");
	put(&b, link_text(st->link_host));
}

static void v_halves(char *out, size_t len)
{
	const struct nexus_status *st = nexus_status_get();
	struct buf b = buf_init(out, len);

	put(&b, link_text(st->link_left));
	put(&b, "/");
	put(&b, link_text(st->link_right));
}

static void put_batt(struct buf *b, uint8_t pct)
{
	if (pct == NEXUS_BATTERY_UNKNOWN || pct > 100) {
		put(b, "--");
	} else {
		put_u(b, pct);
	}
}

static void v_batteries(char *out, size_t len)
{
	const struct nexus_status *st = nexus_status_get();
	struct buf b = buf_init(out, len);

	put_batt(&b, st->battery_left);
	put(&b, "/");
	put_batt(&b, st->battery_right);
}

static void v_memory(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	/*
	 * The UI's own static footprint: the compositor band (5,760 B) plus
	 * the menu value cache (224 B). A constant, not a live reading - the
	 * row is called UI STATIC rather than UI RAM because "RAM" next to a
	 * number invites reading it as free or used memory, and it is
	 * neither.
	 *
	 * NEXUS allocates nothing at runtime: every buffer it owns is static,
	 * so the UI has no way to run out of memory at an awkward moment and
	 * no free-heap number to report. Section 63 asks for "Free RAM"; on
	 * this design the honest answer is that there is no UI heap to watch,
	 * and a fabricated number would be worse than none. For whole-image
	 * figures watch the linker output, which CI prints (Section 100).
	 */
	put_u(&b, (uint32_t)(sizeof(g_val) + GFX_W * GFX_STRIP_H * 2U));
	put(&b, "B");
}

#if IS_ENABLED(CONFIG_NEXUS_DEBUG)
static void v_fps(char *out, size_t len)
{
	struct buf b = buf_init(out, len);

	put_u(&b, nexus_screen_fps());
}
#endif

static void v_uptime(char *out, size_t len)
{
	uint32_t s = (uint32_t)(k_uptime_get() / MSEC_PER_SEC);
	struct buf b = buf_init(out, len);

	put_u(&b, s / 3600U);
	put(&b, "H");
	put_u(&b, (s / 60U) % 60U);
	put(&b, "M");
}

static const struct row diag_rows[] = {
	{ "FIRMWARE", v_version, NULL },
	{ "BOARD", v_board, NULL },
	{ "DISPLAY", v_display, NULL },
	{ "BACKLIGHT", v_backlight, NULL },
	{ "BUZZER", v_buzzer, NULL },
	{ "BUTTON", v_button, NULL },
	{ "HOST", v_host, NULL },
	{ "L/R LINK", v_halves, NULL },
	{ "L/R BATT", v_batteries, NULL },
	{ "UI STATIC", v_memory, NULL },
	{ "UPTIME", v_uptime, NULL },
	{ "BACK", NULL, a_back },
#if IS_ENABLED(CONFIG_NEXUS_DEBUG)
	/* Section 64: the FPS readout is a debug-build feature. Counting frames
	 * is free, but a row that repaints every second to show the number is
	 * not, and production has no use for it. */
	{ "FPS", v_fps, NULL },
#endif
};

static void diag_enter(void)
{
	list_enter("DIAGNOSTICS", diag_rows, ARRAY_SIZE(diag_rows));
}

const struct nexus_screen nexus_screen_diagnostics_def = {
	.name = "DIAGNOSTICS",
	.enter = diag_enter,
	.exit = list_exit,
	.draw = list_draw,
	.action = list_action,
	.tick = list_tick,
	.refresh = NEXUS_REFRESH_NORMAL,
	.btn_short = NEXUS_ACTION_NEXT,
	.btn_long = NEXUS_ACTION_SELECT,
};

/* ---- about ------------------------------------------------------------- */

static void about_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	/* NEXUS_SUBTITLE is not here any more: the wordmark sets it directly
	 * under the name, which is where it belongs and where the splash puts
	 * it. Listing it again three cards down was the same string twice. */
	static const char *const lines[] = {
		"NRF52840  ST7789",
		"POWERED BY ZMK",
	};

	/* 66, not 62: the wordmark carries its subtitle now, so the card holds
	 * caption + name + strapline. Still clears the FIRMWARE card at 84. */
	nexus_draw_card(NEXUS_PAD, 14, NEXUS_CONTENT_W, 66);
	nexus_draw_caption_c(GFX_W / 2, 21, NEXUS_BRAND);
	/*
	 * Scale 2, not 4. At 4 the face alone is 56px tall from y=33, so it
	 * ran to 89 inside a card that ends at 76 - and the FIRMWARE card is
	 * drawn after it, painting over the bottom of the word and its rule.
	 * It has been clipped that way the whole time; the extrusion only made
	 * it more obvious. 2 fits the card it was given, with the face landing
	 * on the same y=33 it always had.
	 */
	nexus_draw_wordmark(GFX_W / 2, 31, NEXUS_PRODUCT, 2);

	nexus_draw_card(NEXUS_PAD, 84, NEXUS_CONTENT_W, 38);
	nexus_draw_caption_c(GFX_W / 2, 91, "FIRMWARE");
	gfx_text_c(GFX_W / 2, 102, "V" NEXUS_VERSION_STR, NEXUS_TXT_BODY,
		   t->accent, GFX_OPAQUE);

	for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
		nexus_draw_caption_c(GFX_W / 2, 130 + (int)i * 14, lines[i]);
	}

	/*
	 * Creator credit. Given its own card at body size rather than a fourth
	 * caption line: whoever built the thing should not be the smallest
	 * text on its About screen. Empty CONFIG_NEXUS_AUTHOR hides it.
	 */
	if (sizeof(NEXUS_AUTHOR) > 1) {
		nexus_draw_card(NEXUS_PAD, 180, NEXUS_CONTENT_W, 46);
		nexus_draw_caption_c(GFX_W / 2, 188, "CREATED BY");

		int scale = gfx_text_w(NEXUS_AUTHOR, NEXUS_TXT_BODY) <=
					    NEXUS_CONTENT_W - 16
				    ? NEXUS_TXT_BODY
				    : NEXUS_TXT_CAPTION;

		gfx_text_c(GFX_W / 2, 202, NEXUS_AUTHOR, scale, t->value,
			   GFX_OPAQUE);
	}
}

const struct nexus_screen nexus_screen_about_def = {
	.name = "ABOUT",
	.draw = about_draw,
	.refresh = NEXUS_REFRESH_IDLE,
	.btn_short = NEXUS_ACTION_BACK,
	.btn_long = NEXUS_ACTION_HOME,
};
