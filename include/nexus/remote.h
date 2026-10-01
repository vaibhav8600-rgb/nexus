/*
 * Remote Input: a phone as keyboard and mouse (docs/remote-input.md).
 *
 * Declarations only, and safe to include from any build: nothing here is
 * defined unless CONFIG_NEXUS_REMOTE_INPUT is set, and every caller is
 * guarded where IS_ENABLED() is in scope.
 */
#ifndef NEXUS_REMOTE_H_
#define NEXUS_REMOTE_H_

#include <nexus/action.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handle NEXUS_ACTION_REMOTE_*. Runs on the NEXUS work queue.
 *
 * @return true if @p action was a remote action (consumed).
 */
bool nexus_remote_action(enum nexus_action action);

/* The pairing / identify screen, for the UI layer. */
enum nexus_remote_view {
	NEXUS_REMOTE_VIEW_WAIT,    /* window open, no phone has asked yet */
	NEXUS_REMOTE_VIEW_PASSKEY, /* type this number into the phone     */
	NEXUS_REMOTE_VIEW_HELLO,   /* Control 0x05: "this is the one"     */
	NEXUS_REMOTE_VIEW_FULL,    /* no phone slot or profile free       */
};

/*
 * Both are safe from any thread - they are called from Bluetooth callbacks -
 * and do their screen work on the NEXUS queue. Without the display they only
 * record, and nothing is drawn.
 */

/** Show @p view; replaces what the remote screen shows if already open. */
void nexus_remote_screen_show(enum nexus_remote_view view, uint32_t passkey);

/**
 * Close the remote screen, but only if it is showing @p view: the pairing
 * window running out must not take a passkey someone is typing with it.
 */
void nexus_remote_screen_hide(enum nexus_remote_view view);

/** Seconds left in the pairing window, 0 when closed. */
uint32_t nexus_remote_pair_remaining_s(void);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_REMOTE_H_ */
