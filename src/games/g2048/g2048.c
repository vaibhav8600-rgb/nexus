/*
 * 2048 (Section 38).
 *
 * The only turn-based game here, and that changes everything about its cost:
 * nothing moves between presses, so it repaints once per move rather than on
 * a clock. There is no tick and no work item at all - the whole game is a
 * 4x4 grid of exponents and a random number generator.
 *
 * Tiles hold the EXPONENT, not the value: 1 means 2, 11 means 2048. Four bits
 * would do; a byte is used because the board is 16 cells and saving 8 bytes is
 * not worth the shifting.
 *
 * The merge rule is the part everyone gets wrong. Sliding a row of 2 2 4 left
 * gives 4 4, not 8 - a tile that has just merged cannot merge again in the
 * same move. The `merged` flag per cell is what enforces it, and the test
 * pins the case.
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

#define N 4
#define CELL 46
#define GAP 2

/*
 * 194 square, and the gap rather than the cell is what gives that.
 *
 * At CELL 46 GAP 4 the board came to 204 and, below a HUD ending at 40, ran
 * four pixels off the bottom - a square board is the one shape whose height is
 * not free once the width is chosen. Shrinking the CELL instead would have
 * been worse: four digits at body size are 46px, so a 42px tile cannot hold
 * "2048" at all. The gap had the slack; the cell did not.
 */
#define BOARD_W (N * CELL + (N + 1) * GAP) /* 194 */
#define BOARD_X ((GFX_W - BOARD_W) / 2)
#define BOARD_Y 44

/* 22, not 18: the title is drawn at y=6 at label size, which is 14 rows, so
 * it occupies 6..20 - a HUD starting at 18 overlaps it by two. */
#define HUD_Y 22
#define HUD_H 22

#define WIN_EXP 11 /* 2^11 = 2048 */

struct board {
	uint8_t cell[N][N]; /* exponent, 0 = empty */
	uint32_t score;
	uint32_t rng;
	bool won;
};

static struct board g_b;
static enum nexus_game_state g_state;
static const char *g_over_title;
static const char *g_over_hint;
static const char *g_over_hint2;

extern const struct nexus_game nexus_game_2048;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t rnd(void)
{
	g_b.rng ^= g_b.rng << 13;
	g_b.rng ^= g_b.rng >> 17;
	g_b.rng ^= g_b.rng << 5;
	return g_b.rng;
}

static void spawn(void)
{
	uint8_t free_cells = 0;

	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			if (g_b.cell[r][c] == 0) {
				free_cells++;
			}
		}
	}
	if (free_cells == 0) {
		return;
	}

	/*
	 * Pick the Nth empty cell rather than retrying random positions - on a
	 * nearly full board rejection sampling can spin a long time, and this
	 * runs on the display work queue.
	 */
	uint8_t want = (uint8_t)(rnd() % free_cells);
	/* One in ten is a 4, as the original does. */
	uint8_t val = (rnd() % 10) == 0 ? 2 : 1;

	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			if (g_b.cell[r][c] != 0) {
				continue;
			}
			if (want-- == 0) {
				g_b.cell[r][c] = val;
				return;
			}
		}
	}
}

/* Can anything still move? Only called when the board is full. */
static bool any_move(void)
{
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			uint8_t v = g_b.cell[r][c];

			if (v == 0) {
				return true;
			}
			if (c + 1 < N && g_b.cell[r][c + 1] == v) {
				return true;
			}
			if (r + 1 < N && g_b.cell[r + 1][c] == v) {
				return true;
			}
		}
	}
	return false;
}

static void end_round(bool won)
{
	g_state = NEXUS_GAME_OVER;
	nexus_game_submit_score(&nexus_game_2048, g_b.score);

	g_over_title = won ? "2048" : "GAME OVER";
	g_over_hint = "ACTION=RESTART";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(won ? NEXUS_SOUND_TETRIS_TETRIS : NEXUS_SOUND_GAME_OVER);
	nexus_screen_invalidate();
}

/* ---- the slide ---------------------------------------------------------- */

enum dir_2048 { D_LEFT, D_RIGHT, D_UP, D_DOWN };

/*
 * One line, always slid toward index 0. Every direction is this function with
 * a different way of reading the board into `line`, which is what keeps the
 * merge rule in exactly one place instead of four.
 */
