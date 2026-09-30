/*
 * Remote Input -> ZMK HID.
 *
 * Keys and text share one ordered byte queue, typed out by one delayable work
 * item, so "type this, then press Enter" cannot arrive the other way round.
 * The mouse does not queue at all: packets are summed into one pending report
 * and whatever has accumulated goes out the next time the work item runs, so
 * a burst from the phone costs one HID report instead of a backlog.
 *
 * Like src/status/zmk_events.c, this is the one Remote Input file that talks
 * to ZMK's HID and event APIs - a ZMK bump that moves them is a fix here.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk/endpoints.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#include <nexus/status.h>

#include "remote_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/*
 * Queue records. Text is itself - every typeable byte is 0x09, 0x0A or
 * 0x20-0x7E - so a byte below 0x09 can only be an opcode.
 */
#define OP_KEY 0x01 /* + action, mods, page, usage lo, usage hi */
#define KEY_REC 6

/* ponytail: 12 is more keys than a phone UI can hold at once; a press past
 * it is dropped rather than growing the table. */
#define MAX_HELD 12

#define MOD_USAGE(bit) ZMK_HID_USAGE(HID_USAGE_KEY, 0xE0 + (bit))

RING_BUF_DECLARE(g_queue, CONFIG_NEXUS_REMOTE_INPUT_TEXT_QUEUE_SIZE);
static struct k_spinlock g_qlock;

/* Everything below this line is touched only on the system work queue. */
static uint32_t g_held[MAX_HELD];
static uint8_t g_nheld;
static uint32_t g_typed;   /* key the typer pressed and will release next */
static uint8_t g_btn_sent; /* mouse buttons we have pressed */
static uint8_t g_delay = CONFIG_NEXUS_REMOTE_INPUT_TYPE_DELAY_MS;
static bool g_typing;

/* Mouse accumulator: written on the BT thread, drained on the work queue. */
static struct k_spinlock g_mlock;
static int32_t g_mx, g_my, g_mwheel, g_mhwheel;
static uint8_t g_mbtn;

static atomic_t g_drop_text;

static void step_fn(struct k_work *work);
static void mouse_fn(struct k_work *work);
static void release_fn(struct k_work *work);
static void watchdog_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_step, step_fn);
static K_WORK_DEFINE(g_mouse_work, mouse_fn);
static K_WORK_DEFINE(g_release_work, release_fn);
static K_WORK_DELAYABLE_DEFINE(g_watchdog, watchdog_fn);

/* ---- keys ---------------------------------------------------------------- */

static void key_event(uint32_t encoded, bool pressed)
{
	raise_zmk_keycode_state_changed_from_encoded(encoded, pressed,
						     k_uptime_get());
}

static void hold(uint32_t encoded)
{
	for (int i = 0; i < g_nheld; i++) {
		if (g_held[i] == encoded) {
			return;
		}
	}
	if (g_nheld == MAX_HELD) {
		LOG_WRN("remote: %d keys held, ignoring another", MAX_HELD);
		return;
	}
	g_held[g_nheld++] = encoded;
	key_event(encoded, true);
}

static void unhold(uint32_t encoded)
{
	for (int i = 0; i < g_nheld; i++) {
		if (g_held[i] == encoded) {
			key_event(encoded, false);
			g_held[i] = g_held[--g_nheld];
			return;
		}
	}
}

