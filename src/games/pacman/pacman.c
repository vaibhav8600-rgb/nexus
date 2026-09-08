/*
 * Maze chase (Section 38).
 *
 * A grid game like Snake and Tetris, so it costs almost nothing: the maze is
 * one byte per cell and everything else is a handful of positions. No
 * framebuffer, same as the rest.
 *
 * Movement is cell-to-cell rather than sub-pixel. A pixel-smooth version would
 * need a fractional position per actor plus a "which cell am I logically in"
 * rule for eating and collisions, and at 12px cells on a panel you read from
 * across a desk the difference is not visible. Snake makes the same trade.
 *
 * The maze is stored as text and decoded once at round start. That costs 285
 * bytes of flash for the layout and buys a level you can read - and edit -
 * without counting bits, which is what you want the first time a dot turns out
 * to be walled in. tests/games/test_pacman_maze.py flood-fills it and fails
 * the build if any dot is unreachable.
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

#define COLS 19
#define ROWS 15
#define CELL 12

#define WELL_W (COLS * CELL) /* 228 */
#define WELL_H (ROWS * CELL) /* 180 */
#define WELL_X ((GFX_W - WELL_W) / 2)
#define WELL_Y 44
#define FRAME_X (WELL_X - 2)
#define FRAME_Y (WELL_Y - 2)
#define FRAME_W (WELL_W + 4)
#define FRAME_H (WELL_H + 4)

#define HUD_Y 20
#define HUD_H 22

#define GHOSTS 3
#define LIVES 3

/*
 * Columns wrap on EVERY row - what actually confines you is the wall down each
 * edge of the maze. Exactly one row is open at both ends, and that is the
 * tunnel. The test finds it by looking rather than trusting a constant here,
 * because a constant that nothing reads is a comment pretending to be code.
 */

#define TICK_MS CONFIG_NEXUS_PACMAN_TICK_MS
#define FRIGHT_TICKS CONFIG_NEXUS_PACMAN_FRIGHT_TICKS

/* Cell contents, one byte each. */
#define WALL 0
#define OPEN 1
#define DOT 2
#define PELLET 3

enum dir { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

static const int8_t dr[4] = { -1, 0, 1, 0 };
static const int8_t dc[4] = { 0, 1, 0, -1 };

/*
 * The level. '#' wall, '.' dot, 'o' power pellet, 'P' the player's start,
 * 'G' where the ghosts come out.
 */
static const char *const maze_src[ROWS] = {
	"###################",
	"#........#........#",
	"#o##.###.#.###.##o#",
	"#.................#",
	"#.##.#.#####.#.##.#",
	"#....#...#...#....#",
	"####.###.#.###.####",
	".........G.........",
	"####.###.#.###.####",
	"#....#...#...#....#",
	"#.##.#.#####.#.##.#",
	"#.................#",
	"#o##.###.#.###.##o#",
	"#........P........#",
	"###################",
};

struct actor {
	int8_t r, c;
	uint8_t dir;
};

struct chase {
	uint8_t grid[ROWS][COLS];

	struct actor pac;
	uint8_t next_dir;
	struct actor ghost[GHOSTS];

	uint16_t dots_left;
	uint16_t fright; /* ticks of frightened time remaining */
	uint32_t score;
	uint8_t lives;
	uint8_t anim;    /* mouth open/shut, and the pellet blink */
	uint32_t rng;
};

static struct chase g_p;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_fn);

extern const struct nexus_game nexus_game_pacman;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t rnd(void)
{
	g_p.rng ^= g_p.rng << 13;
	g_p.rng ^= g_p.rng >> 17;
	g_p.rng ^= g_p.rng << 5;
	return g_p.rng;
}

/* Column wrap is the tunnel; rows never wrap. */
static inline int wrap_c(int c)
{
	if (c < 0) {
		return COLS - 1;
	}
	if (c >= COLS) {
		return 0;
	}
	return c;
}

static bool walkable(int r, int c)
{
	if (r < 0 || r >= ROWS) {
		return false;
	}
	return g_p.grid[r][wrap_c(c)] != WALL;
}

