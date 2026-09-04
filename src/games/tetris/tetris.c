/*
 * Tetris presentation layer (Sections 39-50, 106).
 *
 * All rules live in tetris_core.c. This file does three things: paint the
 * well, turn logical actions into rule calls, and run the gravity clock off a
 * delayed work item. There is no game loop and no game thread - a while(1)
 * here would starve BLE, USB and Studio, which Sections 44 and 141-F forbid
 * outright.
 *
 * There is also no framebuffer. The board is ten by twenty bytes in
 * tetris_core; the pixels are composited a band at a time straight from it, so
 * the whole game costs about 220 bytes of RAM instead of the 3.6 KB canvas a
 * widget toolkit would need for the same picture (Sections 40, 100).
 */

#include <nexus/game.h>
#include <nexus/gfx.h>
#include <nexus/screen.h>
#include <nexus/sound.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../../nexus_priv.h"
#include "tetris_core.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* ---- layout ------------------------------------------------------------ */
#define CELL 10
#define WELL_W (TETRIS_COLS * CELL)          /* 100 */
#define WELL_H (TETRIS_VISIBLE_ROWS * CELL)  /* 200 */
#define WELL_X NEXUS_PAD                     /*   9 */
#define WELL_Y 30
#define FRAME_X (WELL_X - 2)
#define FRAME_Y (WELL_Y - 2)
#define FRAME_W (WELL_W + 4)
#define FRAME_H (WELL_H + 4)

#define SIDE_X 117
#define SIDE_W (GFX_W - NEXUS_PAD - SIDE_X) /* 114 */
#define SIDE_IN 8

#define SCORE_Y 28
#define SCORE_H 44
#define LEVEL_Y 78
#define LEVEL_H 38
#define LINES_Y 122
#define LINES_H 38
#define NEXT_Y 166
#define NEXT_H 64
#define NEXT_CELL 8

#define OVER_X 20
#define OVER_Y 70
#define OVER_W 200
#define OVER_H 108

extern const struct nexus_game nexus_game_tetris;

static struct tetris g_t;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void gravity_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_gravity, gravity_fn);

/* ---- painting ---------------------------------------------------------- */

/*
 * Section 106: pieces must not be told apart by hue alone. Each gets a
 * distinct colour, a lit/shaded bevel, and a two-pixel mark at its own spot in
 * a 3x3 grid - so the board still reads correctly in monochrome, at a glance,
 * or to a colourblind player.
 */
static gfx_color piece_color(uint8_t piece)
{
	static const gfx_color hue[8] = {
		0,
		NEXUS_C(0x36E0E0u), /* I cyan   */
		NEXUS_C(0xF5D442u), /* O yellow */
		NEXUS_C(0xB44DE0u), /* T purple */
		NEXUS_C(0x4DE07Au), /* S green  */
		NEXUS_C(0xE04D6Au), /* Z red    */
		NEXUS_C(0x4D7AE0u), /* J blue   */
		NEXUS_C(0xE0904Du), /* L orange */
	};

	return hue[piece & 7u];
}

static void draw_block(int x, int y, int size, uint8_t piece)
{
	gfx_color body = piece_color(piece);

	gfx_rect(x, y, size, size, body, GFX_OPAQUE);

	/* Lit top-left edge, shaded bottom-right: the bevel is what gives the
	 * stack depth and separates two touching blocks of the same colour. */
	gfx_hline(x, y, size, NEXUS_C(0xFFFFFFu), 90);
	gfx_vline(x, y, size, NEXUS_C(0xFFFFFFu), 90);
	gfx_hline(x, y + size - 1, size, NEXUS_C(0x000000u), 110);
	gfx_vline(x + size - 1, y, size, NEXUS_C(0x000000u), 110);

	if (size < 8) {
		return; /* the preview cells are too small for the mark */
	}

	/* Per-piece mark: index 0-6 walks a 3x3 grid, so no two pieces carry
	 * the same pattern. */
	int i = (piece - 1) % 7;
	int mx = x + 2 + (i % 3) * ((size - 5) / 2);
	int my = y + 2 + (i / 3) * ((size - 5) / 2);

	gfx_rect(mx, my, 2, 2, NEXUS_C(0x000000u), 120);
}

