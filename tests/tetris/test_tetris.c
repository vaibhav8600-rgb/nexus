/*
 * Tetris rules engine tests (Section 110).
 *
 * Plain assert()s against a host compiler - no framework, no fixtures. Run:
 *   cc -Wall -Wextra -o t test_tetris.c ../../src/games/tetris/tetris_core.c && ./t
 * CI does exactly that (.github/workflows/test.yml).
 *
 * Pause/resume/restart/high-score live in tetris.c on top of this engine and
 * are covered by the hardware procedure in docs/development.md, not here.
 */

#include "../../src/games/tetris/tetris_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int checks;
#define CHECK(cond) do { checks++; assert(cond); } while (0)

/* Put a specific piece under player control at a known spot. */
static void place(struct tetris *t, uint8_t piece, uint8_t rot, int x, int y)
{
	t->piece = piece;
	t->rot = rot;
	t->x = (int8_t)x;
	t->y = (int8_t)y;
	t->status = TETRIS_PLAYING;
}

/* Fill board row @p r solid except column @p gap (-1 fills it completely). */
static void fill_row(struct tetris *t, int r, int gap)
{
	for (int c = 0; c < TETRIS_COLS; c++) {
		t->cell[r][c] = (c == gap) ? TETRIS_EMPTY : 3;
	}
}

static int count_shape_cells(uint8_t piece, uint8_t rot)
{
	int n = 0;

	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			n += tetris_shape_has(piece, rot, r, c) ? 1 : 0;
		}
	}
	return n;
}

static void test_shapes(void)
{
	/* Every tetromino is four cells in every rotation. A typo in the hex
	 * tables shows up here and nowhere else. */
	for (uint8_t p = 1; p <= 7; p++) {
		for (uint8_t r = 0; r < 4; r++) {
			CHECK(count_shape_cells(p, r) == 4);
		}
	}

	/* O is rotationally symmetric. */
	for (uint8_t r = 0; r < 4; r++) {
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 4; j++) {
				CHECK(tetris_shape_has(2, r, i, j) ==
				      tetris_shape_has(2, 0, i, j));
			}
		}
	}

	CHECK(!tetris_shape_has(0, 0, 0, 0));  /* piece 0 is "empty" */
	CHECK(!tetris_shape_has(8, 0, 0, 0));  /* out of range */
	CHECK(!tetris_shape_has(1, 0, -1, 0));
}

static void test_spawn(void)
{
	struct tetris t;

	tetris_init(&t, 42);
	CHECK(t.status == TETRIS_PLAYING);
	CHECK(t.piece >= 1 && t.piece <= 7);
	CHECK(t.next >= 1 && t.next <= 7);
	CHECK(t.level == 1);
	CHECK(t.score == 0);
	CHECK(t.lines == 0);

	/* Same seed, same sequence - the tests below depend on this. */
	struct tetris u;

	tetris_init(&u, 42);
	CHECK(u.piece == t.piece && u.next == t.next);
}

static void test_bag(void)
{
	/* 7-bag: across seven consecutive draws each piece appears once. */
	struct tetris t;

	tetris_init(&t, 7);

	int seen[8] = { 0 };

	seen[t.piece]++;
	for (int i = 0; i < 6; i++) {
		tetris_spawn(&t);
		seen[t.piece]++;
	}

	for (int p = 1; p <= 7; p++) {
		CHECK(seen[p] == 1);
	}
}

static void test_move_and_walls(void)
{
	struct tetris t;

	tetris_init(&t, 1);
	place(&t, 3, 0, 3, 5);   /* T piece, spawn rotation */

	CHECK(tetris_move(&t, -1) == TETRIS_EV_MOVE);
	CHECK(t.x == 2);
	CHECK(tetris_move(&t, 1) == TETRIS_EV_MOVE);
	CHECK(t.x == 3);

	/* Walk into the left wall and stop dead rather than wrapping. */
	for (int i = 0; i < 10; i++) {
		tetris_move(&t, -1);
	}
	int8_t parked = t.x;

	CHECK(tetris_move(&t, -1) == 0);
	CHECK(t.x == parked);

	for (int i = 0; i < 20; i++) {
		tetris_move(&t, 1);
	}
	parked = t.x;
	CHECK(tetris_move(&t, 1) == 0);
	CHECK(t.x == parked);
}

