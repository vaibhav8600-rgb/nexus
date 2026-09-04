/*
 * Screen stack + refresh scheduler (Sections 54-56, 60-61).
 *
 * A screen is a draw function and an action table, not a widget tree. draw()
 * is called once per compositor band with the scene clipped to that band, so
 * it must be a pure function of the model: no allocation, no side effects, no
 * assumption about how many times it runs per frame.
 *
 * Everything here executes on ZMK's display work queue. NEXUS starts no thread
 * of its own (Sections 89, 122), and nothing here can block ZMK, BLE, USB or
 * Studio (Requirement F).
 */
#ifndef NEXUS_SCREEN_H_
#define NEXUS_SCREEN_H_

#include <nexus/action.h>
#include <zephyr/kernel.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nexus_refresh {
	NEXUS_REFRESH_IDLE = 0, /* static content       */
	NEXUS_REFRESH_NORMAL,   /* status dashboard     */
	NEXUS_REFRESH_FAST,     /* games, animations    */
};

struct nexus_screen {
	const char *name;

	/** Take the screen: subscribe, reset state, arm timers. May be NULL. */
	void (*enter)(void);
	/** Release the screen: unsubscribe, cancel timers. May be NULL. */
	void (*exit)(void);

	/** Paint the whole scene. Called once per band; ops self-clip. */
	void (*draw)(void);

	/** @return true if the action was consumed. */
	bool (*action)(enum nexus_action action);

	/** Called at the screen's refresh rate; may be NULL. */
	void (*tick)(void);

	enum nexus_refresh refresh;

	/*
	 * Section 12's context-sensitive button, expressed as data. The screen
	 * decides what SELECT/BACK mean inside action(); the button knows
	 * nothing about screens, and a keymap key bound to the same logical
	 * action behaves identically (Section 13).
	 */
	enum nexus_action btn_short;
	enum nexus_action btn_long;
};

void nexus_screen_push(const struct nexus_screen *screen);
void nexus_screen_pop(void);
void nexus_screen_replace(const struct nexus_screen *screen);
void nexus_screen_home(void);
const struct nexus_screen *nexus_screen_current(void);

/** Mark the whole screen for repaint on the next frame. */
void nexus_screen_invalidate(void);

/**
 * Mark rows [y0, y1) for repaint. A WPM change costs 2 bands over SPI instead
 * of 20 - this is the whole of Section 61.
 */
void nexus_screen_invalidate_rows(int y0, int y1);

/**
 * Paint any pending dirty range immediately, bypassing the coalesce window.
 *
 * For input handling only - a keypress should never wait out a timer sized
 * for status events. Must be called from the NEXUS work queue.
 */
void nexus_screen_render_now(void);

/** Raise the refresh class until the next screen change (e.g. mid-animation). */
void nexus_screen_request_refresh(enum nexus_refresh rate);

#if IS_ENABLED(CONFIG_NEXUS_DEBUG)
/**
 * Frames actually pushed to the panel in the last second (Section 64).
 *
 * Debug builds only: the counter itself is free, but a diagnostics row that
 * repaints once a second to display it is not, and production has no use for
 * it.
 */
uint8_t nexus_screen_fps(void);
#endif

/* Screen descriptors provided by the UI layer. Guarded so a build without a
 * game center still links. */
extern const struct nexus_screen nexus_screen_home_def;
extern const struct nexus_screen nexus_screen_settings_def;
extern const struct nexus_screen nexus_screen_diagnostics_def;
extern const struct nexus_screen nexus_screen_about_def;
#if IS_ENABLED(CONFIG_NEXUS_SPLASH)
extern const struct nexus_screen nexus_screen_splash_def;
#endif
#if IS_ENABLED(CONFIG_NEXUS_GAME_CENTER)
extern const struct nexus_screen nexus_screen_game_center_def;
extern const struct nexus_screen nexus_screen_game_def;
#endif

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_SCREEN_H_ */
