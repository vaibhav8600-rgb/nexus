/*
 * Game Center launcher and the screen that hosts a running game
 * (Sections 36, 38, 53, 105).
 *
 * The launcher asks the manager for names, icons and high scores. It has never
 * heard of Tetris, so a second game costs one registry entry and zero edits
 * here (Section 51). The arrows are drawn even with a single game, because a
 * launcher that changes shape when a game is added is a launcher nobody trusts.
 */

#include <nexus/game.h>
#include <nexus/gfx.h>
#include <nexus/screen.h>
#include <nexus/sound.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>

#include "../nexus_priv.h"

#define TITLE_Y 8
#define CARD_W 150
#define CARD_H 130
#define CARD_X ((GFX_W - CARD_W) / 2)
#define CARD_Y 26
#define ICON_CY (CARD_Y + 48)
#define NAME_Y (CARD_Y + CARD_H - 34)
#define ARROW_Y (CARD_Y + CARD_H / 2 - 10)
#define HIGH_Y 176

static uint8_t g_selected;

static void gc_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	const struct nexus_game *game = nexus_game_at(g_selected);
	uint8_t count = nexus_game_count();
	char buf[12];

	if (gfx_hits(TITLE_Y, gfx_text_h(NEXUS_TXT_CAPTION))) {
		nexus_draw_caption(NEXUS_PAD, TITLE_Y, "GAME CENTER");

		/* "01/03" - position in the list, so a second game is visibly
		 * expected rather than a surprise. */
		gfx_utoa(count ? g_selected + 1U : 0U, buf, sizeof(buf), 2);
		int n = 0;

		while (buf[n]) {
			n++;
		}
		buf[n++] = '/';
		gfx_utoa(count, &buf[n], (int)sizeof(buf) - n, 2);

		gfx_text(NEXUS_PAD + NEXUS_CONTENT_W -
				 gfx_text_w(buf, NEXUS_TXT_CAPTION),
			 TITLE_Y, buf, NEXUS_TXT_CAPTION, t->caption,
			 GFX_OPAQUE);
	}

	if (gfx_hits(CARD_Y, CARD_H)) {
		nexus_draw_card_sel(CARD_X, CARD_Y, CARD_W, CARD_H, true);

		if (game == NULL) {
			gfx_text_c(GFX_W / 2, ICON_CY - 7, "NO GAMES",
				   NEXUS_TXT_BODY, t->muted, GFX_OPAQUE);
		} else {
			if (game->draw_icon) {
				game->draw_icon(GFX_W / 2, ICON_CY);
			}
			gfx_text_c(GFX_W / 2, NAME_Y, game->name,
				   NEXUS_TXT_VALUE, t->accent, GFX_OPAQUE);
		}
	}

	if (gfx_hits(ARROW_Y, gfx_text_h(NEXUS_TXT_BODY))) {
		gfx_color arrow = (count > 1) ? t->caption : t->muted;

		gfx_text(NEXUS_PAD + 2, ARROW_Y, "<", NEXUS_TXT_BODY, arrow,
			 GFX_OPAQUE);
		gfx_text(GFX_W - NEXUS_PAD - 2 - gfx_text_w(">", NEXUS_TXT_BODY),
			 ARROW_Y, ">", NEXUS_TXT_BODY, arrow, GFX_OPAQUE);
	}

	if (gfx_hits(HIGH_Y, 46)) {
		nexus_draw_card(NEXUS_PAD, HIGH_Y, NEXUS_CONTENT_W, 46);
		nexus_draw_caption_c(GFX_W / 2, HIGH_Y + 8, "HIGH SCORE");
		gfx_text_c(GFX_W / 2, HIGH_Y + 20,
			   gfx_utoa(game ? nexus_game_highscore(game) : 0, buf,
				    sizeof(buf), 0),
			   NEXUS_TXT_VALUE, t->value, GFX_OPAQUE);
	}
}

static void gc_enter(void)
{
	if (g_selected >= nexus_game_count()) {
		g_selected = 0;
	}
}

static bool gc_action(enum nexus_action action)
{
	uint8_t count = nexus_game_count();

	switch (action) {
	case NEXUS_ACTION_NEXT:
	case NEXUS_ACTION_RIGHT:
		if (count > 1) {
			g_selected = (uint8_t)((g_selected + 1) % count);
			nexus_sound_play(NEXUS_SOUND_SELECT);
			nexus_screen_invalidate();
		}
		return true;
	case NEXUS_ACTION_PREVIOUS:
	case NEXUS_ACTION_LEFT:
		if (count > 1) {
			g_selected = (uint8_t)((g_selected + count - 1) % count);
			nexus_sound_play(NEXUS_SOUND_SELECT);
			nexus_screen_invalidate();
		}
		return true;
	case NEXUS_ACTION_SELECT:
		if (count == 0) {
			return true;
		}
		nexus_sound_play(NEXUS_SOUND_MENU_SELECT);
		nexus_screen_push(&nexus_screen_game_def);
		return true;
	default:
		return false;
	}
}

const struct nexus_screen nexus_screen_game_center_def = {
	.name = "GAME_CENTER",
	.enter = gc_enter,
	.draw = gc_draw,
	.action = gc_action,
	.refresh = NEXUS_REFRESH_IDLE,
	.btn_short = NEXUS_ACTION_SELECT,
	.btn_long = NEXUS_ACTION_HOME,
};

/* ------------------------------------------------------------------------- */
/* The host screen for whatever game the launcher picked.                     */
/* ------------------------------------------------------------------------- */

static void game_enter(void)
{
	nexus_game_launch(g_selected);
}

static void game_exit(void)
{
	/* Banks the score and cancels the game's timers. Reached from BACK, a
	 * long press, and nexus_screen_home() alike, so there is no path that
	 * leaves a gravity timer running behind another screen. */
	nexus_game_stop();
}

static bool game_action(enum nexus_action action)
{
	if (nexus_game_input(action)) {
		return true;
	}

	if (action == NEXUS_ACTION_BACK) {
		nexus_sound_play(NEXUS_SOUND_BACK);
		nexus_screen_pop();
		return true;
	}

	return false;
}

static void game_draw(void)
{
	nexus_game_draw();
}

static void game_tick(void)
{
	/*
	 * Only for games that advance with the screen. Tetris runs its own
	 * gravity timer and invalidates when the board actually changes, which
	 * is far cheaper than repainting 240x240 thirty times a second whether
	 * anything moved or not (Section 60).
	 */
	nexus_game_tick();
}

const struct nexus_screen nexus_screen_game_def = {
	.name = "GAME",
	.enter = game_enter,
	.exit = game_exit,
	.draw = game_draw,
	.action = game_action,
	.tick = game_tick,
	.refresh = NEXUS_REFRESH_FAST,
	.btn_short = NEXUS_ACTION_SELECT, /* pause / resume / restart */
	.btn_long = NEXUS_ACTION_BACK,    /* leave the game           */
};
