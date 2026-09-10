/*
 * Breakout (Section 38).
 *
 * Unlike Tetris and Snake this one does not live on a grid: the ball moves in
 * fractions of a pixel, so positions are 8.8 FIXED POINT in an int32_t - the
 * low 8 bits are the fraction. Integer-per-frame motion would force the ball
 * to travel at least one pixel per tick, which at any playable frame rate is
 * far too fast, and floating point on a Cortex-M4 in a display work queue
 * handler is not a trade worth making for a paddle game.
 *
 * The int32_t is not incidental - see the note on fix_t below.
 *
 * The bricks are a bitmask per row, not a byte array: eight columns fit one
 * uint8_t, "any left in this row" is a compare against zero, and "did I clear
 * the board" is an OR of five bytes.
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

/* ---- geometry ----------------------------------------------------------- */

/*
 * 8.8 fixed point in an int32_t, and the width matters.
 *
 * This was int16_t, which holds 8.8 values from -128.0 to +127.996 - and the
 * field runs to x=230 with the paddle at y=210. Every position past 127
 * overflowed and wrapped negative, so the ball vanished off one edge and
 * reappeared at a nonsense coordinate, and reset_ball() parked it at -46px
 * where no paddle could ever reach it. Three reported symptoms, one type.
 *
 * int32_t gives +/- 8 million pixels at the same precision, for four bytes
 * more of a struct that has one instance.
 */
typedef int32_t fix_t;

#define FIX 8
#define TO_FIX(v) ((fix_t)(v) << FIX)
#define TO_PX(v) ((int)((v) >> FIX))

#define FIELD_X 10
#define FIELD_Y 40
#define FIELD_W 220
#define FIELD_H 180
#define FIELD_R (FIELD_X + FIELD_W)
#define FIELD_B (FIELD_Y + FIELD_H)

#define BRICK_COLS 8
#define BRICK_ROWS 5
#define BRICK_W (FIELD_W / BRICK_COLS) /* 27 */
#define BRICK_H 9
#define BRICK_TOP (FIELD_Y + 6)

#define PADDLE_W 38
#define PADDLE_H 5
#define PADDLE_Y (FIELD_B - 10)
/*
 * Pixels per key repeat, and it has to be this large.
 *
 * A held key repeats every CONFIG_NEXUS_ACTION_REPEAT_MS, so 12px a step
 * crossed the field in about 1.3 seconds - which in a paddle game means the
 * ball reaches the corner first, every time. It is a Kconfig because the right
 * number depends on the repeat rate it is paired with.
 */
#define PADDLE_STEP CONFIG_NEXUS_BREAKOUT_PADDLE_STEP

/* 5, not 3. A 6px ball was the smallest moving thing in any game here and
 * the first to disappear at any distance; 10px still clears the paddle. */
#define BALL_R 5

/*
 * Speed and tick rate come from Kconfig so they can be tuned from a config
 * repo without touching the module. Speed is in HUNDREDTHS of a pixel per
 * tick: at 8.8 the useful range is well under one pixel of granularity, and
 * an integer pixels-per-tick knob would only offer 1, 2, 3 - which is the
 * difference between sedate and unplayable with nothing in between.
 */
#define BALL_SPEED_BASE ((fix_t)CONFIG_NEXUS_BREAKOUT_BALL_SPEED * 256 / 100)

/*
 * Scaled by the live difficulty setting: (2 + speed) / 5, so 3 is neutral and
 * reproduces the Kconfig value exactly, and each step either side is 20% -
 * the same magnitude Snake and Tetris get.
 *
 * Not speed/3, which was the obvious form and wrong: it put SLOW at a third
 * of normal, about one pixel a tick, which is not a difficulty setting but a
 * broken game. Velocity also scales the OPPOSITE way to Snake's interval for
 * the same word "faster", which is why the two expressions do not match.
 */
#define BALL_SPEED ball_speed()

#define TICK_MS CONFIG_NEXUS_BREAKOUT_TICK_MS
#define LIVES 3

struct breakout {
	uint8_t bricks[BRICK_ROWS]; /* bit per column, 1 = still there */

	fix_t bx, by;     /* ball centre, 8.8   */
	fix_t vx, vy;     /* ball velocity, 8.8 */
	int16_t paddle_x; /* left edge, whole pixels - no fraction needed */

	uint32_t score;
	uint8_t level;
	uint8_t lives;
	bool launched;
};

