/*
 * ZMK -> NEXUS status adapter.
 *
 * THIS IS THE ONLY FILE IN NEXUS THAT INCLUDES zmk/events HEADERS. ZMK's APIs
 * move between releases; keeping every call site in one file means a ZMK bump
 * is a localised fix rather than a hunt (Section 124).
 *
 * Verified against the ZMK revision pinned in examples/nexus-config/config/
 * west.yml - see docs/development.md before bumping it.
 */

#include <nexus/status.h>
#include <nexus/sound.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* HID keyboard modifier byte: left half in bits 0-3, right half in 4-7. */
#define HID_MOD_CTRL  (BIT(0) | BIT(4))
#define HID_MOD_SHIFT (BIT(1) | BIT(5))
#define HID_MOD_ALT   (BIT(2) | BIT(6))
#define HID_MOD_GUI   (BIT(3) | BIT(7))

/* HID LED usage page ordering: 1 NumLock, 2 CapsLock, 3 ScrollLock. */
#define HID_LED_NUM    BIT(0)
#define HID_LED_CAPS   BIT(1)
#define HID_LED_SCROLL BIT(2)

/*
 * ZMK API pin point: layer naming.
 * Current ZMK separates layer *index* (position in the active stack) from
 * layer *id* (stable identity used by Studio). Older trees had a single
 * uint8_t plus zmk_keymap_layer_label(). If a ZMK bump breaks the build, this
 * function is the only thing to fix.
 */
static const char *nexus_layer_name(uint8_t *index_out)
{
	zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
	const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));

	*index_out = (uint8_t)index;

	/* Unnamed layers are normal; never assume "DEFAULT"/"LOWER" exist. */
	return (name && name[0]) ? name : NULL;
}

static void refresh_layer(void)
{
	struct nexus_status *st = nexus_status_mut();
	uint8_t index = 0;
	const char *name = nexus_layer_name(&index);

	if (st->layer_index == index && st->layer_name == name) {
		return;
	}

	st->layer_index = index;
	st->layer_name = name;
	nexus_status_mark(NEXUS_STATUS_LAYER);
}

/* ------------------------------------------------------------------ WPM -- */
#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/events/wpm_state_changed.h>

/*
 * Section 27: the raw counter jitters on every sample. Pull the displayed
 * value a quarter of the way toward the truth per update - live enough to feel
 * responsive, slow enough that the numerals stop flickering.
 */
static void refresh_wpm(uint8_t raw)
{
	struct nexus_status *st = nexus_status_mut();

	/*
	 * Idle snaps straight to zero. ZMK only raises this event when the
	 * value changes, so easing toward 0 a quarter at a time would stall
	 * partway down the moment ZMK stopped sending updates - the dashboard
	 * would sit at "014" long after you stopped typing. Smoothing is there
	 * to stop the numerals jittering while typing, not to invent activity
	 * that has ended (Section 27).
	 */
	if (raw == 0) {
		st->wpm_raw = 0;
		if (st->wpm != 0) {
			st->wpm = 0;
			nexus_status_mark(NEXUS_STATUS_WPM);
		}
		return;
	}

	uint8_t smoothed = (uint8_t)((st->wpm * 3 + raw + 2) / 4);

	/* Without this nudge, integer rounding parks one step short forever. */
	if (smoothed == st->wpm && raw != st->wpm) {
		smoothed = (raw > st->wpm) ? st->wpm + 1 : st->wpm - 1;
	}

	st->wpm_raw = raw;
	if (smoothed != st->wpm) {
		st->wpm = smoothed;
		nexus_status_mark(NEXUS_STATUS_WPM);
	}
}
#endif

/* -------------------------------------------------------------- battery -- */
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>

/* ------------------------------------------------------------- endpoint -- */
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