static void draw_well(void)
{
	const struct nexus_theme *t = nexus_theme();

	nexus_draw_card(FRAME_X, FRAME_Y, FRAME_W, FRAME_H);
	gfx_rect(WELL_X, WELL_Y, WELL_W, WELL_H, t->track, 220);

	for (int row = 0; row < TETRIS_VISIBLE_ROWS; row++) {
		int y = WELL_Y + row * CELL;

		/* One band is 12px and a cell is 10, so at most two rows are
		 * ever live: skipping the rest is most of the cost. */
		if (!gfx_hits(y, CELL)) {
			continue;
		}

		for (int col = 0; col < TETRIS_COLS; col++) {
			uint8_t p = tetris_render_cell(&g_t, row, col);

			if (p != TETRIS_EMPTY) {
				draw_block(WELL_X + col * CELL, y, CELL, p);
			}
		}
	}
}

static void stat_card(int y, int h, const char *caption, uint32_t value,
		      int scale, gfx_color color)
{
	char buf[12];

	if (!gfx_hits(y, h)) {
		return;
	}

	nexus_draw_card(SIDE_X, y, SIDE_W, h);
	nexus_draw_caption(SIDE_X + SIDE_IN, y + 7, caption);

	gfx_utoa(value, buf, sizeof(buf), 0);
	gfx_text(SIDE_X + SIDE_W - SIDE_IN - gfx_text_w(buf, scale),
		 y + h - 8 - gfx_text_h(scale), buf, scale, color, GFX_OPAQUE);
}

static void draw_next(void)
{
	const struct nexus_theme *t = nexus_theme();

	if (!gfx_hits(NEXT_Y, NEXT_H)) {
		return;
	}

	nexus_draw_card(SIDE_X, NEXT_Y, SIDE_W, NEXT_H);
	nexus_draw_caption(SIDE_X + SIDE_IN, NEXT_Y + 7, "NEXT");

	int box = 4 * NEXT_CELL;
	int ox = SIDE_X + (SIDE_W - box) / 2;
	int oy = NEXT_Y + NEXT_H - 8 - box;

	gfx_round_rect(ox - 3, oy - 3, box + 6, box + 6, 4, t->track, 200);

	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			if (tetris_shape_has(g_t.next, 0, r, c)) {
				draw_block(ox + c * NEXT_CELL,
					   oy + r * NEXT_CELL, NEXT_CELL,
					   g_t.next);
			}
		}
	}
}