static bool slide_line(uint8_t *line)
{
	uint8_t out[N] = { 0 };
	bool merged[N] = { false };
	int w = 0;
	bool moved = false;

	for (int i = 0; i < N; i++) {
		if (line[i] == 0) {
			continue;
		}

		if (w > 0 && out[w - 1] == line[i] && !merged[w - 1]) {
			out[w - 1]++;
			merged[w - 1] = true;
			g_b.score += 1u << out[w - 1];
			if (out[w - 1] >= WIN_EXP) {
				g_b.won = true;
			}
			moved = true;
		} else {
			out[w++] = line[i];
		}
	}

	for (int i = 0; i < N; i++) {
		if (line[i] != out[i]) {
			moved = true;
		}
		line[i] = out[i];
	}
	return moved;
}

static bool slide(enum dir_2048 d)
{
	uint8_t line[N];
	bool moved = false;

	for (int i = 0; i < N; i++) {
		for (int j = 0; j < N; j++) {
			switch (d) {
			case D_LEFT:
				line[j] = g_b.cell[i][j];
				break;
			case D_RIGHT:
				line[j] = g_b.cell[i][N - 1 - j];
				break;
			case D_UP:
				line[j] = g_b.cell[j][i];
				break;
			default:
				line[j] = g_b.cell[N - 1 - j][i];
				break;
			}
		}

		if (slide_line(line)) {
			moved = true;
		}

		for (int j = 0; j < N; j++) {
			switch (d) {
			case D_LEFT:
				g_b.cell[i][j] = line[j];
				break;
			case D_RIGHT:
				g_b.cell[i][N - 1 - j] = line[j];
				break;
			case D_UP:
				g_b.cell[j][i] = line[j];
				break;
			default:
				g_b.cell[N - 1 - j][i] = line[j];
				break;
			}
		}
	}
	return moved;
}

static void do_move(enum dir_2048 d)
{
	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}

	if (!slide(d)) {
		/* A move that changes nothing must NOT spawn a tile, or the
		 * board fills up from pressing into a wall. */
		return;
	}

	nexus_sound_play(NEXUS_SOUND_TETRIS_MOVE);
	spawn();

	if (g_b.won) {
		end_round(true);
		return;
	}
	if (!any_move()) {
		end_round(false);
		return;
	}
	nexus_screen_invalidate();
}

/* ---- painting ----------------------------------------------------------- */

/*
 * A palette that climbs, so the board reads as progress rather than as
 * confetti: cool and dim at the bottom, hot and bright at the top. Indexed by
 * exponent, clamped - anything past 2048 keeps the top colour.
 */
static gfx_color tile_color(uint8_t exp)
{
	static const uint32_t hue[] = {
		0x3A4166, /* 2    slate   */
		0x46538C, /* 4            */
		0x4E7BC4, /* 8    blue    */
		0x3FA9C9, /* 16   cyan    */
		0x37B98F, /* 32   teal    */
		0x63C25A, /* 64   green   */
		0xC7B93F, /* 128  yellow  */
		0xE0913A, /* 256  amber   */
		0xE2653C, /* 512  orange  */
		0xDC4462, /* 1024 red     */
		0xC94FC0, /* 2048 magenta */
	};

	if (exp < 1) {
		exp = 1;
	}
	if (exp > (uint8_t)ARRAY_SIZE(hue)) {
		exp = (uint8_t)ARRAY_SIZE(hue);
	}
	return NEXUS_C(hue[exp - 1]);
}

static void draw_tile(int x, int y, uint8_t exp)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[8];

	if (exp == 0) {
		/* An empty slot, recessed rather than absent - the eye needs
		 * to see sixteen PLACES or the board loses its shape. */
		gfx_round_rect(x, y, CELL, CELL, 6, t->track, 150);
		gfx_round_frame(x, y, CELL, CELL, 6, t->border,
				t->border_alpha);
		return;
	}

	nexus_draw_block(x, y, CELL, CELL, 6, tile_color(exp));

	uint32_t v = 1u << exp;
	/*
	 * Three digits at VALUE is 51px against a 46px cell, so the step down
	 * happens at 100 rather than at 1000. Four digits at BODY are exactly
	 * 46 and fill the tile edge to edge, which is why the cell is not
	 * allowed to shrink below that.
	 */
	int scale = v >= 100 ? NEXUS_TXT_BODY : NEXUS_TXT_VALUE;

	gfx_utoa(v, buf, sizeof(buf), 0);
	gfx_text(x + (CELL - gfx_text_w(buf, scale)) / 2,
		 y + (CELL - gfx_text_h(scale)) / 2, buf, scale,
		 exp <= 2 ? NEXUS_C(0xE8ECF8u) : NEXUS_C(0x11131Cu),
		 GFX_OPAQUE);
}

