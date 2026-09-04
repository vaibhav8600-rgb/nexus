/*
 * Central action dispatcher (Section 13).
 *
 * The physical button, the &nexus_action keymap behavior and the UI itself all
 * funnel through nexus_action_dispatch(). Screens declare what an action means
 * for them; nothing hard-codes "button 1 does X on screen Y".
 */
#ifndef NEXUS_ACTION_H_
#define NEXUS_ACTION_H_

#include <dt-bindings/nexus.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nexus_action {
	NEXUS_ACTION_SELECT      = NEXUS_ACT_SELECT,
	NEXUS_ACTION_BACK        = NEXUS_ACT_BACK,
	NEXUS_ACTION_PAUSE       = NEXUS_ACT_PAUSE,
	NEXUS_ACTION_RESUME      = NEXUS_ACT_RESUME,
	NEXUS_ACTION_HOME        = NEXUS_ACT_HOME,
	NEXUS_ACTION_GAME_CENTER = NEXUS_ACT_GAME_CENTER,
	NEXUS_ACTION_RESTART     = NEXUS_ACT_RESTART,
	NEXUS_ACTION_NEXT        = NEXUS_ACT_NEXT,
	NEXUS_ACTION_PREVIOUS    = NEXUS_ACT_PREVIOUS,
	NEXUS_ACTION_MENU        = NEXUS_ACT_MENU,

	/* Gameplay verbs. Games never see keycodes or GPIOs (Section 42). */
	NEXUS_ACTION_LEFT        = NEXUS_ACT_LEFT,
	NEXUS_ACTION_RIGHT       = NEXUS_ACT_RIGHT,
	NEXUS_ACTION_UP          = NEXUS_ACT_UP,
	NEXUS_ACTION_DOWN        = NEXUS_ACT_DOWN,
	NEXUS_ACTION_ROTATE      = NEXUS_ACT_ROTATE,
	NEXUS_ACTION_DROP        = NEXUS_ACT_DROP,
};

/**
 * Route a logical action to the active screen.
 *
 * Safe from any context including ISRs: the work is queued onto ZMK's display
 * work queue so LVGL is only ever touched from one thread (Section 122).
 */
void nexus_action_dispatch(enum nexus_action action);

/** Map a physical button gesture to an action using the active screen's table. */
void nexus_action_button_event(bool long_press);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_ACTION_H_ */