/* Manhattan distance with the tunnel taken into account, so a ghost does not
 * ignore the shortest route just because it crosses the seam. */
static int dist(int r0, int c0, int r1, int c1)
{
	int dc_ = c0 - c1;

	if (dc_ < 0) {
		dc_ = -dc_;
	}
	if (dc_ > COLS / 2) {
		dc_ = COLS - dc_;
	}

	int dr_ = r0 - r1;

	return (dr_ < 0 ? -dr_ : dr_) + dc_;
}

/* Just this one chaser, back to the pen. */
static void send_home(struct actor *g)
{
	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			if (maze_src[r][c] == 'G') {
				g->r = (int8_t)r;
				g->c = (int8_t)c;
				return;
			}
		}
	}
}

static void place_actors(void)
{
	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			char ch = maze_src[r][c];

			if (ch == 'P') {
				g_p.pac.r = (int8_t)r;
				g_p.pac.c = (int8_t)c;
				g_p.pac.dir = DIR_LEFT;
				g_p.next_dir = DIR_LEFT;
			} else if (ch == 'G') {
				for (int i = 0; i < GHOSTS; i++) {
					g_p.ghost[i].r = (int8_t)r;
					g_p.ghost[i].c = (int8_t)c;
					g_p.ghost[i].dir = (uint8_t)(i & 3);
				}
			}
		}
	}
}

static void load_maze(void)
{
	g_p.dots_left = 0;
	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			switch (maze_src[r][c]) {
			case '#':
				g_p.grid[r][c] = WALL;
				break;
			case '.':
				g_p.grid[r][c] = DOT;
				g_p.dots_left++;
				break;
			case 'o':
				g_p.grid[r][c] = PELLET;
				g_p.dots_left++;
				break;
			default:
				g_p.grid[r][c] = OPEN;
				break;
			}
		}
	}
}

static void arm_tick(void)
{
	/*
	 * The live difficulty setting scales the interval, same as Snake:
	 * 3 is neutral and each step either side is 20%.
	 */
	uint32_t ms = (uint32_t)TICK_MS * (uint32_t)(8 - nexus_game_speed()) / 5U;

	if (ms < 30U) {
		ms = 30U;
	}
	k_work_reschedule_for_queue(nexus_workq(), &g_tick, K_MSEC(ms));
}