/** @return how long to wait before the next queue step. */
static k_timeout_t do_key(const uint8_t *rec)
{
	uint8_t action = rec[1];
	uint8_t mods = rec[2];
	uint8_t page = rec[3];
	uint16_t usage = (uint16_t)(rec[4] | (rec[5] << 8));
	uint32_t key = usage ? ZMK_HID_USAGE(page, usage) : 0;

	switch (action) {
	case REMOTE_KEY_PRESS:
		for (int b = 0; b < 8; b++) {
			if (mods & BIT(b)) {
				hold(MOD_USAGE(b));
			}
		}
		if (key) {
			hold(key);
		}
		return K_NO_WAIT;

	case REMOTE_KEY_RELEASE:
		if (key) {
			unhold(key);
		}
		for (int b = 0; b < 8; b++) {
			if (mods & BIT(b)) {
				unhold(MOD_USAGE(b));
			}
		}
		return K_NO_WAIT;

	default: /* REMOTE_KEY_TAP */
		if (page == REMOTE_PAGE_CONSUMER) {
			mods = 0;
		} else if (!key) {
			/* A modifier on its own - tapping Win. The lowest one
			 * becomes the key, any others ride along with it. */
			int b = __builtin_ctz(mods);

			key = MOD_USAGE(b);
			mods &= (uint8_t)~BIT(b);
		}
		/* Modifiers in the same report as the key, which is what
		 * ZMK's own &kp LC(C) does. */
		g_typed = APPLY_MODS((uint32_t)mods, key);
		key_event(g_typed, true);
		return K_MSEC(g_delay);
	}
}

static k_timeout_t type_char(uint8_t c)
{
	bool shift;
	uint8_t usage = remote_ascii_usage(c, &shift);
	bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

	/* With Caps Lock on the host, 'a' needs Shift to stay lower case. */
	if (letter && nexus_status_get()->caps_lock) {
		shift = !shift;
	}

	g_typed = ZMK_HID_USAGE(HID_USAGE_KEY, usage);
	if (shift) {
		g_typed = APPLY_MODS((uint32_t)MOD_LSFT, g_typed);
	}
	key_event(g_typed, true);
	return K_MSEC(g_delay);
}

static void set_typing(bool typing)
{
	if (g_typing != typing) {
		g_typing = typing;
		remote_status_kick();
	}
}

/*
 * One step: release what the last step pressed, or take the next record.
 * Each character is press, wait, release, wait - two HID reports at the
 * typing delay apart, which every host sees as a distinct keystroke.
 */
static void step_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_typed) {
		key_event(g_typed, false);
		g_typed = 0;
		k_work_schedule(&g_step, K_MSEC(g_delay));
		return;
	}

	uint8_t rec[KEY_REC];
	uint32_t n = 0;

	K_SPINLOCK(&g_qlock) {
		n = ring_buf_get(&g_queue, rec, 1);
		if (n && rec[0] == OP_KEY) {
			/* Records go in whole, so the rest is there. */
			n += ring_buf_get(&g_queue, rec + 1, KEY_REC - 1);
		}
	}

	if (n == 0) {
		set_typing(false);
		return;
	}

	set_typing(true);
	k_work_schedule(&g_step,
			rec[0] == OP_KEY ? do_key(rec) : type_char(rec[0]));

	/* text_free moved. The kick is rate limited on the other side. */
	remote_status_kick();
}

static int enqueue(const uint8_t *data, uint32_t len)
{
	int err = 0;

	K_SPINLOCK(&g_qlock) {
		if (ring_buf_space_get(&g_queue) < len) {
			err = REMOTE_ERR_FULL;
		} else {
			ring_buf_put(&g_queue, data, len);
		}
	}
	if (err == 0) {
		/* schedule, not reschedule: a step already waiting out the
		 * typing delay keeps its place, and so keeps the order. */
		k_work_schedule(&g_step, K_NO_WAIT);
	}
	return err;
}

int remote_hid_key(const struct remote_key *key)
{
	const uint8_t rec[KEY_REC] = {
		OP_KEY, key->action, key->mods, key->page,
		(uint8_t)key->usage, (uint8_t)(key->usage >> 8),
	};

	remote_hid_keepalive();
	return enqueue(rec, sizeof(rec));
}

int remote_hid_text(const uint8_t *buf, uint16_t len)
{
	return enqueue(buf, len);
}

uint16_t remote_hid_text_free(void)
{
	uint32_t free = 0;

	K_SPINLOCK(&g_qlock) {
		free = ring_buf_space_get(&g_queue);
	}
	return (uint16_t)MIN(free, UINT16_MAX);
}

bool remote_hid_typing(void)
{
	return g_typing;
}

