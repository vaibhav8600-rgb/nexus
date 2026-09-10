/*
 * Jumper - a single-screen platformer (Section 38).
 *
 * This replaces a side-scrolling one that was removed, and the reason it is
 * single-screen is the whole point rather than a simplification.
 *
 * A scrolling platformer has to repaint EVERY row on any frame the camera
 * moves, because every row's contents shift. On this panel that is the entire
 * play area, and once the player is running the camera moves on most frames -
 * so the cost of the worst frame becomes the cost of the normal frame, and no
 * amount of tuning the physics fixes it. The scrolling version was rewritten
 * four times and never stopped feeling heavy.
 *
 * With the level fixed on one screen the background never changes. Only the
 * player, two enemies and the coins they touch are ever dirty, which is about
 * three tile rows - a fifth of the work, on every single frame, permanently.
 * The game got smooth by having less to draw, not by drawing faster.
 *
 * The level is text in flash; only what changes lives in RAM, which is a bit
 * per coin plus the actors.
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
 * 20px tiles over 12x9, not 15px over 16x12.
 *
 * A 240px panel read from across a desk is the constraint, not the level
 * design: fewer, larger cells are the only lever there is. 12 columns of 20px
 * is exactly 240 wide and nine rows is exactly the 180 the HUD leaves, so this
 * is the largest tile the screen can hold without cropping.
 */
#define TILE 20
#define COLS 12
#define ROWS 9

#define VIEW_X 0
#define VIEW_Y 44
#define VIEW_W (COLS * TILE) /* 240 */
#define VIEW_H (ROWS * TILE) /* 180 */

#define HUD_Y 20
#define HUD_H 22

typedef int32_t fix_t;
#define FIX 8
#define TO_FIX(v) ((fix_t)(v) << FIX)
#define TO_PX(v) ((int)((v) >> FIX))

/*
 * Solved for a 16 ms tick, which is the panel's own refresh period - there is
 * no point running physics faster than anything can be shown. 2.26 tiles of
 * jump, 4.8 across, 12.5 tiles a second of running.
 */
#define GRAVITY 133     /* 0.52 px/tick^2 */
#define JUMP_V (-1733)  /* -6.8 px/tick   */
#define RUN_V 896       /* 3.5 px/tick    */
#define MAX_FALL 2048   /* 8.0 px/tick    */
#define ENEMY_V 192     /* 0.75 px/tick   */

#define PLAYER_W 15
#define PLAYER_H 17
#define MAX_ENEMIES 4
#define LIVES 3

/* Longer than CONFIG_NEXUS_ACTION_REPEAT_MS, or a held key stutters. */
#define RUN_HOLD_TICKS 6
#define JUMP_BUFFER_TICKS 5

#define TICK_MS CONFIG_NEXUS_JUMPER_TICK_MS

/*
 * ' ' air, '=' platform, 'o' coin, 'E' enemy, 'P' spawn, 'F' the flag at the
 * top. Collect every coin and reach the flag, and the next board loads.
 *
 * They are laid out to one rule, which is arithmetic rather than taste: a
 * jump leaves at 6.8 px/tick against 0.52 px/tick of gravity, so it peaks 44px
 * up - a little over two 20px tiles - and has moved only about 32px sideways
 * by the time it gets there. So every climb is at most two rows, and a two-row
 * climb has to be onto a platform that overlaps the one you left. Anything
 * further apart is not a hard jump, it is an impossible one, and the player
 * cannot tell the difference from the ground.
 *
 * Three boards, then it wraps - with the clock still rising, so board four is
 * board one played faster. Flash cost is 9 lines of text each.
 */
#define STAGES 3

static const char *const stage_map[STAGES][ROWS] = {
	{
		"            ",
		"     F      ",
		"  =======   ",
		" o        o ",
		"====    ====",
		"     o      ",
		" ====   ====",
		"P    E    o ",
		"============",
	},
	{
		"            ",
		"  F         ",
		" =====      ",
		"  o     o   ",
		"=====  =====",
		"   o        ",
		" ====  =====",
		"P    E    o ",
		"============",
	},
	{
		"          F ",
		"      ======",
		"       o    ",
		"    =====   ",
		"   o E      ",
		"  =====     ",
		" o          ",
		"P===        ",
		"============",
	},
};

