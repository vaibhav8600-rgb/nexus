/*
 * Remote Input internals, shared by remote.c (Bluetooth) and remote_hid.c
 * (everything that reaches ZMK's HID). Not part of the module API.
 *
 * Threading, which is the whole design:
 *   - GATT write handlers run on the Bluetooth RX thread. They validate,
 *     copy into the queue or the mouse accumulator, and return. Nothing
 *     from there touches HID.
 *   - Every HID call runs on the system work queue - the same queue ZMK
 *     processes the halves' key events on - so phone input and keyboard
 *     input never race over ZMK's report state.
 */
#ifndef NEXUS_REMOTE_PRIV_H_
#define NEXUS_REMOTE_PRIV_H_

#include <stdbool.h>
#include <stdint.h>

#include "remote_proto.h"

/* ---- remote_hid.c, callable from any thread ---------------------------- */

/** Queue a Key packet behind any pending text. @return 0 or REMOTE_ERR_FULL. */
int remote_hid_key(const struct remote_key *key);

/** Queue validated text. @return 0 or REMOTE_ERR_FULL. */
int remote_hid_text(const uint8_t *buf, uint16_t len);

/** Merge a Mouse packet into the pending report. */
void remote_hid_mouse(const struct remote_mouse *m);

/** A NEXUS action from the phone, pressed or released. Validated already. */
void remote_hid_action(uint8_t action, bool down);

/** Traffic arrived: push the hold watchdog out. */
void remote_hid_keepalive(void);

/** Let go of everything held; @p drop_text also empties the queue. */
void remote_hid_release_all(bool drop_text);

/** Typing delay for this session, clamped to the protocol's range. */
void remote_hid_set_delay(uint8_t ms);

uint16_t remote_hid_text_free(void);
bool remote_hid_typing(void);

/* ---- remote.c ----------------------------------------------------------- */

/** Something the Status characteristic reports changed. Any thread. */
void remote_status_kick(void);

/** Close the pairing window and abandon a pairing in progress. */
void remote_pair_cancel(void);

#endif /* NEXUS_REMOTE_PRIV_H_ */