static void test_rotate(void)
{
	struct tetris t;

	tetris_init(&t, 2);
	place(&t, 3, 0, 3, 5);

	CHECK(tetris_rotate(&t) == TETRIS_EV_ROTATE);
	CHECK(t.rot == 1);
	tetris_rotate(&t);
	tetris_rotate(&t);
	tetris_rotate(&t);
	CHECK(t.rot == 0);

	/* Rotating hard against the left wall kicks inward instead of failing,
	 * and never leaves a cell outside the well. */
	place(&t, 1, 1, -1, 5);   /* vertical I, box hanging off the edge */
	CHECK(tetris_rotate(&t) == TETRIS_EV_ROTATE);
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			if (tetris_shape_has(t.piece, t.rot, r, c)) {
				CHECK(t.x + c >= 0);
				CHECK(t.x + c < TETRIS_COLS);
			}
		}
	}
}

static void test_floor_and_stack(void)
{
	struct tetris t;

	tetris_init(&t, 3);
	memset(t.cell, 0, sizeof(t.cell));
	place(&t, 2, 0, 4, 0);   /* O piece occupies columns 5 and 6 */

	int guard = 0;

	while (!(tetris_step(&t) & TETRIS_EV_LOCK) && guard++ < 100) {
	}
	CHECK(guard < 100);
	CHECK(t.cell[TETRIS_ROWS - 1][5] != TETRIS_EMPTY);
	CHECK(t.cell[TETRIS_ROWS - 1][6] != TETRIS_EMPTY);

	/* A second O in the same columns stacks on the first, not through it. */
	place(&t, 2, 0, 4, 0);
	guard = 0;
	while (!(tetris_step(&t) & TETRIS_EV_LOCK) && guard++ < 100) {
	}
	CHECK(t.cell[TETRIS_ROWS - 3][5] != TETRIS_EMPTY);
	CHECK(t.cell[TETRIS_ROWS - 3][6] != TETRIS_EMPTY);
}

static void test_single_line_clear(void)
{
	struct tetris t;

	tetris_init(&t, 4);
	memset(t.cell, 0, sizeof(t.cell));
	t.score = 0;
	t.lines = 0;
	t.level = 1;

	/* Bottom row missing column 0; plug it with a vertical I. */
	fill_row(&t, TETRIS_ROWS - 1, 0);
	place(&t, 1, 1, -2, 0);   /* rot 1 of I is column 2 of the box */

	uint32_t ev = tetris_hard_drop(&t);

	CHECK(ev & TETRIS_EV_LOCK);
	CHECK(ev & TETRIS_EV_CLEAR);
	CHECK(t.last_cleared == 1);
	CHECK(t.lines == 1);
	CHECK(t.score >= 100);            /* 100 x level 1, plus drop points */

	/* The full row is gone; the three I cells above it dropped by one. */
	CHECK(t.cell[TETRIS_ROWS - 1][0] != TETRIS_EMPTY);
	CHECK(t.cell[TETRIS_ROWS - 1][1] == TETRIS_EMPTY);
}

static void test_tetris_four_rows(void)
{
	struct tetris t;

	tetris_init(&t, 5);
	memset(t.cell, 0, sizeof(t.cell));
	t.score = 0;
	t.lines = 0;
	t.level = 1;

	for (int r = TETRIS_ROWS - 4; r < TETRIS_ROWS; r++) {
		fill_row(&t, r, 0);
	}

	place(&t, 1, 1, -2, 0);   /* vertical I down column 0 */

	uint32_t ev = tetris_hard_drop(&t);

	CHECK(ev & TETRIS_EV_TETRIS);
	CHECK(t.last_cleared == 4);
	CHECK(t.lines == 4);
	CHECK(t.score >= 800);

	/* Board is empty again apart from whatever just spawned. */
	for (int r = TETRIS_HIDDEN_ROWS; r < TETRIS_ROWS; r++) {
		for (int c = 0; c < TETRIS_COLS; c++) {
			CHECK(t.cell[r][c] == TETRIS_EMPTY);
		}
	}
}

