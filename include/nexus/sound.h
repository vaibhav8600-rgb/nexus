/*
 * Sound engine (Sections 16, 48, 73, 108).
 *
 * Games and UI ask for a named effect. Only src/hal/nexus_buzzer.c knows a PWM
 * exists. Tones are synthesised, not sampled - a passive buzzer wants a square
 * wave and flash is precious.
 */
#ifndef NEXUS_SOUND_H_
#define NEXUS_SOUND_H_

#include <zephyr/kernel.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nexus_sound {
	NEXUS_SOUND_STARTUP = 0,
	NEXUS_SOUND_SELECT,
	NEXUS_SOUND_BACK,
	NEXUS_SOUND_CONNECT,
	NEXUS_SOUND_DISCONNECT,
	NEXUS_SOUND_MENU_OPEN,
	NEXUS_SOUND_MENU_SELECT,
	NEXUS_SOUND_GAME_START,
	NEXUS_SOUND_GAME_PAUSE,
	NEXUS_SOUND_GAME_RESUME,
	NEXUS_SOUND_GAME_OVER,
	NEXUS_SOUND_TETRIS_MOVE,
	NEXUS_SOUND_TETRIS_ROTATE,
	NEXUS_SOUND_TETRIS_DROP,
	NEXUS_SOUND_TETRIS_LINE,
	NEXUS_SOUND_TETRIS_TETRIS,
	NEXUS_SOUND_TETRIS_LEVEL,
	NEXUS_SOUND_COUNT,
};

#if IS_ENABLED(CONFIG_NEXUS_SOUND)

/** Queue an effect. No-op if muted, unbuilt, or the buzzer is missing. */
void nexus_sound_play(enum nexus_sound id);

void nexus_sound_set_enabled(bool enabled);
bool nexus_sound_enabled(void);
void nexus_sound_stop(void);

#else /* sound compiled out - callers stay unconditional and cost nothing */

static inline void nexus_sound_play(enum nexus_sound id) { (void)id; }
static inline void nexus_sound_set_enabled(bool enabled) { (void)enabled; }
static inline bool nexus_sound_enabled(void) { return false; }
static inline void nexus_sound_stop(void) { }

#endif

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_SOUND_H_ */