struct mover {
	fix_t x, y;
	int8_t dir;
	bool alive;
};

struct jumper {
	fix_t x, y;
	fix_t vx, vy;
	bool on_ground;
	int8_t facing;
	uint8_t run_left;
	uint8_t run_right;
	uint8_t jump_want;

	struct mover enemy[MAX_ENEMIES];
	uint8_t enemies;

	uint8_t taken[(COLS * ROWS + 7) / 8];
	uint8_t coins_left;

	uint32_t score;
	uint8_t level;
	uint8_t lives;
	uint8_t anim;
};

static struct jumper g_j;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_fn);

extern const struct nexus_game nexus_game_jumper;

/* ---- level -------------------------------------------------------------- */

/* Level 1 is stage 0. The guard is for the one tick between memset() and
 * new_round() setting the level, where 0 - 1 would index backwards. */
static inline const char *const *stage(void)
{
	uint8_t n = g_j.level ? (uint8_t)(g_j.level - 1U) : 0U;

	return stage_map[n % STAGES];
}

static inline char at(int r, int c)
{
	if (r < 0 || r >= ROWS || c < 0 || c >= COLS) {
		return ' ';
	}
	return stage()[r][c];
}

static inline bool solid(int r, int c)
{
	return at(r, c) == '=';
}

static inline bool taken(int r, int c)
{
	int i = r * COLS + c;

	return (g_j.taken[i >> 3] & (1u << (i & 7))) != 0;
}

static inline void take(int r, int c)
{
	int i = r * COLS + c;

	g_j.taken[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static bool hits(int x, int y, int w, int h)
{
	/*
	 * y as well as x. Without the ceiling the jump from the top platform
	 * carries the player seventeen pixels above the view, where it draws
	 * over the HUD - the sprite leaves the playfield and nothing stops it,
	 * because at() reports everything off the top as open air.
	 */
	if (x < 0 || x + w > COLS * TILE || y < 0) {
		return true;
	}

	for (int r = y / TILE; r <= (y + h - 1) / TILE; r++) {
		for (int c = x / TILE; c <= (x + w - 1) / TILE; c++) {
			if (solid(r, c)) {
				return true;
			}
		}
	}
	return false;
}

static void spawn(void)
{
	g_j.enemies = 0;
	g_j.coins_left = 0;

	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			char ch = stage()[r][c];

			if (ch == 'P') {
				g_j.x = TO_FIX(c * TILE);
				/* Feet on the floor: the sprite is shorter
				 * than a tile, and spawning at the tile top
				 * leaves it hanging. */
				g_j.y = TO_FIX(r * TILE + TILE - PLAYER_H);
			} else if (ch == 'E' && g_j.enemies < MAX_ENEMIES) {
				struct mover *e = &g_j.enemy[g_j.enemies++];

				e->x = TO_FIX(c * TILE);
				e->y = TO_FIX(r * TILE + TILE - PLAYER_H);
				e->dir = -1;
				e->alive = true;
			} else if (ch == 'o' && !taken(r, c)) {
				g_j.coins_left++;
			}
		}
	}

	g_j.vx = g_j.vy = 0;
	g_j.facing = 1;
	g_j.on_ground = false;
	g_j.run_left = g_j.run_right = g_j.jump_want = 0;
}

static void arm_tick(void)
{
	uint32_t ms = (uint32_t)TICK_MS * (uint32_t)(8 - nexus_game_speed()) / 5U;

	/* And the board on top of it. The floor below is not decoration: the
	 * physics constants are px per TICK, so a shorter tick is a longer
	 * jump, and past a point the player clears the ceiling test. */
	ms = ms * 100U / nexus_game_level_pct(g_j.level);

	if (ms < 12U) {
		ms = 12U;
	}
	k_work_reschedule_for_queue(nexus_workq(), &g_tick, K_MSEC(ms));
}

static void end_round(void)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_tick);
	nexus_game_submit_score(&nexus_game_jumper, g_j.score);

	g_over_title = "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

/*
 * The flag. Score and lives carry to the next board; the coin record does
 * not, because it is per-board and spawn() counts what it finds.
 */
