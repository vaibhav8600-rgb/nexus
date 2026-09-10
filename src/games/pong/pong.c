/*
 * Pong (Section 38).
 *
 * The cheapest game here by a wide margin: two paddles, a ball, and no board
 * at all. Roughly 30 bytes of state, and the dirty region is a band around
 * three moving objects rather than a playfield - which is why it runs at the
 * panel's ceiling and not below it.
 *
 * Positions are 8.8 fixed point in an int32_t. int16_t holds 8.8 only to
 * +/-128 px and this field is 240 wide, which is the trap Breakout fell into
 * once already.
 *
 * The opponent is deliberately beatable. A paddle that tracks the ball exactly
 * never loses, and a game you cannot win is a screensaver - so it moves at a
 * fraction of the ball's speed and only reacts once the ball is coming at it.
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

typedef int32_t fix_t;
#define FIX 8
#define TO_FIX(v) ((fix_t)(v) << FIX)
#define TO_PX(v) ((int)((v) >> FIX))

#define FIELD_X 8
#define FIELD_Y 46
#define FIELD_W 224
#define FIELD_H 176
#define FIELD_R (FIELD_X + FIELD_W)
#define FIELD_B (FIELD_Y + FIELD_H)


#define PAD_W 10
#define PAD_H 44
#define PAD_INSET 6
#define BALL_R 8

#define WIN_SCORE 7

#define TICK_MS CONFIG_NEXUS_PONG_TICK_MS
#define BALL_SPEED ((fix_t)CONFIG_NEXUS_PONG_BALL_SPEED * 256 / 100)
#define PAD_STEP CONFIG_NEXUS_PONG_PADDLE_STEP

/*
 * The opponent, as a fraction of the ball's own vertical speed.
 *
 * This used to be PAD_STEP / CPU_LAG - a constant 4 px per tick, which was
 * FASTER than the ball's steepest vy. It could not be beaten by aiming, only
 * outlasted, so every rally ran until someone got bored: the real reason the
 * game felt slow was not the ball, it was that the point never ended.
 *
 * Two thirds of the ball's speed means a steep return crosses faster than the
 * paddle can follow, which is what makes an angle worth playing. It is a
 * fraction rather than a constant so it keeps tracking the ball if the speed
 * knob moves.
 */
#define CPU_STEP (TO_PX(speed()) * 2 / 3 + 1)

struct pong {
	fix_t bx, by;
	fix_t vx, vy;
	fix_t you;  /* paddle top edge, left side  */
	fix_t cpu;  /* paddle top edge, right side */
	uint8_t score_you;
	uint8_t score_cpu;
	uint8_t level;    /* points played, +1 - the ball rises with it */
	uint8_t serve;   /* ticks before the ball is released */
	uint32_t rng;
};

static struct pong g_g;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_fn);

extern const struct nexus_game nexus_game_pong;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t rnd(void)
{
	g_g.rng ^= g_g.rng << 13;
	g_g.rng ^= g_g.rng >> 17;
	g_g.rng ^= g_g.rng << 5;
	return g_g.rng;
}

/*
 * Pong has no board to clear, so its level is the point you are on: every
 * point played makes the next ball faster, and the match to WIN_SCORE is
 * therefore a short difficulty curve rather than seven identical rallies.
 *
 * It reads the level rather than the score so that losing a point advances it
 * too - the game gets harder as it goes on, whoever is winning.
 */
static fix_t speed(void)
{
	/* Same live-difficulty curve Breakout uses: 3 is neutral, each step
	 * either side is 20%, and it scales a VELOCITY so faster is bigger. */
	fix_t v = BALL_SPEED * (2 + nexus_game_speed()) / 5 *
		  (fix_t)nexus_game_level_pct(g_g.level) / 100;
	/*
	 * Clamped to under the paddle's own width plus the ball: the hit test
	 * is a point test at the ball's centre against the paddle plane, and a
	 * tick longer than that crosses the plane and the goal line in the
	 * same step. At LUDICROUS on a late point the unclamped value is
	 * 20.5 px against an 18 px window.
	 */
	const fix_t cap = TO_FIX(PAD_W + BALL_R - 1);

	return v > cap ? cap : v;
}

static void serve(int towards)
{
	g_g.bx = TO_FIX(FIELD_X + FIELD_W / 2);
	g_g.by = TO_FIX(FIELD_Y + FIELD_H / 2);
	g_g.vx = towards > 0 ? speed() : -speed();
	/* A slight vertical component, randomised, so no two rallies are the
	 * same and a serve is never a straight line you can leave. */
	g_g.vy = (fix_t)((int)(rnd() % 3) - 1) * speed() / 2;
	if (g_g.vy == 0) {
		g_g.vy = speed() / 3;
	}
	g_g.serve = 10;
	if (g_g.level < 255) {
		g_g.level++;
	}
}

