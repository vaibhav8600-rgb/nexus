/* Tetris rules engine - see tetris_core.h. Host-compilable, no RTOS. */

#include "tetris_core.h"

#include <string.h>

/*
 * Each rotation is a 4x4 bitmap; bit (r*4 + c) is set when that cell is solid.
 * Storing four explicit rotations costs 56 bytes total and removes every
 * rotate-the-matrix bug class, which is the trade any embedded Tetris wants.
 */
static const uint16_t shapes[7][4] = {
	/* I */ { 0x00F0, 0x4444, 0x0F00, 0x2222 },
	/* O */ { 0x0066, 0x0066, 0x0066, 0x0066 },
	/* T */ { 0x0072, 0x0262, 0x0270, 0x0232 },
	/* S */ { 0x0036, 0x0462, 0x0360, 0x0231 },
	/* Z */ { 0x0063, 0x0264, 0x0630, 0x0132 },
	/* J */ { 0x0071, 0x0226, 0x0470, 0x0322 },
	/* L */ { 0x0074, 0x0622, 0x0170, 0x0223 },
};

bool tetris_shape_has(uint8_t piece, uint8_t rot, int r, int c)
{
	if (piece < 1 || piece > 7 || r < 0 || r > 3 || c < 0 || c > 3) {
		return false;
	}
	return (shapes[piece - 1][rot & 3] >> (r * 4 + c)) & 1U;
}

/* xorshift32: deterministic, seedable, 4 bytes of state. A test that seeds it
 * gets the same bag order every run. */
static uint32_t rnd(struct tetris *t)
{
	uint32_t x = t->rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	t->rng = x ? x : 0x1234567u;
	return t->rng;
}

static void refill_bag(struct tetris *t)
{
	for (uint8_t i = 0; i < 7; i++) {
		t->bag[i] = i + 1;
	}
	for (int i = 6; i > 0; i--) {
		int j = (int)(rnd(t) % (uint32_t)(i + 1));
		uint8_t tmp = t->bag[i];

		t->bag[i] = t->bag[j];
		t->bag[j] = tmp;
	}
	t->bag_pos = 0;
}

static uint8_t take_piece(struct tetris *t)
{
	if (t->bag_pos >= 7) {
		refill_bag(t);
	}
	return t->bag[t->bag_pos++];
}

/* Does @p piece at (@p x,@p y)/@p rot overlap a wall, the floor or a block? */
static bool collides(const struct tetris *t, uint8_t piece, uint8_t rot,
		     int x, int y)
{
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			if (!tetris_shape_has(piece, rot, r, c)) {
				continue;
			}

			int br = y + r;
			int bc = x + c;

			if (bc < 0 || bc >= TETRIS_COLS || br >= TETRIS_ROWS) {
				return true;
			}
			/* Above the ceiling is legal while spawning. */
			if (br < 0) {
				continue;
			}
			if (t->cell[br][bc] != TETRIS_EMPTY) {
				return true;
			}
		}
	}
	return false;
}

void tetris_init(struct tetris *t, uint32_t seed)
{
	memset(t, 0, sizeof(*t));
	t->rng = seed ? seed : 0xA5A5A5A5u;
	t->level = 1;
	t->status = TETRIS_READY;
	refill_bag(t);
	t->next = take_piece(t);
	tetris_spawn(t);
	if (t->status != TETRIS_GAMEOVER) {
		t->status = TETRIS_PLAYING;
	}
}

uint32_t tetris_spawn(struct tetris *t)
{
	t->piece = t->next;
	t->next = take_piece(t);
	t->rot = 0;
	t->x = 3;
	t->y = 0;

	if (collides(t, t->piece, t->rot, t->x, t->y)) {
		t->status = TETRIS_GAMEOVER;
		return TETRIS_EV_OVER;
	}
	return 0;
}

uint32_t tetris_move(struct tetris *t, int dx)
{
	if (t->status != TETRIS_PLAYING) {
		return 0;
	}
	if (collides(t, t->piece, t->rot, t->x + dx, t->y)) {
		return 0;
	}

	t->x = (int8_t)(t->x + dx);
	return TETRIS_EV_MOVE;
}

uint32_t tetris_rotate(struct tetris *t)
{
	if (t->status != TETRIS_PLAYING) {
		return 0;
	}

	uint8_t rot = (uint8_t)((t->rot + 1) & 3);

	/*
	 * ponytail: simple symmetric wall kicks, not full SRS. Tries the spot,
	 * then one and two cells either way. It never produces an illegal
	 * placement and plays fine; swap in an SRS offset table if T-spins ever
	 * matter.
	 */
	static const int kicks[] = { 0, -1, 1, -2, 2 };

	for (unsigned int i = 0; i < sizeof(kicks) / sizeof(kicks[0]); i++) {
		if (!collides(t, t->piece, rot, t->x + kicks[i], t->y)) {
			t->x = (int8_t)(t->x + kicks[i]);
			t->rot = rot;
			return TETRIS_EV_ROTATE;
		}
	}
	return 0;
}

