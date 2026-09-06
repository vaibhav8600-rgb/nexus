/*
 * Snake (Section 38).
 *
 * A grid game, so it costs almost nothing: the whole board is a direction per
 * cell plus a head and tail index, and the scene is composited straight from
 * that. No framebuffer, same as Tetris.
 *
 * The body is stored as a RING OF CELLS, not as a list of segments. Growing is
 * then "do not advance the tail this step" rather than a memmove of the whole
 * snake every frame, and the occupancy test that matters - "did I just eat
 * myself" - is one array read instead of a walk. At 24x24 cells the ring is
 * 576 bytes and the occupancy grid is another 576, which is the entire RAM
 * cost of the game.
 */

#include <nexus/game.h>
#include <nexus/gfx.h>
#include <nexus/screen.h>
#include <nexus/sound.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "../../nexus_priv.h"

/* ---- board -------------------------------------------------------------- */

#define COLS 24
#define ROWS 24
#define CELL 8

#define WELL_W (COLS * CELL) /* 192 */
#define WELL_H (ROWS * CELL) /* 192 */
#define WELL_X ((GFX_W - WELL_W) / 2)
#define WELL_Y 40
#define FRAME_X (WELL_X - 2)
#define FRAME_Y (WELL_Y - 2)
#define FRAME_W (WELL_W + 4)
#define FRAME_H (WELL_H + 4)

#define START_LEN 4

/* All from Kconfig, so a config repo can tune the feel without touching the
 * module. Lower is faster; the floor stops it outrunning the display. */
#define TICK_START_MS CONFIG_NEXUS_SNAKE_TICK_MS
#define TICK_MIN_MS CONFIG_NEXUS_SNAKE_TICK_MIN_MS
#define SPEED_EVERY CONFIG_NEXUS_SNAKE_SPEED_EVERY
#define SPEED_STEP_MS CONFIG_NEXUS_SNAKE_SPEED_STEP_MS

/* Cell contents. Small enough that the grid is a byte per cell. */
#define EMPTY 0
#define BODY 1
#define FOOD 2

enum dir { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

struct snake {
	uint8_t grid[ROWS][COLS];

	/* Ring of occupied cells, oldest (tail) first. */
	uint16_t ring[ROWS * COLS];
	uint16_t head; /* index of the newest cell   */
	uint16_t tail; /* index of the oldest cell   */
	uint16_t len;

	enum dir dir;
	/*
	 * The direction the next step will take. Input writes here, never to
	 * dir: two taps inside one tick would otherwise let you turn 180 and
	 * eat your own neck, which reads as a bug rather than as a mistake.
	 */
	enum dir next_dir;

	uint16_t food;
	uint32_t score;
	uint16_t eaten;
	uint32_t rng;
};

static struct snake g_s;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_fn);

extern const struct nexus_game nexus_game_snake;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t rnd(void)
{
	/* xorshift32: no libc, no division, good enough to place an apple. */
	g_s.rng ^= g_s.rng << 13;
	g_s.rng ^= g_s.rng >> 17;
	g_s.rng ^= g_s.rng << 5;
	return g_s.rng;
}

static inline uint16_t idx(int r, int c)
{
	return (uint16_t)(r * COLS + c);
}

static void place_food(void)
{
	uint16_t free_cells = (uint16_t)(ROWS * COLS) - g_s.len;

	if (free_cells == 0) {
		g_s.food = 0xFFFF; /* board full: you won, nothing to place */
		return;
	}

	/*
	 * Pick the Nth free cell rather than retrying random positions. With a
	 * long snake, rejection sampling can spin for a very long time on a
	 * nearly-full board, and this runs on the display work queue.
	 */
	uint16_t want = (uint16_t)(rnd() % free_cells);

	for (uint16_t i = 0; i < ROWS * COLS; i++) {
		if (g_s.grid[i / COLS][i % COLS] == EMPTY) {
			if (want-- == 0) {
				g_s.food = i;
				g_s.grid[i / COLS][i % COLS] = FOOD;
				return;
			}
		}
	}
}

static uint32_t tick_ms(void)
{
	uint32_t base = TICK_START_MS;
	uint32_t floor_ms = TICK_MIN_MS;

	/*
	 * The live difficulty setting scales the interval, and it has to move
	 * the FLOOR too. Scaling only the start would mean every setting
	 * converged on the same speed after a dozen apples, which is the one
	 * thing a difficulty knob must not do.
	 *
	 * 3 is neutral and reproduces the Kconfig values exactly. Each step
	 * either side is 20%: interval * (8 - speed) / 5.
	 */
	uint32_t num = (uint32_t)(8 - nexus_game_speed());

	base = base * num / 5U;
	floor_ms = floor_ms * num / 5U;

	uint32_t step = (g_s.eaten / SPEED_EVERY) * SPEED_STEP_MS;

	if (step >= base - floor_ms) {
		return floor_ms;
	}
	return base - step;
}