static void draw_overlay(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (g_over_title == NULL || !gfx_hits(OVER_Y, OVER_H)) {
		return;
	}

	/* Dim the board behind the card so the text is readable over whatever
	 * the stack happens to look like. */
	gfx_rect(0, OVER_Y - 10, GFX_W, OVER_H + 20, t->bg_bot, 205);
	nexus_draw_card_sel(OVER_X, OVER_Y, OVER_W, OVER_H, true);

	gfx_text_c(GFX_W / 2, OVER_Y + 12, g_over_title, NEXUS_TXT_VALUE,
		   t->accent, GFX_OPAQUE);

	if (g_state == NEXUS_GAME_OVER) {
		/* Score and best side by side, each caption over its own value
		 * so the two columns line up (Section 50). */
		const int col = 46;

		nexus_draw_caption_c(GFX_W / 2 - col, OVER_Y + 40, "SCORE");
		gfx_text_c(GFX_W / 2 - col, OVER_Y + 52,
			   gfx_utoa(g_t.score, buf, sizeof(buf), 0),
			   NEXUS_TXT_BODY, t->value, GFX_OPAQUE);

		nexus_draw_caption_c(GFX_W / 2 + col, OVER_Y + 40, "BEST");
		gfx_text_c(GFX_W / 2 + col, OVER_Y + 52,
			   gfx_utoa(nexus_game_highscore(&nexus_game_tetris),
				    buf, sizeof(buf), 0),
			   NEXUS_TXT_BODY, t->accent, GFX_OPAQUE);
	}

	/*
	 * Body size, two lines. These are the only instructions the game ever
	 * gives, and at caption size they were the smallest text on a panel
	 * you read from across a desk - so the one screen that has to tell you
	 * what the button does was the hardest thing on it to read.
	 */
	if (g_over_hint) {
		gfx_text_c(GFX_W / 2, OVER_Y + OVER_H - 40, g_over_hint,
			   NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
	}
	if (g_over_hint2) {
		gfx_text_c(GFX_W / 2, OVER_Y + OVER_H - 22, g_over_hint2,
			   NEXUS_TXT_BODY, t->caption, GFX_OPAQUE);
	}
}

static void tetris_draw(void)
{
	const struct nexus_theme *t = nexus_theme();

	if (gfx_hits(8, gfx_text_h(NEXUS_TXT_CAPTION))) {
		nexus_draw_caption(NEXUS_PAD, 8, "TETRIS");

		/*
		 * One physical button has to mean two things, so the screen has
		 * to say which is which. Without this the only way out of a
		 * game is to already know that a long press exits - a short
		 * press just toggles pause, so it looks like the dongle is
		 * stuck (Section 12).
		 */
		nexus_draw_caption(GFX_W - NEXUS_PAD -
					   gfx_text_w("HOLD=EXIT", NEXUS_TXT_CAPTION),
				   8, "HOLD=EXIT");
	}

	draw_well();
	stat_card(SCORE_Y, SCORE_H, "SCORE", g_t.score, NEXUS_TXT_VALUE,
		  t->value);
	stat_card(LEVEL_Y, LEVEL_H, "LEVEL", g_t.level, NEXUS_TXT_BODY,
		  t->accent);
	stat_card(LINES_Y, LINES_H, "LINES", g_t.lines, NEXUS_TXT_BODY,
		  t->value);
	draw_next();
	draw_overlay();
}

/* ---- events ------------------------------------------------------------ */

/* Translate rules events into sounds, so the game never touches the buzzer
 * and muting is one switch somewhere else entirely (Sections 48, 73). */
static void play_events(uint32_t ev)
{
	if (ev & TETRIS_EV_OVER) {
		nexus_sound_play(NEXUS_SOUND_GAME_OVER);
		return;
	}
	if (ev & TETRIS_EV_TETRIS) {
		nexus_sound_play(NEXUS_SOUND_TETRIS_TETRIS);
	} else if (ev & TETRIS_EV_CLEAR) {
		nexus_sound_play(NEXUS_SOUND_TETRIS_LINE);
	} else if (ev & TETRIS_EV_LEVEL) {
		nexus_sound_play(NEXUS_SOUND_TETRIS_LEVEL);
	} else if (ev & TETRIS_EV_LOCK) {
		nexus_sound_play(NEXUS_SOUND_TETRIS_DROP);
	} else if (ev & TETRIS_EV_ROTATE) {
		nexus_sound_play(NEXUS_SOUND_TETRIS_ROTATE);
	} else if (ev & TETRIS_EV_MOVE) {
		nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
	}
}

static void end_round(void)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_gravity);

	nexus_game_submit_score(&nexus_game_tetris, g_t.score);

	g_over_title = "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_screen_invalidate();
}

static void apply(uint32_t ev)
{
	play_events(ev);

	if (ev == 0) {
		return; /* the move was refused; nothing on screen changed */
	}

	if (g_t.status == TETRIS_GAMEOVER) {
		end_round();
		return;
	}

	/* The well and the stat cards span the whole panel height between
	 * them, so a piece landing is a full repaint either way. A plain move
	 * only touches the well. */
	if (ev & (TETRIS_EV_LOCK | TETRIS_EV_CLEAR | TETRIS_EV_LEVEL)) {
		nexus_screen_invalidate();
	} else {
		nexus_screen_invalidate_rows(FRAME_Y, FRAME_Y + FRAME_H);
	}
}