static void next_stage(void)
{
	if (g_j.level < 255) {
		g_j.level++;
	}
	memset(g_j.taken, 0, sizeof(g_j.taken));
	spawn();
	g_j.vx = g_j.vy = 0;
	g_j.on_ground = false;
	g_j.run_left = g_j.run_right = g_j.jump_want = 0;
	nexus_sound_play(NEXUS_SOUND_TETRIS_LEVEL);
	nexus_screen_invalidate();
}

static void lose_life(void)
{
	nexus_sound_play(NEXUS_SOUND_BACK);
	if (--g_j.lives == 0) {
		end_round();
		return;
	}
	spawn();
	nexus_screen_invalidate();
}

/* ---- simulation --------------------------------------------------------- */

static void move_enemies(void)
{
	for (int i = 0; i < g_j.enemies; i++) {
		struct mover *e = &g_j.enemy[i];

		if (!e->alive) {
			continue;
		}

		int nx = TO_PX(e->x) + e->dir;
		int py = TO_PX(e->y);

		/* Turn at a wall or at a ledge. Without the ledge test they
		 * walk off every platform within seconds. */
		if (hits(nx, py, PLAYER_W, PLAYER_H) ||
		    !solid((py + PLAYER_H) / TILE, (nx + PLAYER_W / 2) / TILE)) {
			e->dir = (int8_t)-e->dir;
			continue;
		}
		e->x += (fix_t)e->dir * ENEMY_V;
	}
}