static void arm_tick(void)
{
	k_work_reschedule_for_queue(nexus_workq(), &g_tick, K_MSEC(tick_ms()));
}

static void end_round(void)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_tick);
	nexus_game_submit_score(&nexus_game_snake, g_s.score);

	g_over_title = "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

static void step(void)
{
	static const int8_t dr[4] = { -1, 0, 1, 0 };
	static const int8_t dc[4] = { 0, 1, 0, -1 };

	g_s.dir = g_s.next_dir;

	uint16_t head_cell = g_s.ring[g_s.head];
	int r = head_cell / COLS + dr[g_s.dir];
	int c = head_cell % COLS + dc[g_s.dir];

	/*
	 * Wrap at the edges - the board is a torus, as it is in snake-module.
	 *
	 * Fatal walls make a 24x24 board very short-lived and turn the game
	 * into a test of not touching the frame rather than of not touching
	 * yourself. With wrapping the only thing that can kill you is your own
	 * body, which is the version of Snake people actually mean.
	 */
	if (IS_ENABLED(CONFIG_NEXUS_SNAKE_WRAP)) {
		if (r < 0) {
			r = ROWS - 1;
		} else if (r >= ROWS) {
			r = 0;
		}
		if (c < 0) {
			c = COLS - 1;
		} else if (c >= COLS) {
			c = 0;
		}
	} else if (r < 0 || r >= ROWS || c < 0 || c >= COLS) {
		end_round();
		return;
	}

	bool ate = (g_s.grid[r][c] == FOOD);

	/*
	 * The tail cell is vacated this step, so moving into it is legal - the
	 * classic "chasing your own tail" case. Freeing it before the
	 * collision test is what makes that work, and testing first is the
	 * bug everyone writes once.
	 */
	if (!ate) {
		uint16_t tail_cell = g_s.ring[g_s.tail];

		g_s.grid[tail_cell / COLS][tail_cell % COLS] = EMPTY;
		g_s.tail = (uint16_t)((g_s.tail + 1) % (ROWS * COLS));
		g_s.len--;
	}

	if (g_s.grid[r][c] == BODY) {
		end_round();
		return;
	}

	g_s.head = (uint16_t)((g_s.head + 1) % (ROWS * COLS));
	g_s.ring[g_s.head] = idx(r, c);
	g_s.grid[r][c] = BODY;
	g_s.len++;

	if (ate) {
		g_s.eaten++;
		g_s.score += 10;
		nexus_sound_play(NEXUS_SOUND_TETRIS_LINE);
		place_food();
	}

	nexus_screen_invalidate_rows(FRAME_Y, FRAME_Y + FRAME_H);
}

static void tick_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}
	step();
	if (g_state == NEXUS_GAME_RUNNING) {
		arm_tick();
	}
}

/* ---- painting ----------------------------------------------------------- */

static void draw_cell(int r, int c, gfx_color fill, bool bevel)
{
	int x = WELL_X + c * CELL;
	int y = WELL_Y + r * CELL;

	gfx_rect(x, y, CELL - 1, CELL - 1, fill, GFX_OPAQUE);
	if (bevel) {
		/* One lit edge. At 8px a full bevel is mud; a single highlight
		 * is enough to stop the body reading as a flat stripe. */
		gfx_hline(x, y, CELL - 1, nexus_theme()->edge_hi, 90);
	}
}

