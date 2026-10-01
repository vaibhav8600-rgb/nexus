/*
 * Remote Input wire format: the example bytes in docs/remote-input-protocol.md
 * and the US ASCII table. Host-side, no Zephyr:
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/remote \
 *      tests/remote/test_remote_proto.c src/remote/remote_proto.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote_proto.h"

static int g_fail;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
			g_fail++;                                              \
		}                                                              \
	} while (0)

static void test_ascii(void)
{
	bool shift;

	/* Spot checks against the HID usage tables. */
	CHECK(remote_ascii_usage('a', &shift) == 0x04 && !shift);
	CHECK(remote_ascii_usage('Z', &shift) == 0x1D && shift);
	CHECK(remote_ascii_usage('0', &shift) == 0x27 && !shift);
	CHECK(remote_ascii_usage('1', &shift) == 0x1E && !shift);
	CHECK(remote_ascii_usage('!', &shift) == 0x1E && shift);
	CHECK(remote_ascii_usage('@', &shift) == 0x1F && shift);
	CHECK(remote_ascii_usage(' ', &shift) == 0x2C && !shift);
	CHECK(remote_ascii_usage('~', &shift) == 0x35 && shift);
	CHECK(remote_ascii_usage('`', &shift) == 0x35 && !shift);
	CHECK(remote_ascii_usage('"', &shift) == 0x34 && shift);
	CHECK(remote_ascii_usage('\'', &shift) == 0x34 && !shift);
	CHECK(remote_ascii_usage('_', &shift) == 0x2D && shift);
	CHECK(remote_ascii_usage('|', &shift) == 0x31 && shift);
	CHECK(remote_ascii_usage('\n', &shift) == 0x28 && !shift);
	CHECK(remote_ascii_usage('\t', &shift) == 0x2B && !shift);

	/* Everything printable types, nothing else does. */
	for (int c = 0; c < 256; c++) {
		uint8_t u = remote_ascii_usage((uint8_t)c, &shift);
		bool typeable = (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\t';

		CHECK((u != 0) == typeable);
	}

	/* Letters: lower and upper share a key, only shift differs. */
	for (int c = 'a'; c <= 'z'; c++) {
		bool s1, s2;

		CHECK(remote_ascii_usage((uint8_t)c, &s1) ==
		      remote_ascii_usage((uint8_t)(c - 32), &s2));
		CHECK(!s1 && s2);
	}

	/* No two characters with the same shift state share a key. */
	for (int a = 0x20; a <= 0x7E; a++) {
		for (int b = a + 1; b <= 0x7E; b++) {
			bool sa, sb;
			uint8_t ua = remote_ascii_usage((uint8_t)a, &sa);
			uint8_t ub = remote_ascii_usage((uint8_t)b, &sb);

			CHECK(!(ua == ub && sa == sb));
		}
	}
}

static void test_text(void)
{
	CHECK(remote_text_check((const uint8_t *)"Hi\n", 3) == 0);
	CHECK(remote_text_check((const uint8_t *)"", 0) == REMOTE_ERR_LEN);
	CHECK(remote_text_check((const uint8_t *)"caf\xc3\xa9", 5) ==
	      REMOTE_ERR_VALUE);
	CHECK(remote_text_check((const uint8_t *)"a\rb", 3) == REMOTE_ERR_VALUE);
}

