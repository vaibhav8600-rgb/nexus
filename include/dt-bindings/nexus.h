/*
 * Keymap-visible NEXUS action IDs, e.g. `&nexus_action NEXUS_ACT_GAME_CENTER`.
 * Shared verbatim by C and devicetree so the two can never drift.
 */
#ifndef DT_BINDINGS_NEXUS_H_
#define DT_BINDINGS_NEXUS_H_

#define NEXUS_ACT_SELECT       0
#define NEXUS_ACT_BACK         1
#define NEXUS_ACT_PAUSE        2
#define NEXUS_ACT_RESUME       3
#define NEXUS_ACT_HOME         4
#define NEXUS_ACT_GAME_CENTER  5
#define NEXUS_ACT_RESTART      6
#define NEXUS_ACT_NEXT         7
#define NEXUS_ACT_PREVIOUS     8
#define NEXUS_ACT_MENU         9
#define NEXUS_ACT_LEFT        10
#define NEXUS_ACT_RIGHT       11
#define NEXUS_ACT_UP          12
#define NEXUS_ACT_DOWN        13
#define NEXUS_ACT_ROTATE      14
#define NEXUS_ACT_DROP        15

/* Commit the current sound / theme / brightness to storage. Exists so the
 * keyboard can do everything the dongle button can, and then some. */
#define NEXUS_ACT_SAVE        16

#endif /* DT_BINDINGS_NEXUS_H_ */