static void snake_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (gfx_hits(8, gfx_text_h(NEXUS_TXT_LABEL))) {
		nexus_draw_label(NEXUS_PAD, 8, "SNAKE");
		nexus_draw_label(GFX_W - NEXUS_PAD -
					 gfx_text_w("HOLD=EXIT", NEXUS_TXT_LABEL),
				 8, "HOLD=EXIT");
	}

	if (gfx_hits(26, gfx_text_h(NEXUS_TXT_BODY))) {
		gfx_text(NEXUS_PAD, 26, "SCORE", NEXUS_TXT_CAPTION, t->caption,
			 GFX_OPAQUE);
		gfx_text(NEXUS_PAD + 40, 24,
			 gfx_utoa(g_s.score, buf, sizeof(buf), 0),
			 NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
	}

	if (gfx_hits(FRAME_Y, FRAME_H)) {
		gfx_round_frame(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, 3,
				t->border, t->border_alpha);
		gfx_rect(WELL_X, WELL_Y, WELL_W, WELL_H, t->track, 120);

		for (int r = 0; r < ROWS; r++) {
			for (int c = 0; c < COLS; c++) {
				uint8_t v = g_s.grid[r][c];

				if (v == BODY) {
					draw_cell(r, c, t->accent, true);
				} else if (v == FOOD) {
					draw_cell(r, c, t->warning, false);
				}
			}
		}

		/* The head, marked so you can see which way you are going
		 * without inferring it from motion. */
		if (g_s.len) {
			uint16_t h = g_s.ring[g_s.head];

			draw_cell(h / COLS, h % COLS, t->value, false);
		}
	}

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_s.score,
				nexus_game_highscore(&nexus_game_snake));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(uint32_t salt)
{
	memset(&g_s, 0, sizeof(g_s));
	g_s.rng = (uint32_t)k_uptime_get_32() ^ salt ^ 0xA5A5A5A5u;
	if (g_s.rng == 0) {
		g_s.rng = 1;
	}

	int r = ROWS / 2;
	int c = COLS / 4;

	g_s.dir = g_s.next_dir = DIR_RIGHT;
	for (int i = 0; i < START_LEN; i++) {
		g_s.ring[i] = idx(r, c + i);
		g_s.grid[r][c + i] = BODY;
	}
	g_s.tail = 0;
	g_s.head = START_LEN - 1;
	g_s.len = START_LEN;

	place_food();

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_tick();
	nexus_screen_invalidate();
}

static void snake_start(void)
{
	new_round(0x27D4EB2Fu);
}

static void snake_stop(void)
{
	k_work_cancel_delayable(&g_tick);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void snake_pause(void)
{
	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}
	g_state = NEXUS_GAME_PAUSED;
	k_work_cancel_delayable(&g_tick);
	g_over_title = "PAUSED";
	g_over_hint = "ACTION=RESUME";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(NEXUS_SOUND_GAME_PAUSE);
	nexus_screen_invalidate();
}

static void snake_resume(void)
{
	if (g_state != NEXUS_GAME_PAUSED) {
		return;
	}
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	g_state = NEXUS_GAME_RUNNING;
	arm_tick();
	nexus_screen_invalidate();
}

static void turn(enum dir d)
{
	static const enum dir opposite[4] = { DIR_DOWN, DIR_LEFT, DIR_UP,
					      DIR_RIGHT };

	if (g_state != NEXUS_GAME_RUNNING || d == opposite[g_s.dir]) {
		return;
	}
	g_s.next_dir = d;
}

static bool snake_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			snake_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			snake_resume();
			nexus_sound_play(NEXUS_SOUND_GAME_RESUME);
			return true;
		case NEXUS_GAME_OVER:
			new_round(0xC2B2AE35u);
			nexus_sound_play(NEXUS_SOUND_GAME_START);
			return true;
		default:
			return false;
		}
	}

	switch (action) {
	case NEXUS_ACTION_UP:
	/* I is bound to ROTATE on the game layer, because Tetris needs it
	 * there. In a game with no rotation the obvious reading of the top key
	 * in an IJKL cluster is "up", and leaving it inert made I look
	 * broken. */
	case NEXUS_ACTION_ROTATE:
		turn(DIR_UP);
		break;
	case NEXUS_ACTION_DOWN:
		turn(DIR_DOWN);
		break;
	case NEXUS_ACTION_LEFT:
		turn(DIR_LEFT);
		break;
	case NEXUS_ACTION_RIGHT:
		turn(DIR_RIGHT);
		break;
	default:
		return false;
	}

	nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
	return true;
}

static uint32_t snake_score(void)
{
	return g_s.score;
}

static enum nexus_game_state snake_state(void)
{
	return g_state;
}

static void snake_icon(int cx, int cy)
{
	const struct nexus_theme *t = nexus_theme();
	const int c = 9;

	/* Three body cells and an apple, the smallest picture that says
	 * "snake" rather than "some squares". */
	for (int i = 0; i < 3; i++) {
		gfx_rect(cx - 26 + i * (c + 2), cy - c / 2, c, c, t->accent,
			 GFX_OPAQUE);
	}
	gfx_rect(cx + 7, cy - c / 2, c, c, t->value, GFX_OPAQUE);
	gfx_disc(cx + 26, cy, c / 2, t->warning, GFX_OPAQUE);
}

const struct nexus_game nexus_game_snake = {
	.id = "snake",
	.name = "SNAKE",
	.start = snake_start,
	.input = snake_input,
	.pause = snake_pause,
	.resume = snake_resume,
	.stop = snake_stop,
	.draw = snake_draw,
	.score = snake_score,
	.state = snake_state,
	.draw_icon = snake_icon,
};