static void arm_gravity(void)
{
	k_work_reschedule_for_queue(nexus_workq(), &g_gravity,
				    K_MSEC(tetris_gravity_ms(&g_t)));
}

static void gravity_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}

	apply(tetris_step(&g_t));

	if (g_state == NEXUS_GAME_RUNNING) {
		arm_gravity();
	}
}

/* ---- game interface ---------------------------------------------------- */

static void new_round(uint32_t salt)
{
	tetris_init(&g_t, (uint32_t)k_uptime_get_32() ^ salt);
	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_gravity();
	nexus_screen_invalidate();
}

static void tetris_start(void)
{
	new_round(0x9E3779B9u);
}

static void tetris_stop(void)
{
	k_work_cancel_delayable(&g_gravity);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void tetris_pause(void)
{
	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}

	g_state = NEXUS_GAME_PAUSED;
	k_work_cancel_delayable(&g_gravity);
	g_over_title = "PAUSED";
	g_over_hint = "ACTION=RESUME";
	g_over_hint2 = "HOLD=EXIT";
	nexus_screen_invalidate();
}

static void tetris_resume(void)
{
	if (g_state != NEXUS_GAME_PAUSED) {
		return;
	}

	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	g_state = NEXUS_GAME_RUNNING;
	arm_gravity();
	nexus_screen_invalidate();
}

static bool tetris_input(enum nexus_action action)
{
	/*
	 * SELECT is the context verb: it pauses, resumes or restarts depending
	 * on where the round is. That is what makes one physical button enough
	 * (Section 12) without the button knowing any of this.
	 */
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			tetris_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			tetris_resume();
			nexus_sound_play(NEXUS_SOUND_GAME_RESUME);
			return true;
		case NEXUS_GAME_OVER:
			new_round(0x85EBCA6Bu);
			nexus_sound_play(NEXUS_SOUND_GAME_START);
			return true;
		default:
			return false;
		}
	}

	if (g_state != NEXUS_GAME_RUNNING) {
		return false;
	}

	switch (action) {
	case NEXUS_ACTION_LEFT:
	case NEXUS_ACTION_PREVIOUS:
		apply(tetris_move(&g_t, -1));
		return true;
	case NEXUS_ACTION_RIGHT:
	case NEXUS_ACTION_NEXT:
		apply(tetris_move(&g_t, 1));
		return true;
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_ROTATE:
		apply(tetris_rotate(&g_t));
		return true;
	case NEXUS_ACTION_DOWN:
		apply(tetris_soft_drop(&g_t));
		/* A player-driven drop resets the clock, so soft-dropping never
		 * costs you the gravity step you were about to get. */
		if (g_state == NEXUS_GAME_RUNNING) {
			arm_gravity();
		}
		return true;
	case NEXUS_ACTION_DROP:
		apply(tetris_hard_drop(&g_t));
		if (g_state == NEXUS_GAME_RUNNING) {
			arm_gravity();
		}
		return true;
	default:
		return false;
	}
}

static uint32_t tetris_score(void)
{
	return g_t.score;
}

static enum nexus_game_state tetris_state(void)
{
	return g_state;
}

static void tetris_icon(int cx, int cy)
{
	/* Launcher art: an S-piece in its own colours, drawn with the same
	 * block routine the board uses, so the icon costs no asset at all
	 * (Section 101). */
	static const uint8_t art[2][3] = { { 0, 1, 1 }, { 1, 1, 0 } };
	const int s = 16;

	for (int r = 0; r < 2; r++) {
		for (int c = 0; c < 3; c++) {
			if (art[r][c]) {
				draw_block(cx + (c - 1) * s - s / 2,
					   cy + (r - 1) * s, s, 4 /* S */);
			}
		}
	}
}

const struct nexus_game nexus_game_tetris = {
	.id = "tetris",
	.name = "TETRIS",
	.start = tetris_start,
	.input = tetris_input,
	.pause = tetris_pause,
	.resume = tetris_resume,
	.stop = tetris_stop,
	.draw = tetris_draw,
	.score = tetris_score,
	.state = tetris_state,
	.draw_icon = tetris_icon,
};