static void step(void)
{
	g_j.anim++;

	if (g_j.run_left) {
		g_j.run_left--;
	}
	if (g_j.run_right) {
		g_j.run_right--;
	}
	g_j.vx = 0;
	if (g_j.run_left) {
		g_j.vx = -RUN_V;
		g_j.facing = -1;
	} else if (g_j.run_right) {
		g_j.vx = RUN_V;
		g_j.facing = 1;
	}

	int px = TO_PX(g_j.x);
	int py = TO_PX(g_j.y);

	/* X then Y, resolved separately - doing both at once cannot tell a
	 * wall from a floor, which is how a platformer sticks to walls. */
	if (g_j.vx) {
		int want = TO_PX(g_j.x + g_j.vx);

		if (!hits(want, py, PLAYER_W, PLAYER_H)) {
			g_j.x += g_j.vx;
		} else {
			g_j.vx = 0;
		}
	}

	g_j.vy += GRAVITY;
	if (g_j.vy > MAX_FALL) {
		g_j.vy = MAX_FALL;
	}

	px = TO_PX(g_j.x);

	int wanty = TO_PX(g_j.y + g_j.vy);

	if (!hits(px, wanty, PLAYER_W, PLAYER_H)) {
		g_j.y += g_j.vy;
	} else {
		if (g_j.vy > 0) {
			g_j.y = TO_FIX(((wanty + PLAYER_H) / TILE) * TILE -
				       PLAYER_H);
		} else {
			g_j.y = TO_FIX((wanty / TILE + 1) * TILE);
		}
		g_j.vy = 0;
	}

	px = TO_PX(g_j.x);
	py = TO_PX(g_j.y);

	/*
	 * Grounded is "is there floor under me", not "did I collide going down
	 * this tick". Landing zeroes vy, so the next tick's fraction of a
	 * pixel moves nothing and no collision happens - a latched flag would
	 * clear, alternate every tick, and silently eat half the jumps.
	 */
	g_j.on_ground = hits(px, py + 1, PLAYER_W, PLAYER_H);
	if (g_j.on_ground && g_j.vy > 0) {
		g_j.vy = 0;
	}

	/* A jump asked for just before touching down still counts. */
	if (g_j.jump_want) {
		g_j.jump_want--;
		if (g_j.on_ground) {
			g_j.vy = JUMP_V;
			g_j.on_ground = false;
			g_j.jump_want = 0;
			nexus_sound_play(NEXUS_SOUND_TETRIS_DROP);
		}
	}

	bool scored = false;

	for (int r = py / TILE; r <= (py + PLAYER_H - 1) / TILE; r++) {
		for (int c = px / TILE; c <= (px + PLAYER_W - 1) / TILE; c++) {
			if (at(r, c) == 'o' && !taken(r, c)) {
				take(r, c);
				g_j.coins_left--;
				g_j.score += 100;
				scored = true;
				nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
			} else if (at(r, c) == 'F' && g_j.coins_left == 0) {
				/* The flag only opens once the board is clear,
				 * so the climb is the game rather than a
				 * sprint to the top. */
				next_stage();
				return;
			}
		}
	}

	move_enemies();

	for (int i = 0; i < g_j.enemies; i++) {
		struct mover *e = &g_j.enemy[i];

		if (!e->alive) {
			continue;
		}

		int ex = TO_PX(e->x);
		int ey = TO_PX(e->y);

		if (px + PLAYER_W <= ex || px >= ex + PLAYER_W ||
		    py + PLAYER_H <= ey || py >= ey + PLAYER_H) {
			continue;
		}

		/* Coming down on its head is a stomp; anything else is a hit.
		 * Testing the velocity too is what stops a sideways graze
		 * counting as one. */
		if (g_j.vy >= 0 && py + PLAYER_H - TO_PX(g_j.vy) <= ey + 5) {
			e->alive = false;
			g_j.score += 200;
			scored = true;
			g_j.vy = JUMP_V / 2;
			nexus_sound_play(NEXUS_SOUND_TETRIS_ROTATE);
		} else {
			lose_life();
			return;
		}
	}

	if (scored) {
		nexus_screen_invalidate_rows(HUD_Y, HUD_Y + HUD_H);
	}

	/*
	 * Only the band the actors occupy. Nothing scrolls, so the platforms
	 * and the sky are identical to last frame and pushing them again is
	 * pure waste - this is the entire reason the game is single-screen.
	 */
	int lo = TO_PX(g_j.y);
	int hi = lo + PLAYER_H;

	for (int i = 0; i < g_j.enemies; i++) {
		int ey = TO_PX(g_j.enemy[i].y);

		if (ey < lo) {
			lo = ey;
		}
		if (ey + PLAYER_H > hi) {
			hi = ey + PLAYER_H;
		}
	}
	nexus_screen_invalidate_rows(VIEW_Y + lo - TILE / 2,
				     VIEW_Y + hi + TILE / 2);
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

static void draw_player(int sx, int sy)
{
	const struct nexus_theme *t = nexus_theme();

	nexus_draw_block(sx, sy, PLAYER_W, 7, 2, t->error);
	nexus_draw_block(sx + 1, sy + 7, PLAYER_W - 2, 6, 1, t->warning);
	nexus_draw_block(sx, sy + 13, PLAYER_W, 4, 1, t->accent_alt);

	/* An eye on the side it faces: the cheapest thing that gives a sprite
	 * this small a front. */
	gfx_rect(sx + (g_j.facing > 0 ? PLAYER_W - 5 : 3), sy + 8, 3, 3,
		 NEXUS_C(0xFFFFFFu), GFX_OPAQUE);
}

static void draw_enemy(int sx, int sy)
{
	const struct nexus_theme *t = nexus_theme();

	nexus_draw_block(sx, sy + 3, PLAYER_W, PLAYER_H - 3, 5, t->success);
	gfx_rect(sx + 3, sy + 7, 3, 3, NEXUS_C(0x0A0A12u), GFX_OPAQUE);
	gfx_rect(sx + PLAYER_W - 6, sy + 7, 3, 3, NEXUS_C(0x0A0A12u),
		 GFX_OPAQUE);
}

static void jumper_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (gfx_hits(8, gfx_text_h(NEXUS_TXT_LABEL))) {
		nexus_draw_label(NEXUS_PAD, 8, "JUMPER");
		nexus_draw_label(GFX_W - NEXUS_PAD -
					 gfx_text_w("HOLD=EXIT", NEXUS_TXT_LABEL),
				 8, "HOLD=EXIT");
	}

	if (gfx_hits(HUD_Y, HUD_H)) {
		gfx_text(NEXUS_PAD, 24,
			 gfx_utoa(g_j.score, buf, sizeof(buf), 0),
			 NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
		int lx = nexus_draw_level(GFX_W - NEXUS_PAD, 24, g_j.level);

		for (int i = 0; i < g_j.lives; i++) {
			gfx_disc(lx - 12 - i * 12, 30, 4, t->error,
				 GFX_OPAQUE);
		}
	}

	if (!gfx_hits(VIEW_Y, VIEW_H)) {
		return;
	}

	nexus_draw_field(VIEW_X, VIEW_Y, VIEW_W, VIEW_H);

	for (int r = 0; r < ROWS; r++) {
		int y = VIEW_Y + r * TILE;

		if (!gfx_hits(y, TILE)) {
			continue;
		}

		for (int c = 0; c < COLS; c++) {
			int x = VIEW_X + c * TILE;

			switch (stage()[r][c]) {
			case '=':
				/* Square corners so neighbours tile into one
				 * continuous ledge rather than a row of
				 * separate lozenges. */
				nexus_draw_block(x, y, TILE, TILE, 0,
						 gfx_mix(t->accent,
							 t->accent_alt,
							 (uint8_t)(r * 20)));
				break;
			case 'o':
				if (!taken(r, c)) {
					nexus_draw_orb(x + TILE / 2,
						       y + TILE / 2,
						       (g_j.anim & 8) ? 6 : 7,
						       t->warning);
				}
				break;
			case 'F':
				gfx_rect(x + TILE / 2 - 2, y, 4, TILE,
					 t->value, GFX_OPAQUE);
				nexus_draw_block(x + TILE / 2 + 2, y, 12, 10, 2,
						 g_j.coins_left ? t->muted
								: t->success);
				break;
			default:
				break;
			}
		}
	}

	for (int i = 0; i < g_j.enemies; i++) {
		if (g_j.enemy[i].alive) {
			draw_enemy(VIEW_X + TO_PX(g_j.enemy[i].x),
				   VIEW_Y + TO_PX(g_j.enemy[i].y));
		}
	}
	draw_player(VIEW_X + TO_PX(g_j.x), VIEW_Y + TO_PX(g_j.y));

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_j.score,
				nexus_game_highscore(&nexus_game_jumper));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(void)
{
	memset(&g_j, 0, sizeof(g_j));
	g_j.level = 1;
	spawn();
	g_j.lives = LIVES;

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_tick();
	nexus_screen_invalidate();
}

static void jumper_start(void)
{
	new_round();
}

static void jumper_stop(void)
{
	k_work_cancel_delayable(&g_tick);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void jumper_pause(void)
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

static void jumper_resume(void)
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

static void jump(void)
{
	if (!g_j.on_ground) {
		g_j.jump_want = JUMP_BUFFER_TICKS;
		return;
	}
	g_j.vy = JUMP_V;
	g_j.on_ground = false;
	g_j.jump_want = 0;
	nexus_sound_play(NEXUS_SOUND_TETRIS_DROP);
}

static bool jumper_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			jumper_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			jumper_resume();
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

	if (g_state != NEXUS_GAME_RUNNING) {
		return false;
	}

	switch (action) {
	case NEXUS_ACTION_LEFT:
		g_j.run_left = RUN_HOLD_TICKS;
		g_j.run_right = 0;
		return true;
	case NEXUS_ACTION_RIGHT:
		g_j.run_right = RUN_HOLD_TICKS;
		g_j.run_left = 0;
		return true;
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_DROP:
	case NEXUS_ACTION_ROTATE:
		jump();
		return true;
	default:
		return false;
	}
}

static uint32_t jumper_score(void)
{
	return g_j.score;
}

static enum nexus_game_state jumper_state(void)
{
	return g_state;
}

static void jumper_icon(int cx, int cy)
{
	const struct nexus_theme *t = nexus_theme();

	nexus_draw_block(cx - 32, cy + 10, 26, 8, 0, t->accent);
	nexus_draw_block(cx + 6, cy - 4, 26, 8, 0, t->accent_alt);
	nexus_draw_orb(cx + 19, cy - 14, 5, t->warning);

	nexus_draw_block(cx - 24, cy - 6, 11, 5, 2, t->error);
	nexus_draw_block(cx - 23, cy - 1, 9, 5, 1, t->warning);
	nexus_draw_block(cx - 24, cy + 4, 11, 3, 1, t->accent_alt);
}

const struct nexus_game nexus_game_jumper = {
	.id = "jumper",
	.name = "JUMPER",
	.start = jumper_start,
	.input = jumper_input,
	.pause = jumper_pause,
	.resume = jumper_resume,
	.stop = jumper_stop,
	.draw = jumper_draw,
	.score = jumper_score,
	.state = jumper_state,
	.draw_icon = jumper_icon,
};