static void refresh_endpoint(void)
{
	struct nexus_status *st = nexus_status_mut();
	enum nexus_endpoint ep = NEXUS_ENDPOINT_NONE;
	enum nexus_link_state host = NEXUS_LINK_DISCONNECTED;
	/*
	 * ZMK API pin point: endpoint selection. This is the spelling used by
	 * the shipping dongle code this project already builds against; some
	 * trees call it zmk_endpoints_selected(). If a ZMK bump breaks the
	 * build here, that is the alternative to try - and this is the only
	 * line in NEXUS that needs changing.
	 */
	struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();

#if IS_ENABLED(CONFIG_ZMK_USB)
	bool usb_ready = zmk_usb_is_hid_ready();

	if (st->usb_present != usb_ready) {
		st->usb_present = usb_ready;
		nexus_status_mark(NEXUS_STATUS_ENDPOINT);
	}
#endif

	switch (selected.transport) {
#if IS_ENABLED(CONFIG_ZMK_USB)
	case ZMK_TRANSPORT_USB:
		ep = NEXUS_ENDPOINT_USB;
		host = (zmk_usb_get_conn_state() == ZMK_USB_CONN_HID)
			       ? NEXUS_LINK_CONNECTED
			       : NEXUS_LINK_CONNECTING;
		break;
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
	case ZMK_TRANSPORT_BLE:
		ep = NEXUS_ENDPOINT_BLE;
		st->bt_profile = zmk_ble_active_profile_index();
		st->bt_profile_bonded = !zmk_ble_active_profile_is_open();
		if (zmk_ble_active_profile_is_connected()) {
			host = NEXUS_LINK_CONNECTED;
		} else {
			host = st->bt_profile_bonded ? NEXUS_LINK_RECONNECTING
						     : NEXUS_LINK_CONNECTING;
		}
		break;
#endif
	default:
		break;
	}

	if (st->endpoint == ep && st->link_host == host) {
		return;
	}

	bool became = (host == NEXUS_LINK_CONNECTED &&
		       st->link_host != NEXUS_LINK_CONNECTED);
	bool lost = (st->link_host == NEXUS_LINK_CONNECTED &&
		     host != NEXUS_LINK_CONNECTED);

	st->endpoint = ep;
	st->link_host = host;
	nexus_status_mark(NEXUS_STATUS_ENDPOINT);

	if (became) {
		nexus_sound_play(NEXUS_SOUND_CONNECT);
	} else if (lost) {
		nexus_sound_play(NEXUS_SOUND_DISCONNECT);
	}
}

/* --------------------------------------------------------- mods / locks -- */
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

static void refresh_modifiers(void)
{
	struct nexus_status *st = nexus_status_mut();
	/* Explicit mods only: an implicit shift from a shifted keycode is not
	 * something the user is holding, and showing it makes the pills flicker
	 * on every capital letter. */
	uint8_t hid = zmk_hid_get_explicit_mods();
	uint8_t mods = 0;

	mods |= (hid & HID_MOD_CTRL) ? NEXUS_MOD_CTRL : 0;
	mods |= (hid & HID_MOD_SHIFT) ? NEXUS_MOD_SHIFT : 0;
	mods |= (hid & HID_MOD_ALT) ? NEXUS_MOD_ALT : 0;
	mods |= (hid & HID_MOD_GUI) ? NEXUS_MOD_GUI : 0;

	if (mods != st->modifiers) {
		st->modifiers = mods;
		nexus_status_mark(NEXUS_STATUS_MODS);
	}
}

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>

static void refresh_locks(void)
{
	struct nexus_status *st = nexus_status_mut();
	zmk_hid_indicators_t led = zmk_hid_indicators_get_current_profile();
	bool caps = (led & HID_LED_CAPS) != 0;
	bool num = (led & HID_LED_NUM) != 0;
	bool scroll = (led & HID_LED_SCROLL) != 0;

	if (caps == st->caps_lock && num == st->num_lock && scroll == st->scroll_lock) {
		return;
	}

	st->caps_lock = caps;
	st->num_lock = num;
	st->scroll_lock = scroll;
	nexus_status_mark(NEXUS_STATUS_LOCKS);
}
#endif

/* ------------------------------------------------------ mouse jiggler -- */
#if IS_ENABLED(CONFIG_NEXUS_ANTI_IDLE_STATUS)
/*
 * The only event NEXUS consumes that ZMK itself does not define - it comes
 * from the snake-module module, which is why it sits behind a Kconfig rather
 * than being assumed present (Section 99: sit on top of ZMK, do not require
 * anyone else's module).
 */