static void arm_tick(void)
{
	k_work_reschedule_for_queue(nexus_workq(), &g_tick, K_MSEC(TICK_MS));
}

static void end_round(bool won)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_tick);
	nexus_game_submit_score(&nexus_game_pong, g_g.score_you);

	g_over_title = won ? "YOU WIN" : "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(won ? NEXUS_SOUND_TETRIS_TETRIS : NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

static void clamp_pad(fix_t *p)
{
	if (*p < TO_FIX(FIELD_Y)) {
		*p = TO_FIX(FIELD_Y);
	}
	if (*p > TO_FIX(FIELD_B - PAD_H)) {
		*p = TO_FIX(FIELD_B - PAD_H);
	}
}

/* ---- simulation --------------------------------------------------------- */

static bool paddle_hit(int px, int py, fix_t pad)
{
	int top = TO_PX(pad);

	return py + BALL_R >= top && py - BALL_R <= top + PAD_H &&
	       px >= FIELD_X && px <= FIELD_R;
}

static void bounce_off(fix_t pad, int dir)
{
	int top = TO_PX(pad);
	int off = TO_PX(g_g.by) - (top + PAD_H / 2); /* -20..+20 */

	g_g.vx = dir > 0 ? speed() : -speed();
	/*
	 * Where it lands on the paddle steers it, exactly as in Breakout.
	 * Without this the angle never changes and a rally becomes a
	 * metronome that neither player can influence.
	 */
	g_g.vy = (fix_t)off * speed() / (PAD_H / 2);
	nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
}

