/*
 * Game interface + manager (Sections 37-38, 42, 51, 72).
 *
 * A game gets a tick, logical input verbs and a draw call. It never learns
 * which screen hosts it, which panel it is on, what a keycode is, or that a
 * GPIO exists. draw() renders through the same compositor API the dashboard
 * uses, so swapping the ST7789 for another panel does not touch a game
 * (Sections 72, 141-D).
 *
 * Adding a game is one file plus one entry in the registry in game_manager.c.
 */
#ifndef NEXUS_GAME_H_
#define NEXUS_GAME_H_

#include <nexus/action.h>
#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nexus_game_state {
	NEXUS_GAME_IDLE = 0,
	NEXUS_GAME_RUNNING,
	NEXUS_GAME_PAUSED,
	NEXUS_GAME_OVER,
};

struct nexus_game {
	const char *id;   /* settings key fragment, e.g. "tetris" */
	const char *name; /* shown in the launcher */

	/** Reset to a fresh round and arm whatever clock the game needs. */
	void (*start)(void);
	/** Advance simulation. Only for games that tick with the screen; a game
	 *  with its own gravity clock (Tetris) can leave this empty. */
	void (*update)(void);
	/** @return true if consumed. */
	bool (*input)(enum nexus_action action);
	void (*pause)(void);
	void (*resume)(void);
	/** Cancel timers and release anything the round owned. */
	void (*stop)(void);

	/** Paint the whole scene. Called once per compositor band. */
	void (*draw)(void);

	uint32_t (*score)(void);
	enum nexus_game_state (*state)(void);

	/** Optional launcher artwork, centred on (@p cx, @p cy). */
	void (*draw_icon)(int cx, int cy);
};

/* ---- manager ----------------------------------------------------------- */

uint8_t nexus_game_count(void);
const struct nexus_game *nexus_game_at(uint8_t index);
const struct nexus_game *nexus_game_active(void);

int nexus_game_launch(uint8_t index);
void nexus_game_stop(void);
void nexus_game_tick(void);
void nexus_game_draw(void);
bool nexus_game_input(enum nexus_action action);
void nexus_game_pause(void);
void nexus_game_resume(void);
enum nexus_game_state nexus_game_state(void);

/*
 * Difficulty, 1 (slowest) .. 5 (fastest), 3 = the Kconfig defaults exactly.
 *
 * A live setting rather than a build-time constant, because "the ball is too
 * slow" is a judgement you make while playing, and a knob you have to reflash
 * to turn is a knob you turn once and then live with. It persists with sound,
 * theme and brightness.
 *
 * Games scale their own clock by it; there is no shared notion of speed,
 * because a tick interval and a pixels-per-tick velocity do not respond to
 * the same multiplier.
 */
#define NEXUS_GAME_SPEED_MIN 1
#define NEXUS_GAME_SPEED_MAX 6
#define NEXUS_GAME_SPEED_DEFAULT 3

uint8_t nexus_game_speed(void);

/**
 * How much harder level @p level is than level 1, as a percentage.
 *
 * 100 at level 1, +12 per level after it, capped at 200. Two things make the
 * cap non-negotiable: the tick can never go below
 * CONFIG_NEXUS_UI_REFRESH_FAST_MS - the panel simply will not repaint faster -
 * and a velocity that outruns half the paddle tunnels straight through it.
 * A level that cannot be survived is not difficulty, it is an ending with
 * extra steps, so the curve flattens at level 9 and the boards themselves get
 * harder after that.
 *
 * One helper for both kinds of clock, because a percentage divides an
 * interval and multiplies a velocity - which is the whole reason games here
 * do not share a "speed" number.
 *
 *     interval = base * 100 / pct;    velocity = base * pct / 100;
 */
uint16_t nexus_game_level_pct(uint8_t level);
void nexus_game_speed_set(uint8_t speed);
/** Human label for the current setting, for the Settings screen. */
const char *nexus_game_speed_name(void);

/**
 * Snake wraps at the edges (true) or dies on them (false).
 *
 * A setting rather than only a Kconfig for the same reason speed is: walls or
 * no walls is the single biggest change to how Snake plays, and it is not a
 * decision worth a reflash. CONFIG_NEXUS_SNAKE_WRAP is the power-on default.
 *
 * It lives here beside the speed knob rather than inside snake.c because
 * settings.c has to reach it to restore it, and settings.c must not depend on
 * which games were built in.
 */
bool nexus_snake_wrap(void);
void nexus_snake_wrap_set(bool wrap);

uint32_t nexus_game_highscore(const struct nexus_game *game);
/** Persists only when the record actually improves (Section 107). */
void nexus_game_submit_score(const struct nexus_game *game, uint32_t score);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_GAME_H_ */