static struct breakout g_b;

/*
 * ... and clamped to under one brick row.
 *
 * hit_bricks() tests the ball's CENTRE against a single cell, so a tick
 * longer than BRICK_H steps clean through a brick without ever being inside
 * it - the ball crosses the wall and nothing happens, which reads as the game
 * being broken rather than as being fast. At LUDICROUS on a late board the
 * unclamped value is 11.2 px against a 9 px row, so this is not theoretical.
 *
 * ponytail: clamped rather than swept. If the ball ever needs to go faster
 * than a brick row, hit_bricks() has to test the segment from the previous
 * centre, not the new one.
 */
static inline fix_t ball_speed(void)
{
	fix_t v = BALL_SPEED_BASE * (2 + nexus_game_speed()) / 5 *
		  (fix_t)nexus_game_level_pct(g_b.level) / 100;
	const fix_t cap = TO_FIX(BRICK_H - 1);

	return v > cap ? cap : v;
}
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_fn);

extern const struct nexus_game nexus_game_breakout;

/* ---- helpers ------------------------------------------------------------ */

static void arm_tick(void)
{
	k_work_reschedule_for_queue(nexus_workq(), &g_tick, K_MSEC(TICK_MS));
}

static bool bricks_left(void)
{
	uint8_t any = 0;

	for (int r = 0; r < BRICK_ROWS; r++) {
		any |= g_b.bricks[r];
	}
	return any != 0;
}

/* Park the ball on the paddle, waiting for a launch. */
static void reset_ball(void)
{
	g_b.launched = false;
	g_b.bx = TO_FIX(g_b.paddle_x + PADDLE_W / 2);
	g_b.by = TO_FIX(PADDLE_Y - BALL_R - 1);
	g_b.vx = BALL_SPEED / 2;
	g_b.vy = -BALL_SPEED;
}

/*
 * Clearing the wall is no longer the end of the game - it is the end of a
 * board. The score, the lives and the level carry; the wall comes back and
 * the ball is faster, because BALL_SPEED reads the level.
 *
 * Losing the last life is still the only ending, which is what makes the
 * level worth printing: it is how far you got, not whether you finished.
 */
static void end_round(void)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_tick);
	nexus_game_submit_score(&nexus_game_breakout, g_b.score);

	g_over_title = "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

static void next_board(void)
{
	if (g_b.level < 255) {
		g_b.level++;
	}
	for (int r = 0; r < BRICK_ROWS; r++) {
		g_b.bricks[r] = (uint8_t)((1u << BRICK_COLS) - 1u);
	}
	reset_ball();
	nexus_sound_play(NEXUS_SOUND_TETRIS_LEVEL);
	nexus_screen_invalidate();
}

/*
 * One brick hit test against the ball's new centre. Returns true if a brick
 * was destroyed, and flips vy - which is the cheap approximation: a real
 * Breakout picks the reflection axis from which face was crossed, but at this
 * brick size and ball speed the difference is not visible and the exact
 * version needs the previous position kept for every axis.
 *
 * ponytail: vertical-only reflection. Track the entry face if the ball ever
 * starts tunnelling along a brick row.
 */
static bool hit_bricks(int px, int py)
{
	if (py < BRICK_TOP || py >= BRICK_TOP + BRICK_ROWS * BRICK_H) {
		return false;
	}

	int row = (py - BRICK_TOP) / BRICK_H;
	int col = (px - FIELD_X) / BRICK_W;

	if (row < 0 || row >= BRICK_ROWS || col < 0 || col >= BRICK_COLS) {
		return false;
	}
	if (!(g_b.bricks[row] & BIT(col))) {
		return false;
	}

	g_b.bricks[row] &= (uint8_t)~BIT(col);
	/* Top rows are worth more, which is the only reason to aim. */
	g_b.score += (uint32_t)(BRICK_ROWS - row) * 10U;
	g_b.vy = -g_b.vy;
	nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
	nexus_screen_invalidate_rows(NEXUS_HUD_ROW_Y,
				     NEXUS_HUD_ROW_Y +
					     NEXUS_HUD_ROW_H);
	return true;
}

