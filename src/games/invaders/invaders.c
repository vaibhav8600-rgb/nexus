/*
 * Space Invaders (Section 38).
 *
 * A grid of aliens that steps sideways in lockstep and drops a row at the
 * wall, one cannon, and a handful of shots. The formation is a BITMASK PER
 * ROW - five rows of eleven fits in a uint16_t each - so "how many are left"
 * is five popcounts and "did that shot hit" is one bit test rather than a
 * search through an array of structs.
 *
 * The formation does not each move on its own clock. It has one position and
 * one direction, and every alien is drawn at an offset from it, which is why
 * a fleet of 55 costs about the same as a single sprite to simulate.
 *
 * Speed rises as the fleet thins, which is the whole arc of the original: the
 * last alien is fast because there are fewer of them to step, not because
 * anything ramps a difficulty variable.
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

#define COLS 8
#define ROWS 4
#define ALIEN_W 24
#define ALIEN_H 18
#define STEP_X 6
#define STEP_Y 12

#define FIELD_X 6
#define FIELD_Y 44
#define FIELD_W 228
#define FIELD_H 180
#define FIELD_R (FIELD_X + FIELD_W)
#define FIELD_B (FIELD_Y + FIELD_H)

#define HUD_Y 20
#define HUD_H 22

#define CANNON_W 28
#define CANNON_H 14
#define CANNON_Y (FIELD_B - CANNON_H - 2)
#define CANNON_STEP 9

#define SHOT_W 4
#define SHOT_H 10
#define MAX_BOMBS 3

/*
 * Auto-fire. The original allowed one shot at a time, which is what made
 * missing cost you something - but that rule was written for a cabinet with a
 * dedicated fire button, and here the button repeats at
 * CONFIG_NEXUS_ACTION_REPEAT_MS. Holding it under the old rule produced a
 * shot, a long wait while it flew the length of the field, then another: the
 * gun felt broken rather than deliberate.
 *
 * So: hold to fire, up to MAX_SHOTS in the air, one every SHOT_COOL ticks.
 * The cap is what keeps it a game - three in flight is a stream you have to
 * aim, not a wall that clears the screen on its own.
 */
#define MAX_SHOTS 3
#define SHOT_COOL 4

#define LIVES 3
#define TICK_MS CONFIG_NEXUS_INVADERS_TICK_MS

/* Ticks between formation steps, fastest and slowest. */
#define MARCH_SLOW 16
#define MARCH_FAST 2

struct shot {
	int16_t x, y;
	bool live;
};

struct fleet {
	uint16_t row[ROWS]; /* bit per column, 1 = still there */
	int16_t fx, fy;     /* formation origin, pixels */
	int8_t dir;
	uint8_t march;      /* ticks until the next step */
	uint8_t left;       /* aliens remaining */

	int16_t cannon;
	struct shot shot[MAX_SHOTS]; /* yours */
	struct shot bomb[MAX_BOMBS];
	uint8_t cool;                /* ticks until the gun will fire again */

	uint32_t score;
	uint8_t lives;
	uint8_t anim;
	uint32_t rng;
};

static struct fleet g_f;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_fn);

extern const struct nexus_game nexus_game_invaders;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t rnd(void)
{
	g_f.rng ^= g_f.rng << 13;
	g_f.rng ^= g_f.rng >> 17;
	g_f.rng ^= g_f.rng << 5;
	return g_f.rng;
}

static inline bool alive(int r, int c)
{
	return (g_f.row[r] & (1u << c)) != 0;
}

static inline int alien_x(int c)
{
	return g_f.fx + c * (ALIEN_W + 4);
}

static inline int alien_y(int r)
{
	return g_f.fy + r * (ALIEN_H + 6);
}

/* How long until the next formation step. Fewer aliens, faster fleet. */
static uint8_t march_period(void)
{
	int total = ROWS * COLS;
	int span = MARCH_SLOW - MARCH_FAST;
	uint8_t p = (uint8_t)(MARCH_FAST + (int)g_f.left * span / total);

	/* The live difficulty setting scales the interval: 3 is neutral. */
	int scaled = (int)p * (8 - nexus_game_speed()) / 5;

	if (scaled < 1) {
		scaled = 1;
	}
	return (uint8_t)scaled;
}