static uint8_t clear_lines(struct tetris *t)
{
	uint8_t cleared = 0;

	for (int r = TETRIS_ROWS - 1; r >= 0; r--) {
		bool full = true;

		for (int c = 0; c < TETRIS_COLS; c++) {
			if (t->cell[r][c] == TETRIS_EMPTY) {
				full = false;
				break;
			}
		}

		if (!full) {
			continue;
		}

		/* Collapse everything above down by one, then re-test this same
		 * row - otherwise stacked full rows get missed. */
		for (int rr = r; rr > 0; rr--) {
			memcpy(t->cell[rr], t->cell[rr - 1], TETRIS_COLS);
		}
		memset(t->cell[0], TETRIS_EMPTY, TETRIS_COLS);
		cleared++;
		r++;
	}

	return cleared;
}

static uint32_t lock_piece(struct tetris *t)
{
	uint32_t ev = TETRIS_EV_LOCK;

	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			if (!tetris_shape_has(t->piece, t->rot, r, c)) {
				continue;
			}

			int br = t->y + r;
			int bc = t->x + c;

			if (br >= 0 && br < TETRIS_ROWS && bc >= 0 &&
			    bc < TETRIS_COLS) {
				t->cell[br][bc] = t->piece;
			}
		}
	}

	uint8_t cleared = clear_lines(t);

	t->last_cleared = cleared;

	if (cleared) {
		/* Section 46 table, scaled by level. */
		static const uint16_t per_lines[5] = { 0, 100, 300, 500, 800 };
		uint8_t n = cleared > 4 ? 4 : cleared;

		t->score += (uint32_t)per_lines[n] * t->level;
		t->lines = (uint16_t)(t->lines + cleared);
		ev |= TETRIS_EV_CLEAR;
		if (cleared >= 4) {
			ev |= TETRIS_EV_TETRIS;
		}

		uint8_t level = (uint8_t)(1 + t->lines / 10);

		if (level > t->level) {
			t->level = level;
			ev |= TETRIS_EV_LEVEL;
		}
	}

	return ev | tetris_spawn(t);
}

uint32_t tetris_step(struct tetris *t)
{
	if (t->status != TETRIS_PLAYING) {
		return 0;
	}

	if (!collides(t, t->piece, t->rot, t->x, t->y + 1)) {
		t->y++;
		return TETRIS_EV_MOVE;
	}

	return lock_piece(t);
}

uint32_t tetris_soft_drop(struct tetris *t)
{
	if (t->status != TETRIS_PLAYING) {
		return 0;
	}

	if (!collides(t, t->piece, t->rot, t->x, t->y + 1)) {
		t->y++;
		t->score += 1;
		return TETRIS_EV_MOVE;
	}

	return lock_piece(t);
}

uint32_t tetris_hard_drop(struct tetris *t)
{
	if (t->status != TETRIS_PLAYING) {
		return 0;
	}

	uint32_t rows = 0;

	while (!collides(t, t->piece, t->rot, t->x, t->y + 1)) {
		t->y++;
		rows++;
	}

	t->score += rows * 2U;
	return lock_piece(t);
}

uint32_t tetris_gravity_ms(const struct tetris *t)
{
	/*
	 * 800 ms at level 1, 100 ms faster per level, floored so the game stays
	 * humanly playable rather than becoming a coin flip (Section 45).
	 *
	 * Clamped before the subtraction, not after: level is a uint8_t, and at
	 * level 20 `800 - 19*100` wraps to about four billion rather than going
	 * negative, so an "is it too small" test after the fact would pass and
	 * hand back a piece that never falls.
	 */
	if (t->level >= 8) {
		return 100U;
	}
	return 800U - ((uint32_t)(t->level - 1) * 100U);
}

uint8_t tetris_render_cell(const struct tetris *t, int row, int col)
{
	if (row < 0 || row >= TETRIS_VISIBLE_ROWS || col < 0 || col >= TETRIS_COLS) {
		return TETRIS_EMPTY;
	}

	int br = row + TETRIS_HIDDEN_ROWS;

	if (t->cell[br][col] != TETRIS_EMPTY) {
		return t->cell[br][col];
	}

	if (t->status == TETRIS_GAMEOVER) {
		return TETRIS_EMPTY;
	}

	int pr = br - t->y;
	int pc = col - t->x;

	if (tetris_shape_has(t->piece, t->rot, pr, pc)) {
		return t->piece;
	}

	return TETRIS_EMPTY;
}