static void step(void)
{
	if (!g_b.launched) {
		/* Glued to the paddle until launch, so the ball follows it. */
		g_b.bx = TO_FIX(g_b.paddle_x + PADDLE_W / 2);
		return;
	}

	g_b.bx += g_b.vx;
	g_b.by += g_b.vy;

	int px = TO_PX(g_b.bx);
	int py = TO_PX(g_b.by);

	/* Walls. */
	if (px - BALL_R <= FIELD_X) {
		g_b.bx = TO_FIX(FIELD_X + BALL_R);
		g_b.vx = -g_b.vx;
	} else if (px + BALL_R >= FIELD_R) {
		g_b.bx = TO_FIX(FIELD_R - BALL_R);
		g_b.vx = -g_b.vx;
	}
	if (py - BALL_R <= FIELD_Y) {
		g_b.by = TO_FIX(FIELD_Y + BALL_R);
		g_b.vy = -g_b.vy;
	}

	px = TO_PX(g_b.bx);
	py = TO_PX(g_b.by);

	if (hit_bricks(px, py) && !bricks_left()) {
		next_board();
		return;
	}

	/*
	 * Paddle.
	 *
	 * The catch window has to be at least as deep as one tick of travel,
	 * or a fast ball steps straight over the paddle between frames and the
	 * life is lost to a collision test that never ran. A fixed +2 was
	 * enough at the old top speed and is not at LUDICROUS, so it scales
	 * with the ball instead of being a number that happens to work.
	 *
	 * ponytail: still a point test, not a swept one. If the ball ever gets
	 * fast enough to clear PADDLE_H + its own travel in a tick, this needs
	 * the previous position rather than a deeper window.
	 */
	int reach = PADDLE_H + 2 + TO_PX(BALL_SPEED);

	if (g_b.vy > 0 && py + BALL_R >= PADDLE_Y &&
	    py + BALL_R <= PADDLE_Y + reach &&
	    px >= g_b.paddle_x && px <= g_b.paddle_x + PADDLE_W) {
		g_b.by = TO_FIX(PADDLE_Y - BALL_R);
		g_b.vy = -g_b.vy;

		/*
		 * Where it lands on the paddle steers it. Without this the
		 * angle never changes and the game becomes a metronome you
		 * cannot influence - this one line is most of what makes
		 * Breakout a game rather than a screensaver.
		 */
		int off = px - (g_b.paddle_x + PADDLE_W / 2); /* -19..+19 */

		g_b.vx = (fix_t)off * BALL_SPEED / (PADDLE_W / 2);
		nexus_sound_play(NEXUS_SOUND_TETRIS_ROTATE);
	}

	/* Missed. */
	if (py - BALL_R > FIELD_B) {
		if (--g_b.lives == 0) {
			end_round();
			return;
		}
		nexus_sound_play(NEXUS_SOUND_BACK);
		reset_ball();
		/* A life just went, so the pips have to redraw too. */
		nexus_screen_invalidate_rows(NEXUS_HUD_ROW_Y,
				     NEXUS_HUD_ROW_Y +
					     NEXUS_HUD_ROW_H);
	}

	nexus_screen_invalidate_rows(FIELD_Y, FIELD_B + 2);
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

static void breakout_draw(void)
{
	const struct nexus_theme *t = nexus_theme();

	/* Lives as pips, not a number: you glance at them mid-rally. */
	const struct nexus_hud hud = {
		.title = "BREAKOUT",
		.score = g_b.score,
		.rival = -1,
		.level = g_b.level,
		.lives = g_b.lives,
		.life = t->accent,
	};

	nexus_draw_game_header(&hud);

	if (!gfx_hits(FIELD_Y - 2, FIELD_H + 6)) {
		return;
	}

	nexus_draw_field(FIELD_X, FIELD_Y, FIELD_W, FIELD_H);
	gfx_round_frame(FIELD_X - 2, FIELD_Y - 2, FIELD_W + 4, FIELD_H + 4,
			t->radius, t->border, t->border_alpha);

	for (int r = 0; r < BRICK_ROWS; r++) {
		if (g_b.bricks[r] == 0) {
			continue;
		}
		for (int c = 0; c < BRICK_COLS; c++) {
			if (!(g_b.bricks[r] & BIT(c))) {
				continue;
			}

			int x = FIELD_X + c * BRICK_W;
			int y = BRICK_TOP + r * BRICK_H;
			/* Colour by row, so the board reads as ranked value
			 * rather than as a wall of one colour. */
			gfx_color col = gfx_mix(t->accent, t->accent_alt,
						(uint8_t)(r * 255 /
							  (BRICK_ROWS - 1)));

			gfx_rect(x + 1, y + 1, BRICK_W - 2, BRICK_H - 2, col,
				 GFX_OPAQUE);
			gfx_hline(x + 1, y + 1, BRICK_W - 2, t->edge_hi,
				  t->edge_hi_alpha);
		}
	}

	gfx_round_rect(g_b.paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H,
		       PADDLE_H / 2, t->value, GFX_OPAQUE);

	gfx_disc(TO_PX(g_b.bx), TO_PX(g_b.by), BALL_R, t->warning, GFX_OPAQUE);

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_b.score,
				nexus_game_highscore(&nexus_game_breakout));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(void)
{
	memset(&g_b, 0, sizeof(g_b));

	for (int r = 0; r < BRICK_ROWS; r++) {
		g_b.bricks[r] = (uint8_t)((1u << BRICK_COLS) - 1u);
	}
	g_b.paddle_x = FIELD_X + (FIELD_W - PADDLE_W) / 2;
	g_b.lives = LIVES;
	g_b.level = 1;
	g_b.score = 0;
	reset_ball();

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_tick();
	nexus_screen_invalidate();
}

static void breakout_start(void)
{
	new_round();
}

static void breakout_stop(void)
{
	k_work_cancel_delayable(&g_tick);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void breakout_pause(void)
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

static void breakout_resume(void)
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

static void move_paddle(int delta)
{
	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}

	int x = g_b.paddle_x + delta;

	if (x < FIELD_X) {
		x = FIELD_X;
	}
	if (x > FIELD_R - PADDLE_W) {
		x = FIELD_R - PADDLE_W;
	}
	g_b.paddle_x = (int16_t)x;
	nexus_screen_invalidate_rows(FIELD_Y, FIELD_B + 2);
}

static bool breakout_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			/*
			 * The action button launches a parked ball before it
			 * pauses. Otherwise the only way to start a life is a
			 * direction key, and on the physical button - which is
			 * all some users have - the game would be unstartable.
			 */
			if (!g_b.launched) {
				g_b.launched = true;
				nexus_sound_play(NEXUS_SOUND_GAME_START);
				return true;
			}
			breakout_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			breakout_resume();
			nexus_sound_play(NEXUS_SOUND_GAME_RESUME);
			return true;
		case NEXUS_GAME_OVER:
			new_round();
			nexus_sound_play(NEXUS_SOUND_GAME_START);
			return true;
		default:
			return false;
		}
	}

	switch (action) {
	case NEXUS_ACTION_LEFT:
		move_paddle(-PADDLE_STEP);
		return true;
	case NEXUS_ACTION_RIGHT:
		move_paddle(PADDLE_STEP);
		return true;
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_DROP:
	/* I is bound to ROTATE on the game layer, because Tetris needs it
	 * there. Accepting it as "launch" means the same key does the
	 * expected thing in every game rather than being inert in two. */
	case NEXUS_ACTION_ROTATE:
		if (g_state == NEXUS_GAME_RUNNING && !g_b.launched) {
			g_b.launched = true;
			nexus_sound_play(NEXUS_SOUND_GAME_START);
		}
		return true;
	default:
		return false;
	}
}

static uint32_t breakout_score(void)
{
	return g_b.score;
}

static enum nexus_game_state breakout_state(void)
{
	return g_state;
}

static void breakout_icon(int cx, int cy)
{
	const struct nexus_theme *t = nexus_theme();

	for (int r = 0; r < 2; r++) {
		for (int c = 0; c < 4; c++) {
			gfx_rect(cx - 30 + c * 16, cy - 22 + r * 10, 14, 8,
				 gfx_mix(t->accent, t->accent_alt,
					 (uint8_t)(r * 160)),
				 GFX_OPAQUE);
		}
	}
	gfx_disc(cx + 4, cy + 2, 4, t->warning, GFX_OPAQUE);
	gfx_round_rect(cx - 14, cy + 18, 30, 5, 2, t->value, GFX_OPAQUE);
}

const struct nexus_game nexus_game_breakout = {
	.id = "breakout",
	.name = "BREAKOUT",
	.start = breakout_start,
	.input = breakout_input,
	.pause = breakout_pause,
	.resume = breakout_resume,
	.stop = breakout_stop,
	.draw = breakout_draw,
	.score = breakout_score,
	.state = breakout_state,
	.draw_icon = breakout_icon,
};