static void arm_tick(void)
{
	k_work_reschedule_for_queue(nexus_workq(), &g_tick, K_MSEC(TICK_MS));
}

static void end_round(bool won)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_tick);
	nexus_game_submit_score(&nexus_game_invaders, g_f.score);

	g_over_title = won ? "CLEARED" : "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(won ? NEXUS_SOUND_TETRIS_TETRIS : NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

static void lose_life(void)
{
	nexus_sound_play(NEXUS_SOUND_BACK);
	if (--g_f.lives == 0) {
		end_round(false);
		return;
	}
	for (int i = 0; i < MAX_SHOTS; i++) {
		g_f.shot[i].live = false;
	}
	g_f.cool = 0;
	for (int i = 0; i < MAX_BOMBS; i++) {
		g_f.bomb[i].live = false;
	}
	g_f.cannon = FIELD_X + (FIELD_W - CANNON_W) / 2;
	nexus_screen_invalidate();
}

/* ---- simulation --------------------------------------------------------- */

static void march(void)
{
	int lo = FIELD_R;
	int hi = FIELD_X;

	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			if (!alive(r, c)) {
				continue;
			}
			if (alien_x(c) < lo) {
				lo = alien_x(c);
			}
			if (alien_x(c) + ALIEN_W > hi) {
				hi = alien_x(c) + ALIEN_W;
			}
		}
	}

	/*
	 * The edge test uses the LIVE extent, not the formation's nominal
	 * width. Clearing the left column should let the fleet slide further
	 * left; testing the origin instead makes it turn early against an
	 * invisible wall, which looks like a bug even though nothing is wrong.
	 */
	if ((g_f.dir > 0 && hi + STEP_X > FIELD_R) ||
	    (g_f.dir < 0 && lo - STEP_X < FIELD_X)) {
		g_f.dir = (int8_t)-g_f.dir;
		g_f.fy += STEP_Y;
	} else {
		g_f.fx += g_f.dir * STEP_X;
	}

	nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);

	/* Reached the cannon's row: over, however many lives are left. */
	for (int r = ROWS - 1; r >= 0; r--) {
		if (g_f.row[r] == 0) {
			continue;
		}
		if (alien_y(r) + ALIEN_H >= CANNON_Y) {
			end_round(false);
		}
		break;
	}
}

static void drop_bomb(void)
{
	/* One of the aliens with nothing below it in its own column - the
	 * bottom of a column is the only one with line of sight. */
	int c = (int)(rnd() % COLS);

	for (int r = ROWS - 1; r >= 0; r--) {
		if (!alive(r, c)) {
			continue;
		}
		for (int i = 0; i < MAX_BOMBS; i++) {
			if (g_f.bomb[i].live) {
				continue;
			}
			g_f.bomb[i].live = true;
			g_f.bomb[i].x = (int16_t)(alien_x(c) + ALIEN_W / 2);
			g_f.bomb[i].y = (int16_t)(alien_y(r) + ALIEN_H);
			return;
		}
		return;
	}
}