static void end_round(bool won)
{
	g_state = NEXUS_GAME_OVER;
	k_work_cancel_delayable(&g_tick);
	nexus_game_submit_score(&nexus_game_pacman, g_p.score);

	g_over_title = won ? "CLEARED" : "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(won ? NEXUS_SOUND_TETRIS_TETRIS : NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

/* ---- simulation --------------------------------------------------------- */

static void move_ghost(struct actor *g)
{
	int best = -1;
	int best_score = 0;
	int opposite = (g->dir + 2) & 3;

	/*
	 * Greedy, with one rule: never reverse. That single constraint is what
	 * turns "walks at you" into something that commits to a route and can
	 * be led away from a corridor - without it a ghost oscillates on the
	 * spot every time you cross its axis.
	 */
	for (int d = 0; d < 4; d++) {
		if (d == opposite) {
			continue;
		}

		int nr = g->r + dr[d];
		int nc = wrap_c(g->c + dc[d]);

		if (!walkable(nr, nc)) {
			continue;
		}

		int score = dist(nr, nc, g_p.pac.r, g_p.pac.c);

		/* Frightened: same search, opposite sign. */
		if (g_p.fright) {
			score = -score;
		}
		/* A little noise so three ghosts do not walk in lockstep. */
		score = score * 4 + (int)(rnd() & 3);

		if (best < 0 || score < best_score) {
			best = d;
			best_score = score;
		}
	}

	if (best < 0) {
		/* Dead end: reversing is the only legal move left. */
		best = opposite;
		if (!walkable(g->r + dr[best], g->c + dc[best])) {
			return;
		}
	}

	g->dir = (uint8_t)best;
	g->r = (int8_t)(g->r + dr[best]);
	g->c = (int8_t)wrap_c(g->c + dc[best]);
}

static bool touching(const struct actor *a, const struct actor *b)
{
	return a->r == b->r && a->c == b->c;
}

static void lose_life(void)
{
	nexus_sound_play(NEXUS_SOUND_BACK);
	if (--g_p.lives == 0) {
		end_round(false);
		return;
	}
	place_actors();
	g_p.fright = 0;
	nexus_screen_invalidate();
}

static void step(void)
{
	g_p.anim++;

	/* Turn when the new direction is actually open, otherwise keep going -
	 * so you can press a turn early and have it taken at the junction. */
	if (walkable(g_p.pac.r + dr[g_p.next_dir],
		     g_p.pac.c + dc[g_p.next_dir])) {
		g_p.pac.dir = g_p.next_dir;
	}
	if (walkable(g_p.pac.r + dr[g_p.pac.dir], g_p.pac.c + dc[g_p.pac.dir])) {
		g_p.pac.r = (int8_t)(g_p.pac.r + dr[g_p.pac.dir]);
		g_p.pac.c = (int8_t)wrap_c(g_p.pac.c + dc[g_p.pac.dir]);
	}

	uint8_t *cell = &g_p.grid[g_p.pac.r][g_p.pac.c];
	bool scored = false;

	if (*cell == DOT) {
		*cell = OPEN;
		g_p.score += 10;
		g_p.dots_left--;
		scored = true;
		nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
	} else if (*cell == PELLET) {
		*cell = OPEN;
		g_p.score += 50;
		g_p.dots_left--;
		g_p.fright = FRIGHT_TICKS;
		scored = true;
		nexus_sound_play(NEXUS_SOUND_TETRIS_LEVEL);
	}

	if (g_p.dots_left == 0) {
		end_round(true);
		return;
	}

	/*
	 * Collisions are tested twice - before and after the ghosts move.
	 * Testing only once lets a ghost and the player swap cells in the same
	 * tick and pass straight through each other, which reads as the hit
	 * detection being broken rather than as a near miss.
	 */
	for (int i = 0; i < GHOSTS; i++) {
		if (!touching(&g_p.pac, &g_p.ghost[i])) {
			continue;
		}
		if (g_p.fright) {
			g_p.score += 200;
			scored = true;
			send_home(&g_p.ghost[i]);
			nexus_sound_play(NEXUS_SOUND_TETRIS_TETRIS);
		} else {
			lose_life();
			return;
		}
	}

	for (int i = 0; i < GHOSTS; i++) {
		move_ghost(&g_p.ghost[i]);
	}

	for (int i = 0; i < GHOSTS; i++) {
		if (!touching(&g_p.pac, &g_p.ghost[i])) {
			continue;
		}
		if (g_p.fright) {
			g_p.score += 200;
			scored = true;
			send_home(&g_p.ghost[i]);
			nexus_sound_play(NEXUS_SOUND_TETRIS_TETRIS);
		} else {
			lose_life();
			return;
		}
	}

	if (g_p.fright) {
		g_p.fright--;
	}

	if (scored) {
		nexus_screen_invalidate_rows(HUD_Y, HUD_Y + HUD_H);
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

static void draw_pac(int x, int y)
{
	const struct nexus_theme *t = nexus_theme();
	int cx = x + CELL / 2;
	int cy = y + CELL / 2;
	int r = CELL / 2 - 1;

	gfx_disc(cx, cy, r, t->warning, GFX_OPAQUE);

	/*
	 * The mouth: a wedge cut back out in the well colour, opening and
	 * shutting on alternate ticks. Drawn as rows rather than as a polygon
	 * because the compositor has no triangle and this is five hlines.
	 */
	if (g_p.anim & 1) {
		return; /* shut - a plain disc */
	}

	for (int i = 0; i < r; i++) {
		int half = i / 2 + 1;

		switch (g_p.pac.dir) {
		case DIR_RIGHT:
			gfx_rect(cx + i, cy - half, 1, half * 2, t->track, 220);
			break;
		case DIR_LEFT:
			gfx_rect(cx - i, cy - half, 1, half * 2, t->track, 220);
			break;
		case DIR_UP:
			gfx_rect(cx - half, cy - i, half * 2, 1, t->track, 220);
			break;
		default:
			gfx_rect(cx - half, cy + i, half * 2, 1, t->track, 220);
			break;
		}
	}
}

static void draw_ghost(int x, int y, int idx)
{
	const struct nexus_theme *t = nexus_theme();
	gfx_color body;

	if (g_p.fright) {
		/* Flashing back to normal in the last stretch is the warning
		 * that the pellet is running out. */
		bool blink = g_p.fright < 8 && (g_p.anim & 1);

		body = blink ? t->value : t->accent_alt;
	} else {
		static const uint8_t mixv[GHOSTS] = { 0, 128, 255 };

		body = gfx_mix(t->error, t->accent, mixv[idx % GHOSTS]);
	}

	int w = CELL - 2;
	int h = CELL - 2;

	/* Domed head over a square skirt: a rounded rect with the bottom
	 * corners filled back in. */
	gfx_round_rect(x + 1, y + 1, w, h, w / 2, body, GFX_OPAQUE);
	gfx_rect(x + 1, y + 1 + h / 2, w, h / 2, body, GFX_OPAQUE);

	/* Eyes, always white - they are what makes it read as a face at 10px. */
	gfx_rect(x + 3, y + 4, 2, 3, NEXUS_C(0xFFFFFFu), GFX_OPAQUE);
	gfx_rect(x + w - 3, y + 4, 2, 3, NEXUS_C(0xFFFFFFu), GFX_OPAQUE);
}

static void pacman_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (gfx_hits(8, gfx_text_h(NEXUS_TXT_LABEL))) {
		nexus_draw_label(NEXUS_PAD, 8, "PAC-MAN");
		nexus_draw_label(GFX_W - NEXUS_PAD -
					 gfx_text_w("HOLD=EXIT", NEXUS_TXT_LABEL),
				 8, "HOLD=EXIT");
	}

	if (gfx_hits(HUD_Y, HUD_H)) {
		gfx_text(NEXUS_PAD, 26, "SCORE", NEXUS_TXT_CAPTION, t->caption,
			 GFX_OPAQUE);
		gfx_text(NEXUS_PAD + 40, 24,
			 gfx_utoa(g_p.score, buf, sizeof(buf), 0),
			 NEXUS_TXT_BODY, t->value, GFX_OPAQUE);

		/* Lives as pips, as Breakout does - you glance at them. */
		for (int i = 0; i < g_p.lives; i++) {
			gfx_disc(GFX_W - NEXUS_PAD - 6 - i * 12, 30, 4,
				 t->warning, GFX_OPAQUE);
		}
	}

	if (!gfx_hits(FRAME_Y, FRAME_H)) {
		return;
	}

	gfx_round_frame(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, 3, t->border,
			t->border_alpha);
	gfx_rect(WELL_X, WELL_Y, WELL_W, WELL_H, t->track, 120);

	for (int r = 0; r < ROWS; r++) {
		int y = WELL_Y + r * CELL;

		if (!gfx_hits(y, CELL)) {
			continue;
		}

		for (int c = 0; c < COLS; c++) {
			int x = WELL_X + c * CELL;

			switch (g_p.grid[r][c]) {
			case WALL:
				/*
				 * The whole cell, with no inset and no
				 * rounding. An inset rounded wall looks
				 * better as one tile and completely wrong as
				 * a maze: every cell becomes a separate blob
				 * instead of merging with its neighbours into
				 * a continuous corridor, which is the shape
				 * you actually navigate by.
				 */
				gfx_rect(x, y, CELL, CELL, t->accent, 70);
				gfx_hline(x, y, CELL, t->edge_hi, 40);
				break;
			case DOT:
				gfx_rect(x + CELL / 2 - 1, y + CELL / 2 - 1, 2,
					 2, t->caption, GFX_OPAQUE);
				break;
			case PELLET:
				/* Blinks, so it reads as the thing worth
				 * detouring for rather than as a big dot. */
				if (!(g_p.anim & 2)) {
					gfx_disc(x + CELL / 2, y + CELL / 2, 3,
						 t->value, GFX_OPAQUE);
				}
				break;
			default:
				break;
			}
		}
	}

	for (int i = 0; i < GHOSTS; i++) {
		draw_ghost(WELL_X + g_p.ghost[i].c * CELL,
			   WELL_Y + g_p.ghost[i].r * CELL, i);
	}
	draw_pac(WELL_X + g_p.pac.c * CELL, WELL_Y + g_p.pac.r * CELL);

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_p.score,
				nexus_game_highscore(&nexus_game_pacman));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(uint32_t salt)
{
	memset(&g_p, 0, sizeof(g_p));
	g_p.rng = (uint32_t)k_uptime_get_32() ^ salt ^ 0x5BD1E995u;
	if (g_p.rng == 0) {
		g_p.rng = 1;
	}

	load_maze();
	place_actors();
	g_p.lives = LIVES;

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	arm_tick();
	nexus_screen_invalidate();
}

static void pacman_start(void)
{
	new_round(0x9E3779B9u);
}

static void pacman_stop(void)
{
	k_work_cancel_delayable(&g_tick);
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void pacman_pause(void)
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

static void pacman_resume(void)
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

static bool pacman_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			pacman_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			pacman_resume();
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
	case NEXUS_ACTION_UP:
	/* I is ROTATE on the game layer because Tetris needs it there. There
	 * is nothing to rotate in a maze, so it steers up - the same reading
	 * Snake gives it. */
	case NEXUS_ACTION_ROTATE:
		g_p.next_dir = DIR_UP;
		break;
	case NEXUS_ACTION_DOWN:
		g_p.next_dir = DIR_DOWN;
		break;
	case NEXUS_ACTION_LEFT:
		g_p.next_dir = DIR_LEFT;
		break;
	case NEXUS_ACTION_RIGHT:
		g_p.next_dir = DIR_RIGHT;
		break;
	default:
		return false;
	}

	/*
	 * No sound on a turn. Unlike Snake, a direction here is a REQUEST -
	 * it is taken at the next junction that allows it - so a click on
	 * every press would fire for turns that never happen.
	 */
	return true;
}

static uint32_t pacman_score(void)
{
	return g_p.score;
}

static enum nexus_game_state pacman_state(void)
{
	return g_state;
}

static void pacman_icon(int cx, int cy)
{
	const struct nexus_theme *t = nexus_theme();

	/* The eater, three pellets and one chaser: the smallest picture that
	 * says "maze chase" rather than "some circles". */
	gfx_disc(cx - 26, cy, 9, t->warning, GFX_OPAQUE);
	gfx_rect(cx - 26, cy - 4, 10, 8, t->bg_bot, GFX_OPAQUE);

	for (int i = 0; i < 3; i++) {
		gfx_rect(cx - 6 + i * 9, cy - 1, 3, 3, t->caption, GFX_OPAQUE);
	}

	gfx_round_rect(cx + 20, cy - 9, 16, 18, 8, t->error, GFX_OPAQUE);
	gfx_rect(cx + 20, cy, 16, 9, t->error, GFX_OPAQUE);
	gfx_rect(cx + 23, cy - 4, 3, 4, NEXUS_C(0xFFFFFFu), GFX_OPAQUE);
	gfx_rect(cx + 31, cy - 4, 3, 4, NEXUS_C(0xFFFFFFu), GFX_OPAQUE);
}

const struct nexus_game nexus_game_pacman = {
	.id = "pacman",
	.name = "PAC-MAN",
	.start = pacman_start,
	.input = pacman_input,
	.pause = pacman_pause,
	.resume = pacman_resume,
	.stop = pacman_stop,
	.draw = pacman_draw,
	.score = pacman_score,
	.state = pacman_state,
	.draw_icon = pacman_icon,
};