static void step(void)
{
	if (g_g.serve) {
		g_g.serve--;
		nexus_screen_invalidate_rows(FIELD_Y, FIELD_B);
		return;
	}

	/*
	 * The opponent, one lag step behind and capped below the ball's own
	 * speed. Tracking exactly would make it unbeatable; this loses when
	 * the ball is steep and it has to cross the field.
	 */
	int want = TO_PX(g_g.by) - PAD_H / 2;
	int cpu = TO_PX(g_g.cpu);

	if (g_g.vx > 0) {
		int d = want - cpu;
		int step = CPU_STEP;

		if (d > step) {
			d = step;
		}
		if (d < -step) {
			d = -step;
		}
		g_g.cpu += TO_FIX(d);
		clamp_pad(&g_g.cpu);
	}

	g_g.bx += g_g.vx;
	g_g.by += g_g.vy;

	int px = TO_PX(g_g.bx);
	int py = TO_PX(g_g.by);

	if (py - BALL_R <= FIELD_Y) {
		g_g.by = TO_FIX(FIELD_Y + BALL_R);
		g_g.vy = -g_g.vy;
		nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
	} else if (py + BALL_R >= FIELD_B) {
		g_g.by = TO_FIX(FIELD_B - BALL_R);
		g_g.vy = -g_g.vy;
		nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
	}

	px = TO_PX(g_g.bx);
	py = TO_PX(g_g.by);

	int you_x = FIELD_X + PAD_INSET + PAD_W;
	int cpu_x = FIELD_R - PAD_INSET - PAD_W;

	if (g_g.vx < 0 && px - BALL_R <= you_x &&
	    paddle_hit(px, py, g_g.you)) {
		g_g.bx = TO_FIX(you_x + BALL_R);
		bounce_off(g_g.you, +1);
	} else if (g_g.vx > 0 && px + BALL_R >= cpu_x &&
		   paddle_hit(px, py, g_g.cpu)) {
		g_g.bx = TO_FIX(cpu_x - BALL_R);
		bounce_off(g_g.cpu, -1);
	}

	bool scored = false;

	if (TO_PX(g_g.bx) < FIELD_X) {
		g_g.score_cpu++;
		scored = true;
		nexus_sound_play(NEXUS_SOUND_BACK);
		if (g_g.score_cpu >= WIN_SCORE) {
			end_round(false);
			return;
		}
		serve(+1);
	} else if (TO_PX(g_g.bx) > FIELD_R) {
		g_g.score_you++;
		scored = true;
		nexus_sound_play(NEXUS_SOUND_TETRIS_LINE);
		if (g_g.score_you >= WIN_SCORE) {
			end_round(true);
			return;
		}
		serve(-1);
	}

	if (scored) {
		nexus_screen_invalidate_rows(NEXUS_HUD_ROW_Y,
				     NEXUS_HUD_ROW_Y +
					     NEXUS_HUD_ROW_H);
	}
	nexus_screen_invalidate_rows(FIELD_Y, FIELD_B);
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

static void pong_draw(void)
{
	const struct nexus_theme *t = nexus_theme();

	/* Your score where every other game puts it and in the same colour,
	 * with the opponent's beside it in the red of the paddle it belongs
	 * to - which is the job the old two-numerals-either-side-of-centre
	 * layout was doing, in a third of the width. */
	const struct nexus_hud hud = {
		.title = "PONG",
		.score = g_g.score_you,
		.rival = g_g.score_cpu,
		.level = g_g.level,
	};

	nexus_draw_game_header(&hud);

	if (!gfx_hits(FIELD_Y - 2, FIELD_H + 4)) {
		return;
	}

	gfx_round_frame(FIELD_X - 2, FIELD_Y - 2, FIELD_W + 4, FIELD_H + 4,
			t->radius, t->border, t->border_alpha);
	nexus_draw_field(FIELD_X, FIELD_Y, FIELD_W, FIELD_H);

	/* Dashed centre line, behind everything. */
	for (int y = FIELD_Y + 6; y < FIELD_B - 4; y += 14) {
		gfx_rect(GFX_W / 2 - 1, y, 3, 8, t->border, 90);
	}

	nexus_draw_block(FIELD_X + PAD_INSET, TO_PX(g_g.you), PAD_W, PAD_H, 3,
			 t->accent);
	nexus_draw_block(FIELD_R - PAD_INSET - PAD_W, TO_PX(g_g.cpu), PAD_W,
			 PAD_H, 3, t->error);

	if (!g_g.serve || (g_g.serve & 2)) {
		/* Blinks while the serve counts down, so the restart reads as
		 * deliberate rather than as the ball having vanished. */
		nexus_draw_orb(TO_PX(g_g.bx), TO_PX(g_g.by), BALL_R,
			       t->warning);
	}

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_g.score_you,
				nexus_game_highscore(&nexus_game_pong));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(void)
{
	memset(&g_g, 0, sizeof(g_g));
	g_g.rng = (uint32_t)k_uptime_get_32() ^ 0x2545F491u;
	if (g_g.rng == 0) {
		g_g.rng = 1;
	}
	g_g.you = TO_FIX(FIELD_Y + (FIELD_H - PAD_H) / 2);
	g_g.cpu = g_g.you;
	/* serve() bumps it, so the first ball of a match is level 1. */
	g_g.level = 0;
	serve(+1);

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_tick();
	nexus_screen_invalidate();
}

static void pong_start(void)
{
	new_round();
}

static void pong_stop(void)
{
	k_work_cancel_delayable(&g_tick);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void pong_pause(void)
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

static void pong_resume(void)
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

static void move_you(int delta)
{
	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}
	g_g.you += TO_FIX(delta);
	clamp_pad(&g_g.you);
	nexus_screen_invalidate_rows(FIELD_Y, FIELD_B);
}

static bool pong_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			pong_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			pong_resume();
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
	/*
	 * The paddle moves vertically, so it is I and K that drive it - but J
	 * and L are what a player reaches for on a horizontal cluster, and
	 * accepting both costs nothing and removes a whole class of "the keys
	 * do not work" the first time someone plays.
	 */
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_ROTATE:
	case NEXUS_ACTION_LEFT:
		move_you(-PAD_STEP);
		return true;
	case NEXUS_ACTION_DOWN:
	case NEXUS_ACTION_RIGHT:
		move_you(PAD_STEP);
		return true;
	default:
		return false;
	}
}

static uint32_t pong_score(void)
{
	return g_g.score_you;
}

static enum nexus_game_state pong_state(void)
{
	return g_state;
}

static void pong_icon(int cx, int cy)
{
	const struct nexus_theme *t = nexus_theme();

	nexus_draw_block(cx - 30, cy - 14, 6, 28, 2, t->accent);
	nexus_draw_block(cx + 24, cy - 6, 6, 28, 2, t->error);
	nexus_draw_orb(cx, cy, 6, t->warning);
}

const struct nexus_game nexus_game_pong = {
	.id = "pong",
	.name = "PONG",
	.start = pong_start,
	.input = pong_input,
	.pause = pong_pause,
	.resume = pong_resume,
	.stop = pong_stop,
	.draw = pong_draw,
	.score = pong_score,
	.state = pong_state,
	.draw_icon = pong_icon,
};
