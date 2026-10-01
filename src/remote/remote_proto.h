/*
 * Remote Input wire format (docs/remote-input-protocol.md), RTOS-free.
 *
 * No Zephyr and no ZMK in here, so tests/remote/test_remote_proto.c can check
 * the ASCII table and the validators with a plain C compiler in CI.
 */
#ifndef NEXUS_REMOTE_PROTO_H_
#define NEXUS_REMOTE_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REMOTE_PROTO_VERSION 1

/* ATT error codes a write can fail with. */
#define REMOTE_ERR_LEN   0x0D /* BT_ATT_ERR_INVALID_ATTRIBUTE_LEN */
#define REMOTE_ERR_VALUE 0x13 /* BT_ATT_ERR_VALUE_NOT_ALLOWED     */
#define REMOTE_ERR_OFF   0x80 /* remote mode is off               */
#define REMOTE_ERR_FULL  0x81 /* text queue full                  */

#define REMOTE_PAGE_KEYBOARD 0x07
#define REMOTE_PAGE_CONSUMER 0x0C

enum remote_key_action {
	REMOTE_KEY_RELEASE = 0,
	REMOTE_KEY_PRESS = 1,
	REMOTE_KEY_TAP = 2,
};

struct remote_key {
	uint8_t action; /* enum remote_key_action */
	uint8_t mods;   /* HID modifier byte */
	uint8_t page;   /* REMOTE_PAGE_* */
	uint16_t usage;
};

struct remote_mouse {
	uint8_t buttons; /* bit0 left .. bit4 forward */
	int16_t dx;
	int16_t dy;
	int8_t wheel;
	int8_t hwheel;
};

enum remote_ctrl_op {
	REMOTE_CTRL_RELEASE_ALL = 0x01,
	REMOTE_CTRL_KEEPALIVE = 0x02,
	REMOTE_CTRL_CANCEL_TEXT = 0x03,
	REMOTE_CTRL_TYPE_DELAY = 0x04,
	REMOTE_CTRL_IDENTIFY = 0x05,
	REMOTE_CTRL_ACTION = 0x06, /* u8 NEXUS action, u8 1 down / 0 up */
	REMOTE_CTRL_OUTPUT = 0x07, /* u8 REMOTE_OUTPUT_* */
};

/* Output transports, numbered as ZMK's enum zmk_transport. */
#define REMOTE_OUTPUT_USB 1
#define REMOTE_OUTPUT_BLE 2

/*
 * The NEXUS actions a phone may send: SELECT (1) through HOST (20), the
 * game layer's verbs. Not the Remote Input ones above them - a phone that
 * could switch remote off, open pairing or forget phones could lock itself
 * out or let another phone in. remote.c asserts these match dt-bindings.
 */
#define REMOTE_ACTION_FIRST 1
#define REMOTE_ACTION_LAST  20

#define REMOTE_TYPE_DELAY_MIN 2
#define REMOTE_TYPE_DELAY_MAX 50

/** @return 0, or a REMOTE_ERR_* code. */
int remote_key_parse(const uint8_t *buf, size_t len, struct remote_key *out);
int remote_mouse_parse(const uint8_t *buf, size_t len, struct remote_mouse *out);

/** @return 0, or a REMOTE_ERR_* code for a Control 0x06 payload. */
int remote_action_check(const uint8_t *buf, size_t len);

/** @return 0 if every byte is one the dongle can type, else REMOTE_ERR_*. */
int remote_text_check(const uint8_t *buf, size_t len);

/**
 * US-layout keyboard usage for a typeable byte.
 *
 * @param c      0x20-0x7E, '\n' or '\t'
 * @param shift  set to whether Shift must be held
 * @return the usage on page 0x07, or 0 if @p c cannot be typed
 */
uint8_t remote_ascii_usage(uint8_t c, bool *shift);

#endif /* NEXUS_REMOTE_PROTO_H_ */