#include <zmk_dongle_events/anti_idle_state_event.h>

static void refresh_anti_idle(bool active)
{
	struct nexus_status *st = nexus_status_mut();

	if (st->anti_idle != active) {
		st->anti_idle = active;
		nexus_status_mark(NEXUS_STATUS_JIGGLE);
	}
}
#endif

/* ------------------------------------------------------------- listener -- */

static int nexus_status_listener(const zmk_event_t *eh)
{
	if (as_zmk_layer_state_changed(eh)) {
		refresh_layer();
	} else if (as_zmk_keycode_state_changed(eh)) {
		refresh_modifiers();
	} else if (as_zmk_endpoint_changed(eh)) {
		refresh_endpoint();
	}
#if IS_ENABLED(CONFIG_ZMK_USB)
	else if (as_zmk_usb_conn_state_changed(eh)) {
		refresh_endpoint();
	}
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
	else if (as_zmk_ble_active_profile_changed(eh)) {
		refresh_endpoint();
	}
#endif
#if IS_ENABLED(CONFIG_ZMK_WPM)
	else if (as_zmk_wpm_state_changed(eh)) {
		refresh_wpm(as_zmk_wpm_state_changed(eh)->state);
	}
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
	else if (as_zmk_hid_indicators_changed(eh)) {
		refresh_locks();
	}
#endif
#if IS_ENABLED(CONFIG_NEXUS_ANTI_IDLE_STATUS)
	else if (as_zmk_anti_idle_state(eh)) {
		refresh_anti_idle(as_zmk_anti_idle_state(eh)->active);
	}
#endif
	else if (as_zmk_battery_state_changed(eh)) {
		nexus_status_mut()->battery_dongle =
			as_zmk_battery_state_changed(eh)->state_of_charge;
		nexus_status_mark(NEXUS_STATUS_BATTERY);
	}
#if IS_ENABLED(CONFIG_NEXUS_SOUND_SPLIT)
	else if (as_zmk_activity_state_changed(eh)) {
		/*
		 * ACTIVE <-> SLEEP only. IDLE is skipped on purpose: with the
		 * stock CONFIG_ZMK_IDLE_TIMEOUT the dongle drops to IDLE after
		 * 30 s of not typing, so chirping on it would mean a noise
		 * every half minute you look away.
		 */
		static enum zmk_activity_state prev = ZMK_ACTIVITY_ACTIVE;
		enum zmk_activity_state now =
			as_zmk_activity_state_changed(eh)->state;

		if (now == ZMK_ACTIVITY_SLEEP && prev != ZMK_ACTIVITY_SLEEP) {
			nexus_sound_play(NEXUS_SOUND_SLEEP);
		} else if (now == ZMK_ACTIVITY_ACTIVE &&
			   prev == ZMK_ACTIVITY_SLEEP) {
			nexus_sound_play(NEXUS_SOUND_WAKE);
		}
		prev = now;
	}
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
	else if (as_zmk_peripheral_battery_state_changed(eh)) {
		const struct zmk_peripheral_battery_state_changed *p =
			as_zmk_peripheral_battery_state_changed(eh);

		nexus_status_peripheral_battery(p->source, p->state_of_charge);
	}
#endif

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nexus_status, nexus_status_listener);
ZMK_SUBSCRIPTION(nexus_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(nexus_status, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(nexus_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(nexus_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_NEXUS_SOUND_SPLIT)
ZMK_SUBSCRIPTION(nexus_status, zmk_activity_state_changed);
#endif
#if IS_ENABLED(CONFIG_NEXUS_ANTI_IDLE_STATUS)
ZMK_SUBSCRIPTION(nexus_status, zmk_anti_idle_state);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(nexus_status, zmk_peripheral_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(nexus_status, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(nexus_status, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_WPM)
ZMK_SUBSCRIPTION(nexus_status, zmk_wpm_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(nexus_status, zmk_hid_indicators_changed);
#endif

/** Seed the model so the first paint shows reality rather than zeroes. */
void nexus_status_seed(void)
{
	refresh_layer();
	refresh_endpoint();
	refresh_modifiers();
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
	refresh_locks();
#endif
	nexus_status_mark(NEXUS_STATUS_ALL);
}
