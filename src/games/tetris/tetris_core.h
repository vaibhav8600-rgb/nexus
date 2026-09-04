/*
 * Tetris rules engine (Sections 39-47).
 *
 * Deliberately free of Zephyr, LVGL and NEXUS headers: this file and its .c
 * compile with a plain host compiler, which is what makes tests/tetris
 * runnable in CI without a board (Sections 109-110). Rendering, timing and
 * sound all live in tetris.c.
 */
#ifndef NEXUS_TETRIS_CORE_H_
#define NEXUS_TETRIS_CORE_H_

#include <stdbool.h>
#include <stdint.h>

#define TETRIS_COLS 10
#define TETRIS_VISIBLE_ROWS 20
/* Two hidden rows above the well so a spawning piece has somewhere to be. */
#define TETRIS_HIDDEN_ROWS 2
#define TETRIS_ROWS (TETRIS_VISIBLE_ROWS + TETRIS_HIDDEN_ROWS)

#define TETRIS_EMPTY 0

/* Event bits returned by the mutating calls, so the caller can pick sounds and
 * effects without re-deriving what happened. */
#define TETRIS_EV_MOVE   (1U << 0)
#define TETRIS_EV_ROTATE (1U << 1)
#define TETRIS_EV_LOCK   (1U << 2)
#define TETRIS_EV_CLEAR  (1U << 3)
#define TETRIS_EV_TETRIS (1U << 4)  /* four rows at once */
#define TETRIS_EV_LEVEL  (1U << 5)
#define TETRIS_EV_OVER   (1U << 6)

enum tetris_status {
	TETRIS_READY = 0,
	TETRIS_PLAYING,
	TETRIS_PAUSED,
	TETRIS_GAMEOVER,
};

struct tetris {
	/* cell[row][col]: 0 empty, else 1-7 = piece id, used as a colour and
	 * pattern index so pieces stay distinguishable without relying on
	 * colour alone (Section 106). */
	uint8_t cell[TETRIS_ROWS][TETRIS_COLS];

	uint8_t piece;      /* 1-7 */
	uint8_t next;
	uint8_t rot;        /* 0-3 */
	int8_t x;           /* left edge of the 4x4 box */
	int8_t y;           /* top edge of the 4x4 box */

	uint32_t score;
	uint16_t lines;
	uint8_t level;
	uint8_t last_cleared;

	enum tetris_status status;

	/* 7-bag randomiser: every piece appears once per seven, so you never
	 * wait fifteen pieces for an I. */
	uint8_t bag[7];
	uint8_t bag_pos;
	uint32_t rng;
};

void tetris_init(struct tetris *t, uint32_t seed);

/** @return event bits; TETRIS_EV_OVER if the new piece cannot be placed. */
uint32_t tetris_spawn(struct tetris *t);

uint32_t tetris_move(struct tetris *t, int dx);
uint32_t tetris_rotate(struct tetris *t);

/** One gravity step. Locks, clears and spawns when it cannot fall further. */
uint32_t tetris_step(struct tetris *t);

/** Player-driven single-row drop; awards 1 point per row (Section 46). */
uint32_t tetris_soft_drop(struct tetris *t);

/** Slam to the floor and lock; awards 2 points per row travelled. */
uint32_t tetris_hard_drop(struct tetris *t);

/** Gravity period for the current level, in milliseconds (Section 45). */
uint32_t tetris_gravity_ms(const struct tetris *t);

/**
 * Cell as it should be drawn, active piece composited in.
 * @param row 0 == top *visible* row; the hidden spawn rows are not addressable.
 */
uint8_t tetris_render_cell(const struct tetris *t, int row, int col);

/** True if the 4x4 box of @p piece/@p rot has a block at (r,c). */
bool tetris_shape_has(uint8_t piece, uint8_t rot, int r, int c);

#endif /* NEXUS_TETRIS_CORE_H_ */
