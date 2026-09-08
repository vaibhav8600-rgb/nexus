/*
 * Side-scrolling platformer (Section 38).
 *
 * The other four games are grid games. This one is not, and two things follow
 * from that which are worth reading before changing anything here.
 *
 * 1. HELD KEYS, FROM AN EVENT SYSTEM THAT HAS NONE.
 *
 *    Running needs "is left held right now". NEXUS dispatches discrete
 *    actions - there is no key-state to poll - so a naive port moves one step
 *    per press and the character twitches instead of running.
 *
 *    What there IS is auto-repeat: hold a direction and LEFT arrives every
 *    CONFIG_NEXUS_ACTION_REPEAT_MS. So a press sets a direction and winds a
 *    timer, each tick unwinds it, and movement continues while it is above
 *    zero. Set the timer longer than the repeat interval and a held key keeps
 *    it permanently topped up; let go and it runs down in a couple of frames.
 *    Held input, reconstructed from repeats, with no new plumbing.
 *
 * 2. THE LEVEL LIVES IN FLASH.
 *
 *    64x15 tiles of text is 960 bytes of flash and, more usefully, a level you
 *    can see. Only what changes is in RAM: a bit per coin, four enemies and
 *    the player. That is ~180 bytes against the 960 a mutable copy would cost.
 *
 * Positions are 8.8 fixed point in an int32_t. int16_t holds 8.8 only to
 * +/-128 px and this level is 768 px wide - the same trap Breakout fell into,
 * and the reason that type is spelled out rather than inferred.
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

#define TILE 12
#define LEVEL_W 64
#define LEVEL_H 15

#define VIEW_X 0
#define VIEW_Y 44
#define VIEW_W GFX_W          /* 240 - 20 tiles visible */
#define VIEW_H (LEVEL_H * TILE) /* 180 */

#define HUD_Y 20
#define HUD_H 22

#define MAX_ENEMIES 4
#define LIVES 3

typedef int32_t fix_t;
#define FIX 8
#define TO_FIX(v) ((fix_t)(v) << FIX)
#define TO_PX(v) ((int)((v) >> FIX))

/*
 * Tuned against the level, not picked by feel: the jump clears 2.1 tiles and
 * reaches 4.2 across, so a 3-tile pit has a tile of margin.
 * tests/games/test_mario_level.py re-derives that and fails if a pit grows.
 */
#define GRAVITY 87        /* 0.34 px/tick^2 */
#define JUMP_V (-1101)    /* -4.3 px/tick   */
#define RUN_V 512         /* 2.0 px/tick    */
#define MAX_FALL 1536     /* 6.0 px/tick    */
#define ENEMY_V 128       /* 0.5 px/tick    */

#define PLAYER_W 9
#define PLAYER_H 11

/*
 * How long a direction keeps running after the last press. It has to exceed
 * CONFIG_NEXUS_ACTION_REPEAT_MS or a held key stutters; two ticks past it is
 * enough and still stops promptly on release.
 */
#define RUN_HOLD_TICKS 3

#define TICK_MS CONFIG_NEXUS_MARIO_TICK_MS

/*
 * The level. ' ' sky, '#' ground, '=' brick, 'o' coin, 'E' enemy, 'P' spawn,
 * 'F' the flag. Solid is '#' and '='; everything else you walk through.
 */
static const char *const level[LEVEL_H] = {
	"                                                                ",
	"                                                                ",
	"                                                                ",
	"                                                                ",
	"                                                                ",
	"                                                                ",
	"                   oo          oo               ooo             ",
	"                  ===         ====             =====            ",
	"              ooo        ooo                oo                  ",
	"             ====       =====              ===                  ",
	"         oo                            oo                       ",
	"        ===                 ===       ====      ===       oo    ",
	"   P           E               E         E               oo F   ",
	"###################   ############   ###############   #########",
	"###################   ############   ###############   #########",
};

struct enemy {
	fix_t x, y;
	int8_t dir;
	bool alive;
};

struct plat {
	fix_t x, y;
	fix_t vx, vy;
	bool on_ground;
	int8_t facing;
	uint8_t run_left;  /* ticks of held-left remaining  */
	uint8_t run_right; /* ticks of held-right remaining */

	struct enemy enemy[MAX_ENEMIES];
	uint8_t enemies;