static void test_level_and_speed(void)
{
	struct tetris t;

	tetris_init(&t, 6);
	CHECK(tetris_gravity_ms(&t) == 800);

	t.lines = 0;
	t.level = 1;

	uint32_t saw_level = 0;

	/* Ten lines = level 2, cleared in passes of four, four and two. */
	for (int pass = 0; pass < 3; pass++) {
		int rows = (pass == 2) ? 2 : 4;

		memset(t.cell, 0, sizeof(t.cell));
		for (int r = TETRIS_ROWS - rows; r < TETRIS_ROWS; r++) {
			fill_row(&t, r, 0);
		}
		place(&t, 1, 1, -2, 0);
		saw_level |= tetris_hard_drop(&t) & TETRIS_EV_LEVEL;
	}

	CHECK(t.lines == 10);
	CHECK(t.level == 2);
	CHECK(saw_level == TETRIS_EV_LEVEL);
	CHECK(tetris_gravity_ms(&t) == 700);

	t.level = 7;
	CHECK(tetris_gravity_ms(&t) == 200);   /* last level before the floor */
	t.level = 8;
	CHECK(tetris_gravity_ms(&t) == 100);   /* the floor itself */
	t.level = 20;
	/* Past the floor the subtraction would wrap a uint32 to ~4e9 rather
	 * than go negative, so this is the case that must be clamped first. */
	CHECK(tetris_gravity_ms(&t) == 100);
	t.level = 255;
	CHECK(tetris_gravity_ms(&t) == 100);
}

static void test_game_over(void)
{
	struct tetris t;

	tetris_init(&t, 8);

	/* Brick up the spawn area; the next piece has nowhere to go. */
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < TETRIS_COLS; c++) {
			t.cell[r][c] = 5;
		}
	}

	CHECK(tetris_spawn(&t) & TETRIS_EV_OVER);
	CHECK(t.status == TETRIS_GAMEOVER);

	/* A finished game ignores input rather than quietly continuing. */
	CHECK(tetris_move(&t, -1) == 0);
	CHECK(tetris_rotate(&t) == 0);
	CHECK(tetris_step(&t) == 0);
	CHECK(tetris_hard_drop(&t) == 0);

	/* Restart clears the board and the score. */
	tetris_init(&t, 8);
	CHECK(t.status == TETRIS_PLAYING);
	CHECK(t.score == 0);
	CHECK(t.cell[TETRIS_ROWS - 1][0] == TETRIS_EMPTY);
}

static void test_render_bounds(void)
{
	struct tetris t;

	tetris_init(&t, 9);

	/* The renderer must never be asked to read outside the well. */
	CHECK(tetris_render_cell(&t, -1, 0) == TETRIS_EMPTY);
	CHECK(tetris_render_cell(&t, TETRIS_VISIBLE_ROWS, 0) == TETRIS_EMPTY);
	CHECK(tetris_render_cell(&t, 0, -1) == TETRIS_EMPTY);
	CHECK(tetris_render_cell(&t, 0, TETRIS_COLS) == TETRIS_EMPTY);

	/* A locked block shows up where it was locked. */
	memset(t.cell, 0, sizeof(t.cell));
	t.cell[TETRIS_HIDDEN_ROWS + 3][4] = 6;
	CHECK(tetris_render_cell(&t, 3, 4) == 6);
}

int main(void)
{
	test_shapes();
	test_spawn();
	test_bag();
	test_move_and_walls();
	test_rotate();
	test_floor_and_stack();
	test_single_line_clear();
	test_tetris_four_rows();
	test_level_and_speed();
	test_game_over();
	test_render_bounds();

	printf("tetris: %d checks passed\n", checks);
	return 0;
}