static void step(void)
{
	g_f.anim++;

	if (g_f.march == 0) {
		march();
		if (g_state != NEXUS_GAME_RUNNING) {
			return;
		}
		g_f.march = march_period();
	} else {
		g_f.march--;
	}

	bool scored = false;

	if (g_f.cool) {
		g_f.cool--;
	}

	/* Your shots, upward. */
	for (int i = 0; i < MAX_SHOTS; i++) {
		struct shot *sh = &g_f.shot[i];

		if (!sh->live) {
			continue;
		}
		sh->y -= 7;
		if (sh->y < FIELD_Y) {
			sh->live = false;
			continue;
		}
		for (int r = 0; r < ROWS && sh->live; r++) {
			for (int c = 0; c < COLS; c++) {
				if (!alive(r, c)) {
					continue;
				}
				int ax = alien_x(c);
				int ay = alien_y(r);

				if (sh->x < ax || sh->x > ax + ALIEN_W ||
				    sh->y < ay || sh->y > ay + ALIEN_H) {
					continue;
				}

				g_f.row[r] &= (uint16_t)~(1u << c);
				g_f.left--;
				/* Back rows are worth more, which is the
				 * only reason to aim past the front one. */
				g_f.score += (uint32_t)(ROWS - r) * 10U;
				sh->live = false;
				scored = true;
				nexus_sound_play(NEXUS_SOUND_TETRIS_ROTATE);
				break;
			}
		}
	}

	if (g_f.left == 0) {
		end_round(true);
		return;
	}

	/* Their bombs, downward. */
	for (int i = 0; i < MAX_BOMBS; i++) {
		if (!g_f.bomb[i].live) {
			continue;
		}
		g_f.bomb[i].y += 5;
		if (g_f.bomb[i].y > FIELD_B) {
			g_f.bomb[i].live = false;
			continue;
		}
		if (g_f.bomb[i].y + SHOT_H >= CANNON_Y &&
		    g_f.bomb[i].x >= g_f.cannon &&
		    g_f.bomb[i].x <= g_f.cannon + CANNON_W) {
			g_f.bomb[i].live = false;
			lose_life();
			return;
		}
	}

	/* Roughly one bomb a second at the default tick, scaled by how many
	 * are left to throw them. */
	if ((rnd() % 40) == 0) {
		drop_bomb();
	}

	if (scored) {
		nexus_screen_invalidate_rows(HUD_Y, HUD_Y + HUD_H);
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

static void draw_alien(int x, int y, int row)
{
	const struct nexus_theme *t = nexus_theme();
	/* Colour by row, so the board reads as ranked value. */
	gfx_color body = gfx_mix(t->accent, t->accent_alt,
				 (uint8_t)(row * 255 / (ROWS - 1)));

	/* Body, then two legs that swap on alternate steps - the whole
	 * animation of the original, and it costs two rects. */
	nexus_draw_block(x + 3, y, ALIEN_W - 6, ALIEN_H - 5, 4, body);

	int lift = (g_f.anim & 8) ? 0 : 3;

	gfx_rect(x, y + ALIEN_H - 6 - lift, 5, 6, body, GFX_OPAQUE);
	gfx_rect(x + ALIEN_W - 5, y + ALIEN_H - 6 - (3 - lift), 5, 6, body,
		 GFX_OPAQUE);

	/* Eyes. Three pixels is what it takes to still read as a face from
	 * the other side of a desk. */
	gfx_rect(x + 7, y + 5, 4, 4, NEXUS_C(0x0A0A12u), GFX_OPAQUE);
	gfx_rect(x + ALIEN_W - 11, y + 5, 4, 4, NEXUS_C(0x0A0A12u), GFX_OPAQUE);
}

static void invaders_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (gfx_hits(8, gfx_text_h(NEXUS_TXT_LABEL))) {
		nexus_draw_label(NEXUS_PAD, 8, "INVADERS");
		nexus_draw_label(GFX_W - NEXUS_PAD -
					 gfx_text_w("HOLD=EXIT", NEXUS_TXT_LABEL),
				 8, "HOLD=EXIT");
	}

	if (gfx_hits(HUD_Y, HUD_H)) {
		gfx_text(NEXUS_PAD, 24,
			 gfx_utoa(g_f.score, buf, sizeof(buf), 0),
			 NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
		for (int i = 0; i < g_f.lives; i++) {
			gfx_disc(GFX_W - NEXUS_PAD - 6 - i * 12, 30, 4,
				 t->success, GFX_OPAQUE);
		}
	}

	if (!gfx_hits(FIELD_Y, FIELD_H)) {
		return;
	}

	gfx_round_frame(FIELD_X - 2, FIELD_Y - 2, FIELD_W + 4, FIELD_H + 4, 3,
			t->border, t->border_alpha);

	for (int r = 0; r < ROWS; r++) {
		if (g_f.row[r] == 0) {
			continue;
		}
		for (int c = 0; c < COLS; c++) {
			if (alive(r, c)) {
				draw_alien(alien_x(c), alien_y(r), r);
			}
		}
	}

	for (int i = 0; i < MAX_SHOTS; i++) {
		if (!g_f.shot[i].live) {
			continue;
		}
		nexus_draw_block(g_f.shot[i].x - SHOT_W / 2, g_f.shot[i].y,
				 SHOT_W, SHOT_H, 1, t->warning);
	}
	for (int i = 0; i < MAX_BOMBS; i++) {
		if (g_f.bomb[i].live) {
			nexus_draw_block(g_f.bomb[i].x - SHOT_W / 2,
					 g_f.bomb[i].y, SHOT_W, SHOT_H, 1,
					 t->error);
		}
	}

	/* The cannon: a base with a barrel, so which way it fires is obvious. */
	nexus_draw_block(g_f.cannon, CANNON_Y + 4, CANNON_W, CANNON_H - 4, 2,
			 t->success);
	nexus_draw_block(g_f.cannon + CANNON_W / 2 - 2, CANNON_Y, 4, 6, 1,
			 t->success);

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_f.score,
				nexus_game_highscore(&nexus_game_invaders));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(void)
{
	memset(&g_f, 0, sizeof(g_f));
	g_f.rng = (uint32_t)k_uptime_get_32() ^ 0x27220A95u;
	if (g_f.rng == 0) {
		g_f.rng = 1;
	}

	for (int r = 0; r < ROWS; r++) {
		g_f.row[r] = (uint16_t)((1u << COLS) - 1u);
	}
	g_f.left = ROWS * COLS;
	g_f.fx = FIELD_X + 8;
	g_f.fy = FIELD_Y + 6;
	g_f.dir = 1;
	g_f.march = MARCH_SLOW;
	g_f.cannon = FIELD_X + (FIELD_W - CANNON_W) / 2;
	g_f.lives = LIVES;

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_tick();
	nexus_screen_invalidate();
}

static void invaders_start(void)
{
	new_round();
}

static void invaders_stop(void)
{
	k_work_cancel_delayable(&g_tick);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void invaders_pause(void)
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

static void invaders_resume(void)
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

static void move_cannon(int delta)
{
	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}

	int x = g_f.cannon + delta;

	if (x < FIELD_X) {
		x = FIELD_X;
	}
	if (x > FIELD_R - CANNON_W) {
		x = FIELD_R - CANNON_W;
	}
	g_f.cannon = (int16_t)x;
	nexus_screen_invalidate_rows(FIELD_Y, FIELD_B);
}

static void fire(void)
{
	if (g_state != NEXUS_GAME_RUNNING || g_f.cool) {
		return;
	}
	for (int i = 0; i < MAX_SHOTS; i++) {
		if (g_f.shot[i].live) {
			continue;
		}
		g_f.shot[i].live = true;
		g_f.shot[i].x = (int16_t)(g_f.cannon + CANNON_W / 2);
		g_f.shot[i].y = CANNON_Y;
		g_f.cool = SHOT_COOL;
		nexus_sound_play(NEXUS_SOUND_TETRIS_DROP);
		return;
	}
}

static bool invaders_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_PAUSE || action == NEXUS_ACTION_RESUME ||
	    action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			invaders_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			invaders_resume();
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

	/*
	 * SELECT fires while playing rather than pausing. A shooter whose
	 * main button pauses is unplayable on the dongle's own button, and
	 * pause is still reachable by holding - which every screen already
	 * spells out as HOLD=EXIT.
	 */
	if (action == NEXUS_ACTION_SELECT) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			fire();
			return true;
		case NEXUS_GAME_PAUSED:
			invaders_resume();
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
		move_cannon(-CANNON_STEP);
		return true;
	case NEXUS_ACTION_RIGHT:
		move_cannon(CANNON_STEP);
		return true;
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_DROP:
	case NEXUS_ACTION_ROTATE:
		fire();
		return true;
	default:
		return false;
	}
}

static uint32_t invaders_score(void)
{
	return g_f.score;
}

static enum nexus_game_state invaders_state(void)
{
	return g_state;
}

static void invaders_icon(int cx, int cy)
{
	const struct nexus_theme *t = nexus_theme();

	for (int i = 0; i < 3; i++) {
		nexus_draw_block(cx - 30 + i * 22, cy - 20, 16, 12, 3,
				 gfx_mix(t->accent, t->accent_alt,
					 (uint8_t)(i * 120)));
	}
	nexus_draw_block(cx - 4, cy - 2, 3, 8, 1, t->warning);
	nexus_draw_block(cx - 11, cy + 12, 22, 8, 2, t->success);
}

const struct nexus_game nexus_game_invaders = {
	.id = "invaders",
	.name = "INVADERS",
	.start = invaders_start,
	.input = invaders_input,
	.pause = invaders_pause,
	.resume = invaders_resume,
	.stop = invaders_stop,
	.draw = invaders_draw,
	.score = invaders_score,
	.state = invaders_state,
	.draw_icon = invaders_icon,
};
