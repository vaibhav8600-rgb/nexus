/*
 * Keymap-visible NEXUS action IDs, e.g. `&nexus_action NEXUS_ACT_GAME_CENTER`.
 * Shared verbatim by C and devicetree so the two can never drift.
 */
#ifndef DT_BINDINGS_NEXUS_H_
#define DT_BINDINGS_NEXUS_H_

/*
 * 0 is NOT an action, and that is load-bearing.
 *
 * SELECT used to be 0, which meant a `struct nexus_screen` field left out of
 * its initialiser - btn_long, btn_double - defaulted to SELECT rather than to
 * nothing. The Game Center's double-tap shipped broken on exactly that: the
 * code used "btn_double != 0" as "this screen has a double", and SELECT being
 * 0 made a single tap store a falsy pending value, so neither gesture fired.
 *
 * With NONE at 0 the C default means what everybody assumes it means.
 */
#define NEXUS_ACT_NONE         0

#define NEXUS_ACT_SELECT       1
#define NEXUS_ACT_BACK         2
#define NEXUS_ACT_PAUSE        3
#define NEXUS_ACT_RESUME       4
#define NEXUS_ACT_HOME         5
#define NEXUS_ACT_GAME_CENTER  6
#define NEXUS_ACT_RESTART      7
#define NEXUS_ACT_NEXT         8
#define NEXUS_ACT_PREVIOUS     9
#define NEXUS_ACT_MENU         10
#define NEXUS_ACT_LEFT        11
#define NEXUS_ACT_RIGHT       12
#define NEXUS_ACT_UP          13
#define NEXUS_ACT_DOWN        14
#define NEXUS_ACT_ROTATE      15
#define NEXUS_ACT_DROP        16

/* Commit the current sound / theme / brightness to storage. Exists so the
 * keyboard can do everything the dongle button can, and then some. */
#define NEXUS_ACT_SAVE        17

/* Cycle the theme without opening Settings. Meant for an encoder, which is
 * why they are a pair rather than one toggle. */
#define NEXUS_ACT_THEME_NEXT  18
#define NEXUS_ACT_THEME_PREV  19

#endif /* DT_BINDINGS_NEXUS_H_ */
