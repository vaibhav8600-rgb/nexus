/*
 * Remote Input wire format: parsing, validation and the US ASCII table.
 * See remote_proto.h for why this file knows nothing about Zephyr.
 */

#include "remote_proto.h"

#define S 0x80 /* shift flag, folded into the table below */

/*
 * 0x20..0x7E -> usage | S. One byte per character, 95 bytes of flash, and
 * every usage fits in 7 bits so the shift flag rides along for free.
 */
static const uint8_t k_ascii[95] = {
	/* ' ' ! " # $ % & ' */
	0x2C, S | 0x1E, S | 0x34, S | 0x20, S | 0x21, S | 0x22, S | 0x24, 0x34,
	/* ( ) * + , - . / */
	S | 0x26, S | 0x27, S | 0x25, S | 0x2E, 0x36, 0x2D, 0x37, 0x38,
	/* 0-9 */
	0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
	/* : ; < = > ? @ */
	S | 0x33, 0x33, S | 0x36, 0x2E, S | 0x37, S | 0x38, S | 0x1F,
	/* A-Z */
	S | 0x04, S | 0x05, S | 0x06, S | 0x07, S | 0x08, S | 0x09, S | 0x0A,
	S | 0x0B, S | 0x0C, S | 0x0D, S | 0x0E, S | 0x0F, S | 0x10, S | 0x11,
	S | 0x12, S | 0x13, S | 0x14, S | 0x15, S | 0x16, S | 0x17, S | 0x18,
	S | 0x19, S | 0x1A, S | 0x1B, S | 0x1C, S | 0x1D,
	/* [ \ ] ^ _ ` */
	0x2F, 0x31, 0x30, S | 0x23, S | 0x2D, 0x35,
	/* a-z */
	0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
	0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
	0x1A, 0x1B, 0x1C, 0x1D,
	/* { | } ~ */
	S | 0x2F, S | 0x31, S | 0x30, S | 0x35,
};

uint8_t remote_ascii_usage(uint8_t c, bool *shift)
{
	*shift = false;

	if (c == '\n') {
		return 0x28; /* Enter */
	}
	if (c == '\t') {
		return 0x2B; /* Tab */
	}
	if (c < 0x20 || c > 0x7E) {
		return 0;
	}

	uint8_t e = k_ascii[c - 0x20];

	*shift = (e & S) != 0;
	return e & (uint8_t)~S;
}

int remote_text_check(const uint8_t *buf, size_t len)
{
	bool shift;

	if (len == 0) {
		return REMOTE_ERR_LEN;
	}
	for (size_t i = 0; i < len; i++) {
		if (remote_ascii_usage(buf[i], &shift) == 0) {
			return REMOTE_ERR_VALUE;
		}
	}
	return 0;
}

/* The whole Control write: opcode, action, down. */
int remote_action_check(const uint8_t *buf, size_t len)
{
	if (len != 3) {
		return REMOTE_ERR_LEN;
	}
	if (buf[1] < REMOTE_ACTION_FIRST || buf[1] > REMOTE_ACTION_LAST ||
	    buf[2] > 1) {
		return REMOTE_ERR_VALUE;
	}
	return 0;
}

int remote_key_parse(const uint8_t *buf, size_t len, struct remote_key *out)
{
	if (len != 6) {
		return REMOTE_ERR_LEN;
	}

	uint16_t page = (uint16_t)(buf[2] | (buf[3] << 8));

	out->action = buf[0];
	out->mods = buf[1];
	out->usage = (uint16_t)(buf[4] | (buf[5] << 8));

	if (out->action > REMOTE_KEY_TAP) {
		return REMOTE_ERR_VALUE;
	}

	if (page == REMOTE_PAGE_KEYBOARD) {
		/* Usage 0 is "just the modifiers", so it needs some. */
		if (out->usage > 0xFF || (out->usage == 0 && out->mods == 0)) {
			return REMOTE_ERR_VALUE;
		}
	} else if (page == REMOTE_PAGE_CONSUMER) {
		/* ZMK's full consumer report tops out at 0xFFF. */
		if (out->usage == 0 || out->usage > 0xFFF) {
			return REMOTE_ERR_VALUE;
		}
	} else {
		return REMOTE_ERR_VALUE;
	}

	out->page = (uint8_t)page;
	return 0;
}

int remote_mouse_parse(const uint8_t *buf, size_t len, struct remote_mouse *out)
{
	if (len != 8) {
		return REMOTE_ERR_LEN;
	}

	out->buttons = buf[0] & 0x1F;
	out->dx = (int16_t)(buf[1] | (buf[2] << 8));
	out->dy = (int16_t)(buf[3] | (buf[4] << 8));
	out->wheel = (int8_t)buf[5];
	out->hwheel = (int8_t)buf[6];
	return 0;
}