static void test_key(void)
{
	struct remote_key k;

	/* tap Ctrl+C */
	const uint8_t ctrl_c[] = {0x02, 0x01, 0x07, 0x00, 0x06, 0x00};
	CHECK(remote_key_parse(ctrl_c, 6, &k) == 0);
	CHECK(k.action == REMOTE_KEY_TAP && k.mods == 0x01);
	CHECK(k.page == REMOTE_PAGE_KEYBOARD && k.usage == 0x06);

	/* hold LCtrl alone */
	const uint8_t lctrl[] = {0x01, 0x01, 0x07, 0x00, 0x00, 0x00};
	CHECK(remote_key_parse(lctrl, 6, &k) == 0 && k.usage == 0);

	/* tap Play/Pause */
	const uint8_t play[] = {0x02, 0x00, 0x0C, 0x00, 0xCD, 0x00};
	CHECK(remote_key_parse(play, 6, &k) == 0);
	CHECK(k.page == REMOTE_PAGE_CONSUMER && k.usage == 0xCD);

	/* Rejections. */
	const uint8_t nothing[] = {0x01, 0x00, 0x07, 0x00, 0x00, 0x00};
	const uint8_t bad_action[] = {0x03, 0x00, 0x07, 0x00, 0x04, 0x00};
	const uint8_t bad_page[] = {0x02, 0x00, 0x01, 0x00, 0x04, 0x00};
	const uint8_t big_key[] = {0x02, 0x00, 0x07, 0x00, 0x00, 0x01};
	const uint8_t big_cons[] = {0x02, 0x00, 0x0C, 0x00, 0x00, 0x10};

	CHECK(remote_key_parse(nothing, 6, &k) == REMOTE_ERR_VALUE);
	CHECK(remote_key_parse(bad_action, 6, &k) == REMOTE_ERR_VALUE);
	CHECK(remote_key_parse(bad_page, 6, &k) == REMOTE_ERR_VALUE);
	CHECK(remote_key_parse(big_key, 6, &k) == REMOTE_ERR_VALUE);
	CHECK(remote_key_parse(big_cons, 6, &k) == REMOTE_ERR_VALUE);
	CHECK(remote_key_parse(ctrl_c, 5, &k) == REMOTE_ERR_LEN);
}

static void test_mouse(void)
{
	struct remote_mouse m;
	const uint8_t pkt[] = {0x01, 0x05, 0x00, 0xFD, 0xFF, 0x01, 0x00, 0x00};

	CHECK(remote_mouse_parse(pkt, 8, &m) == 0);
	CHECK(m.buttons == 0x01 && m.dx == 5 && m.dy == -3);
	CHECK(m.wheel == 1 && m.hwheel == 0);

	const uint8_t extremes[] = {0xFF, 0x00, 0x80, 0xFF, 0x7F, 0x80, 0x7F, 0};
	CHECK(remote_mouse_parse(extremes, 8, &m) == 0);
	CHECK(m.buttons == 0x1F); /* bits 5-7 are not buttons */
	CHECK(m.dx == -32768 && m.dy == 32767);
	CHECK(m.wheel == -128 && m.hwheel == 127);

	CHECK(remote_mouse_parse(pkt, 7, &m) == REMOTE_ERR_LEN);
}

/* Control 0x06: the game-layer verbs only, never the Remote Input ones. */
static void test_action(void)
{
	const uint8_t up_down[] = {0x06, 13, 1};   /* UP, pressed */
	const uint8_t sel_up[] = {0x06, 1, 0};     /* SELECT, released */
	const uint8_t host[] = {0x06, 20, 1};      /* HOST, the last one */
	const uint8_t none[] = {0x06, 0, 1};
	const uint8_t toggle[] = {0x06, 21, 1};    /* REMOTE_TOGGLE */
	const uint8_t clear[] = {0x06, 23, 1};     /* REMOTE_CLEAR */
	const uint8_t state[] = {0x06, 13, 2};

	CHECK(remote_action_check(up_down, 3) == 0);
	CHECK(remote_action_check(sel_up, 3) == 0);
	CHECK(remote_action_check(host, 3) == 0);
	CHECK(remote_action_check(none, 3) == REMOTE_ERR_VALUE);
	CHECK(remote_action_check(toggle, 3) == REMOTE_ERR_VALUE);
	CHECK(remote_action_check(clear, 3) == REMOTE_ERR_VALUE);
	CHECK(remote_action_check(state, 3) == REMOTE_ERR_VALUE);
	CHECK(remote_action_check(up_down, 2) == REMOTE_ERR_LEN);
}

int main(void)
{
	test_ascii();
	test_text();
	test_key();
	test_mouse();
	test_action();

	if (g_fail) {
		printf("%d check(s) failed\n", g_fail);
		return EXIT_FAILURE;
	}
	printf("remote proto: all checks passed\n");
	return EXIT_SUCCESS;
}