void remote_hid_set_delay(uint8_t ms)
{
	g_delay = CLAMP(ms, REMOTE_TYPE_DELAY_MIN, REMOTE_TYPE_DELAY_MAX);
}

/* ---- mouse --------------------------------------------------------------- */

static void send_mouse(uint8_t buttons, int32_t x, int32_t y, int32_t wheel,
		       int32_t hwheel)
{
	uint8_t changed = buttons ^ g_btn_sent;
	bool pending = changed != 0;

	if (changed & buttons) {
		zmk_hid_mouse_buttons_press(changed & buttons);
	}
	if (changed & g_btn_sent) {
		zmk_hid_mouse_buttons_release(changed & g_btn_sent);
	}
	g_btn_sent = buttons;

	/* A sum can outgrow one report; send it in as many as it takes. */
	while (pending || x || y || wheel || hwheel) {
		int16_t sx = (int16_t)CLAMP(x, -INT16_MAX, INT16_MAX);
		int16_t sy = (int16_t)CLAMP(y, -INT16_MAX, INT16_MAX);
		int16_t sw = (int16_t)CLAMP(wheel, -INT16_MAX, INT16_MAX);
		int16_t sh = (int16_t)CLAMP(hwheel, -INT16_MAX, INT16_MAX);

		zmk_hid_mouse_movement_set(sx, sy);
		zmk_hid_mouse_scroll_set(sh, sw);
		zmk_endpoint_send_mouse_report();

		x -= sx;
		y -= sy;
		wheel -= sw;
		hwheel -= sh;
		pending = false;
	}

	/* Movement is relative: leave nothing behind for the next report,
	 * including one ZMK itself sends. */
	zmk_hid_mouse_movement_set(0, 0);
	zmk_hid_mouse_scroll_set(0, 0);
}

static void mouse_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int32_t x = 0, y = 0, wheel = 0, hwheel = 0;
	uint8_t buttons = 0;

	K_SPINLOCK(&g_mlock) {
		x = g_mx;
		y = g_my;
		wheel = g_mwheel;
		hwheel = g_mhwheel;
		buttons = g_mbtn;
		g_mx = g_my = g_mwheel = g_mhwheel = 0;
	}

	send_mouse(buttons, x, y, wheel, hwheel);
}

void remote_hid_mouse(const struct remote_mouse *m)
{
	K_SPINLOCK(&g_mlock) {
		g_mx += m->dx;
		g_my += m->dy;
		g_mwheel += m->wheel;
		g_mhwheel += m->hwheel;
		g_mbtn = m->buttons;
	}
	k_work_submit(&g_mouse_work);
	remote_hid_keepalive();
}

/* ---- safety -------------------------------------------------------------- */

/* Keys and buttons the phone holds - not the typer's, which lets go of its
 * own key one step later whatever happens. */
static void release_holds(void)
{
	while (g_nheld) {
		unhold(g_held[g_nheld - 1]);
	}

	K_SPINLOCK(&g_mlock) {
		g_mx = g_my = g_mwheel = g_mhwheel = 0;
		g_mbtn = 0;
	}
	if (g_btn_sent) {
		send_mouse(0, 0, 0, 0, 0);
	}
}

static void release_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_clear(&g_drop_text)) {
		K_SPINLOCK(&g_qlock) {
			ring_buf_reset(&g_queue);
		}
		if (g_typed) {
			key_event(g_typed, false);
			g_typed = 0;
		}
		remote_status_kick();
	}
	release_holds();
}

static void watchdog_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_nheld || g_btn_sent) {
		LOG_WRN("remote: no traffic for %d ms, releasing",
			CONFIG_NEXUS_REMOTE_INPUT_HOLD_TIMEOUT_MS);
		release_holds();
	}
}

void remote_hid_keepalive(void)
{
	k_work_reschedule(&g_watchdog,
			  K_MSEC(CONFIG_NEXUS_REMOTE_INPUT_HOLD_TIMEOUT_MS));
}

void remote_hid_release_all(bool drop_text)
{
	if (drop_text) {
		atomic_set(&g_drop_text, 1);
	}
	k_work_submit(&g_release_work);
}