	/* One bit per cell: set means the coin there is gone. */
	uint8_t taken[(LEVEL_W * LEVEL_H + 7) / 8];

	int cam;
	uint32_t score;
	uint8_t lives;
	uint8_t anim;
};

static struct plat g_m;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_fn);

extern const struct nexus_game nexus_game_mario;

/* ---- level access ------------------------------------------------------- */

static inline char at(int r, int c)
{
	if (r < 0 || r >= LEVEL_H || c < 0 || c >= LEVEL_W) {
		/* Outside is open air, except below the level, which is the
		 * pit that kills you - handled by the fall check, not here. */
		return ' ';
	}
	return level[r][c];
}

static inline bool solid(int r, int c)
{
	char ch = at(r, c);

	return ch == '#' || ch == '=';
}

static inline bool taken(int r, int c)
{
	int i = r * LEVEL_W + c;

	return (g_m.taken[i >> 3] & (1u << (i & 7))) != 0;
}

static inline void take(int r, int c)
{
	int i = r * LEVEL_W + c;

	g_m.taken[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* True if the box [x, x+w) x [y, y+h) in pixels overlaps any solid tile. */
static bool hits(int x, int y, int w, int h)
{
	int c0 = x / TILE;
	int c1 = (x + w - 1) / TILE;
	int r0 = y / TILE;
	int r1 = (y + h - 1) / TILE;

	if (x < 0 || x + w > LEVEL_W * TILE) {
		return true; /* the level edges are walls */
	}

	for (int r = r0; r <= r1; r++) {
		for (int c = c0; c <= c1; c++) {
			if (solid(r, c)) {
				return true;
			}
		}
	}
	return false;
}

/* ---- setup -------------------------------------------------------------- */

static void spawn(void)
{
	g_m.enemies = 0;
	for (int r = 0; r < LEVEL_H; r++) {
		for (int c = 0; c < LEVEL_W; c++) {
			char ch = level[r][c];

			if (ch == 'P') {
				g_m.x = TO_FIX(c * TILE);
				/* Feet on the floor, same as the enemies.
				 * Gravity would settle the player anyway, but
				 * only after a visible drop on every respawn. */
				g_m.y = TO_FIX(r * TILE + TILE - PLAYER_H);
			} else if (ch == 'E' && g_m.enemies < MAX_ENEMIES) {
				struct enemy *e = &g_m.enemy[g_m.enemies++];

				e->x = TO_FIX(c * TILE);
				/*
				 * Feet ON the floor, not at the top of the
				 * tile. The sprite is 11px in a 12px cell and
				 * enemies have no gravity to settle them, so
				 * spawning at r*TILE leaves them a pixel high
				 * - and the ledge test below then reads their
				 * own row, finds no floor, and reverses them
				 * every tick. They vibrate instead of walking.
				 */
				e->y = TO_FIX(r * TILE + TILE - PLAYER_H);
				e->dir = -1;
				e->alive = true;
			}
		}
	}
	g_m.vx = g_m.vy = 0;
	g_m.facing = 1;
	g_m.on_ground = false;
	g_m.run_left = g_m.run_right = 0;
	g_m.cam = 0;
}

static void arm_tick(void)
{
	uint32_t ms = (uint32_t)TICK_MS * (uint32_t)(8 - nexus_game_speed()) / 5U;

	if (ms < 20U) {
		ms = 20U;
	}
	k_work_reschedule_for_queue(nexus_workq(), &g_tick, K_MSEC(ms));
}

static void end_round(bool won)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_tick);
	nexus_game_submit_score(&nexus_game_mario, g_m.score);

	g_over_title = won ? "COURSE CLEAR" : "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(won ? NEXUS_SOUND_TETRIS_TETRIS : NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

static void lose_life(void)
{
	nexus_sound_play(NEXUS_SOUND_BACK);
	if (--g_m.lives == 0) {
		end_round(false);
		return;
	}
	spawn();
	nexus_screen_invalidate();
}

/* ---- simulation --------------------------------------------------------- */

static void move_enemies(void)
{
	for (int i = 0; i < g_m.enemies; i++) {
		struct enemy *e = &g_m.enemy[i];

		if (!e->alive) {
			continue;
		}

		int nx = TO_PX(e->x) + e->dir;
		int py = TO_PX(e->y);

		/*
		 * Turn at a wall OR at a ledge. Without the ledge test they
		 * walk off every platform in the first ten seconds and the
		 * level empties itself.
		 */
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
	g_m.anim++;

	/* Held-input, rebuilt from auto-repeat. */
	if (g_m.run_left) {
		g_m.run_left--;
	}
	if (g_m.run_right) {
		g_m.run_right--;
	}
	g_m.vx = 0;
	if (g_m.run_left) {
		g_m.vx = -RUN_V;
		g_m.facing = -1;
	} else if (g_m.run_right) {
		g_m.vx = RUN_V;
		g_m.facing = 1;
	}

	/*
	 * Axes resolved separately - X, then Y. Doing both at once and backing
	 * out of the overlap cannot tell "walked into a wall" from "landed on
	 * a floor", which is how a platformer ends up sticking to walls.
	 */
	int px = TO_PX(g_m.x);
	int py = TO_PX(g_m.y);

	if (g_m.vx) {
		int want = TO_PX(g_m.x + g_m.vx);

		if (!hits(want, py, PLAYER_W, PLAYER_H)) {
			g_m.x += g_m.vx;
		} else {
			g_m.x = TO_FIX(want > px ? want - 1 : want + 1);
			g_m.vx = 0;
		}
	}

	g_m.vy += GRAVITY;
	if (g_m.vy > MAX_FALL) {
		g_m.vy = MAX_FALL;
	}

	px = TO_PX(g_m.x);
	int wanty = TO_PX(g_m.y + g_m.vy);

	g_m.on_ground = false;
	if (!hits(px, wanty, PLAYER_W, PLAYER_H)) {
		g_m.y += g_m.vy;
	} else {
		if (g_m.vy > 0) {
			/* Landed: snap to the top of the tile below. */
			int row = (wanty + PLAYER_H) / TILE;

			g_m.y = TO_FIX(row * TILE - PLAYER_H);
			g_m.on_ground = true;
		} else {
			/* Head on a brick. */
			int row = wanty / TILE;

			g_m.y = TO_FIX((row + 1) * TILE);
		}
		g_m.vy = 0;
	}

	px = TO_PX(g_m.x);
	py = TO_PX(g_m.y);

	/* Fell off the world. */
	if (py > LEVEL_H * TILE) {
		lose_life();
		return;
	}

	bool scored = false;

	/* Coins, over the tiles the player box covers. */
	for (int r = py / TILE; r <= (py + PLAYER_H - 1) / TILE; r++) {
		for (int c = px / TILE; c <= (px + PLAYER_W - 1) / TILE; c++) {
			if (at(r, c) == 'o' && !taken(r, c)) {
				take(r, c);
				g_m.score += 100;
				scored = true;
				nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
			} else if (at(r, c) == 'F') {
				end_round(true);
				return;
			}
		}
	}

	move_enemies();

	for (int i = 0; i < g_m.enemies; i++) {
		struct enemy *e = &g_m.enemy[i];

		if (!e->alive) {
			continue;
		}

		int ex = TO_PX(e->x);
		int ey = TO_PX(e->y);

		if (px + PLAYER_W <= ex || px >= ex + PLAYER_W ||
		    py + PLAYER_H <= ey || py >= ey + PLAYER_H) {
			continue;
		}

		/*
		 * Coming down onto its head is a stomp; anything else is a
		 * hit. Testing the velocity as well as the overlap is what
		 * stops a jump that grazes an enemy sideways counting as one.
		 */
		if (g_m.vy > 0 && py + PLAYER_H - TO_PX(g_m.vy) <= ey + 4) {
			e->alive = false;
			g_m.score += 200;
			scored = true;
			g_m.vy = JUMP_V / 2; /* the little bounce */
			nexus_sound_play(NEXUS_SOUND_TETRIS_ROTATE);
		} else {
			lose_life();
			return;
		}
	}

	/*
	 * Camera with a dead zone: it only moves when the player leaves the
	 * middle third. Following exactly would make the whole world shudder
	 * every time you nudge left, and on a panel this size that is far more
	 * distracting than the scroll being a frame late.
	 */
	int want_cam = g_m.cam;

	if (px - g_m.cam > VIEW_W * 2 / 3) {
		want_cam = px - VIEW_W * 2 / 3;
	} else if (px - g_m.cam < VIEW_W / 3) {
		want_cam = px - VIEW_W / 3;
	}
	if (want_cam < 0) {
		want_cam = 0;
	}
	if (want_cam > LEVEL_W * TILE - VIEW_W) {
		want_cam = LEVEL_W * TILE - VIEW_W;
	}
	g_m.cam = want_cam;

	if (scored) {
		nexus_screen_invalidate_rows(HUD_Y, HUD_Y + HUD_H);
	}
	nexus_screen_invalidate_rows(VIEW_Y, VIEW_Y + VIEW_H);
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

	/* Cap and body in two colours, so which way it faces is readable at
	 * 9x11 without drawing a face. */
	gfx_rect(sx, sy, PLAYER_W, 4, t->error, GFX_OPAQUE);
	gfx_rect(sx + (g_m.facing > 0 ? 3 : 0), sy + 2, PLAYER_W - 3, 2,
		 t->error, GFX_OPAQUE);
	gfx_rect(sx + 1, sy + 4, PLAYER_W - 2, 4, t->warning, GFX_OPAQUE);
	gfx_rect(sx, sy + 8, PLAYER_W, 3, t->accent_alt, GFX_OPAQUE);

	/* Legs alternate while running and hold apart in the air. */
	if (!g_m.on_ground || (g_m.anim & 4)) {
		gfx_rect(sx, sy + PLAYER_H - 1, 3, 1, t->value, GFX_OPAQUE);
		gfx_rect(sx + PLAYER_W - 3, sy + PLAYER_H - 1, 3, 1, t->value,
			 GFX_OPAQUE);
	}
}

static void draw_enemy(int sx, int sy)
{
	const struct nexus_theme *t = nexus_theme();

	gfx_round_rect(sx, sy + 2, PLAYER_W, PLAYER_H - 2, 3, t->success,
		       GFX_OPAQUE);
	gfx_rect(sx + 1, sy + PLAYER_H - 2, 2, 2, t->muted, GFX_OPAQUE);
	gfx_rect(sx + PLAYER_W - 3, sy + PLAYER_H - 2, 2, 2, t->muted,
		 GFX_OPAQUE);
	gfx_rect(sx + 2, sy + 5, 2, 2, NEXUS_C(0xFFFFFFu), GFX_OPAQUE);
	gfx_rect(sx + PLAYER_W - 4, sy + 5, 2, 2, NEXUS_C(0xFFFFFFu),
		 GFX_OPAQUE);
}

static void mario_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (gfx_hits(8, gfx_text_h(NEXUS_TXT_LABEL))) {
		/* "MARIO", not "SUPER MARIO": 11 characters at label size is 130px
		 * beside a 106px hint on a 240px panel, and they overlapped.
		 * The Game Center still shows the full name. */
		nexus_draw_label(NEXUS_PAD, 8, "MARIO");
		nexus_draw_label(GFX_W - NEXUS_PAD -
					 gfx_text_w("HOLD=EXIT", NEXUS_TXT_LABEL),
				 8, "HOLD=EXIT");
	}

	if (gfx_hits(HUD_Y, HUD_H)) {
		gfx_text(NEXUS_PAD, 24,
			 gfx_utoa(g_m.score, buf, sizeof(buf), 0),
			 NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
		for (int i = 0; i < g_m.lives; i++) {
			gfx_disc(GFX_W - NEXUS_PAD - 6 - i * 12, 30, 4,
				 t->error, GFX_OPAQUE);
		}
	}

	if (!gfx_hits(VIEW_Y, VIEW_H)) {
		return;
	}

	/* Sky, so the themed ground gradient does not show through a cave. */
	gfx_rect(VIEW_X, VIEW_Y, VIEW_W, VIEW_H, t->track, 140);

	int c0 = g_m.cam / TILE;
	int c1 = (g_m.cam + VIEW_W) / TILE;

	for (int r = 0; r < LEVEL_H; r++) {
		int y = VIEW_Y + r * TILE;

		if (!gfx_hits(y, TILE)) {
			continue;
		}

		for (int c = c0; c <= c1 && c < LEVEL_W; c++) {
			int x = VIEW_X + c * TILE - g_m.cam;
			char ch = level[r][c];

			switch (ch) {
			case '#':
				gfx_rect(x, y, TILE, TILE, t->accent, 80);
				gfx_hline(x, y, TILE, t->edge_hi, 90);
				break;
			case '=':
				gfx_rect(x, y, TILE, TILE, t->warning, 110);
				gfx_hline(x, y, TILE, t->edge_hi, 90);
				gfx_hline(x, y + TILE - 1, TILE, t->edge_lo,
					  110);
				break;
			case 'o':
				if (!taken(r, c) && !(g_m.anim & 8)) {
					gfx_disc(x + TILE / 2, y + TILE / 2, 3,
						 t->warning, GFX_OPAQUE);
				} else if (!taken(r, c)) {
					gfx_disc(x + TILE / 2, y + TILE / 2, 2,
						 t->warning, GFX_OPAQUE);
				}
				break;
			case 'F':
				gfx_rect(x + TILE / 2 - 1, y - TILE, 2,
					 TILE * 2, t->value, GFX_OPAQUE);
				gfx_rect(x + TILE / 2 + 1, y - TILE, 6, 5,
					 t->success, GFX_OPAQUE);
				break;
			default:
				break;
			}
		}
	}

	for (int i = 0; i < g_m.enemies; i++) {
		if (!g_m.enemy[i].alive) {
			continue;
		}
		draw_enemy(VIEW_X + TO_PX(g_m.enemy[i].x) - g_m.cam,
			   VIEW_Y + TO_PX(g_m.enemy[i].y));
	}

	draw_player(VIEW_X + TO_PX(g_m.x) - g_m.cam, VIEW_Y + TO_PX(g_m.y));

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_m.score,
				nexus_game_highscore(&nexus_game_mario));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(void)
{
	memset(&g_m, 0, sizeof(g_m));
	spawn();
	g_m.lives = LIVES;

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_tick();
	nexus_screen_invalidate();
}

static void mario_start(void)
{
	new_round();
}

static void mario_stop(void)
{
	k_work_cancel_delayable(&g_tick);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void mario_pause(void)
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

static void mario_resume(void)
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
	if (!g_m.on_ground) {
		return;
	}
	g_m.vy = JUMP_V;
	g_m.on_ground = false;
	nexus_sound_play(NEXUS_SOUND_TETRIS_DROP);
}

static bool mario_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			mario_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			mario_resume();
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
		g_m.run_left = RUN_HOLD_TICKS;
		g_m.run_right = 0;
		return true;
	case NEXUS_ACTION_RIGHT:
		g_m.run_right = RUN_HOLD_TICKS;
		g_m.run_left = 0;
		return true;
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_DROP:
	/* I is ROTATE on the game layer. Nothing rotates in a platformer, so
	 * it jumps - the same reading Snake and the maze give it. */
	case NEXUS_ACTION_ROTATE:
		jump();
		return true;
	default:
		return false;
	}
}

static uint32_t mario_score(void)
{
	return g_m.score;
}

static enum nexus_game_state mario_state(void)
{
	return g_state;
}

static void mario_icon(int cx, int cy)
{
	const struct nexus_theme *t = nexus_theme();

	/* A figure on a block, mid-jump over a coin. */
	gfx_rect(cx - 30, cy + 10, 60, 8, t->accent, 110);
	gfx_rect(cx - 8, cy - 14, 9, 4, t->error, GFX_OPAQUE);
	gfx_rect(cx - 7, cy - 10, 7, 4, t->warning, GFX_OPAQUE);
	gfx_rect(cx - 8, cy - 6, 9, 3, t->accent_alt, GFX_OPAQUE);
	gfx_disc(cx + 18, cy - 2, 4, t->warning, GFX_OPAQUE);
	gfx_rect(cx - 30, cy - 2, 10, 10, t->warning, 130);
}

const struct nexus_game nexus_game_mario = {
	.id = "mario",
	.name = "SUPER MARIO",
	.start = mario_start,
	.input = mario_input,
	.pause = mario_pause,
	.resume = mario_resume,
	.stop = mario_stop,
	.draw = mario_draw,
	.score = mario_score,
	.state = mario_state,
	.draw_icon = mario_icon,
};