static void g2048_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[12];

	if (gfx_hits(6, gfx_text_h(NEXUS_TXT_LABEL))) {
		nexus_draw_label(NEXUS_PAD, 6, "2048");
		nexus_draw_label(GFX_W - NEXUS_PAD -
					 gfx_text_w("HOLD=EXIT", NEXUS_TXT_LABEL),
				 6, "HOLD=EXIT");
	}

	if (gfx_hits(HUD_Y, HUD_H)) {
		gfx_text(NEXUS_PAD, HUD_Y + 2, "SCORE", NEXUS_TXT_CAPTION,
			 t->caption, GFX_OPAQUE);
		gfx_text(NEXUS_PAD + 40, HUD_Y,
			 gfx_utoa(g_b.score, buf, sizeof(buf), 0),
			 NEXUS_TXT_BODY, t->value, GFX_OPAQUE);
	}

	if (!gfx_hits(BOARD_Y, BOARD_W)) {
		return;
	}

	gfx_round_rect(BOARD_X - GAP, BOARD_Y - GAP, BOARD_W + GAP,
		       BOARD_W + GAP, 8, t->panel, 90);

	for (int r = 0; r < N; r++) {
		int y = BOARD_Y + r * (CELL + GAP);

		if (!gfx_hits(y, CELL)) {
			continue;
		}
		for (int c = 0; c < N; c++) {
			draw_tile(BOARD_X + c * (CELL + GAP), y,
				  g_b.cell[r][c]);
		}
	}

	nexus_draw_game_overlay(g_over_title, g_over_hint, g_over_hint2,
				g_state == NEXUS_GAME_OVER, g_b.score,
				nexus_game_highscore(&nexus_game_2048));
}

/* ---- game interface ------------------------------------------------------ */

static void new_round(void)
{
	memset(&g_b, 0, sizeof(g_b));
	g_b.rng = (uint32_t)k_uptime_get_32() ^ 0x9E3779B9u;
	if (g_b.rng == 0) {
		g_b.rng = 1;
	}
	spawn();
	spawn();

	g_state = NEXUS_GAME_RUNNING;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	nexus_screen_invalidate();
}

static void g2048_start(void)
{
	new_round();
}

static void g2048_stop(void)
{
	g_state = NEXUS_GAME_IDLE;
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
}

static void g2048_pause(void)
{
	if (g_state != NEXUS_GAME_RUNNING) {
		return;
	}
	g_state = NEXUS_GAME_PAUSED;
	g_over_title = "PAUSED";
	g_over_hint = "ACTION=RESUME";
	g_over_hint2 = "HOLD=EXIT";
	nexus_sound_play(NEXUS_SOUND_GAME_PAUSE);
	nexus_screen_invalidate();
}

static void g2048_resume(void)
{
	if (g_state != NEXUS_GAME_PAUSED) {
		return;
	}
	g_over_title = NULL;
	g_over_hint = NULL;
	g_over_hint2 = NULL;
	g_state = NEXUS_GAME_RUNNING;
	nexus_screen_invalidate();
}

static bool g2048_input(enum nexus_action action)
{
	if (action == NEXUS_ACTION_SELECT || action == NEXUS_ACTION_PAUSE ||
	    action == NEXUS_ACTION_RESUME || action == NEXUS_ACTION_RESTART) {
		switch (g_state) {
		case NEXUS_GAME_RUNNING:
			g2048_pause();
			return true;
		case NEXUS_GAME_PAUSED:
			g2048_resume();
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
		do_move(D_LEFT);
		return true;
	case NEXUS_ACTION_RIGHT:
		do_move(D_RIGHT);
		return true;
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_ROTATE:
		do_move(D_UP);
		return true;
	case NEXUS_ACTION_DOWN:
		do_move(D_DOWN);
		return true;
	default:
		return false;
	}
}

static uint32_t g2048_score(void)
{
	return g_b.score;
}

static enum nexus_game_state g2048_state(void)
{
	return g_state;
}

static void g2048_icon(int cx, int cy)
{
	/* Four tiles climbing the palette, which is the game in one picture. */
	static const uint8_t exp[4] = { 1, 3, 6, 9 };

	for (int i = 0; i < 4; i++) {
		nexus_draw_block(cx - 32 + (i % 2) * 34, cy - 18 + (i / 2) * 34,
				 30, 30, 4, tile_color(exp[i]));
	}
}

const struct nexus_game nexus_game_2048 = {
	.id = "2048",
	.name = "2048",
	.start = g2048_start,
	.input = g2048_input,
	.pause = g2048_pause,
	.resume = g2048_resume,
	.stop = g2048_stop,
	.draw = g2048_draw,
	.score = g2048_score,
	.state = g2048_state,
	.draw_icon = g2048_icon,
};
