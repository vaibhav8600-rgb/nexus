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

uint32_t nexus_game_highscore(const struct nexus_game *game);
/** Persists only when the record actually improves (Section 107). */
void nexus_game_submit_score(const struct nexus_game *game, uint32_t score);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_GAME_H_ */
