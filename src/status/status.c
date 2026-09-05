/*
 * Aggregated status model + change fan-out (Sections 92-93).
 *
 * Writers are ZMK event listeners running on the event-manager context;
 * readers are LVGL widgets on the display work queue. The hand-off is a single
 * queued work item carrying a bitmask, so widgets never race the model and no
 * widget ever polls.
 */

#include <nexus/sound.h>
#include <nexus/status.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* Screens subscribe on enter and unsubscribe on exit, so this only has to
 * cover the deepest screen stack plus the sound observer. */
#define NEXUS_STATUS_MAX_OBS 6

static struct nexus_status g_status = {
	.layer_name = "DEFAULT",
	.battery_left = NEXUS_BATTERY_UNKNOWN,
	.battery_right = NEXUS_BATTERY_UNKNOWN,
	.battery_dongle = NEXUS_BATTERY_UNKNOWN,
};

static nexus_status_cb_t g_obs[NEXUS_STATUS_MAX_OBS];
static atomic_t g_pending;

/* Last time each peripheral said anything, for the staleness sweep. */
static int64_t g_seen[2];

static void notify_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	uint32_t changed = (uint32_t)atomic_clear(&g_pending);

	if (changed == 0) {
		return;
	}

	for (int i = 0; i < NEXUS_STATUS_MAX_OBS; i++) {
		if (g_obs[i]) {
			g_obs[i](&g_status, changed);
		}
	}
}
static K_WORK_DEFINE(g_notify_work, notify_work_cb);

/*
 * A peripheral that has gone quiet is not a peripheral at 0%. Demote it to
 * DISCONNECTED / unknown rather than leaving a stale "57%" on screen forever.
 */
static void stale_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_stale_work, stale_work_cb);

/* ZMK numbers peripherals by pairing order, not by physical side. Every
 * mapping in NEXUS goes through here so the battery card, the link state and
 * the connect chirp can never disagree about which half is which. */
static bool slot_is_left(int slot)
{
	return (slot == 0) != IS_ENABLED(CONFIG_NEXUS_SPLIT_SWAP_SIDES);
}

/*
 * Every link-state change for a half goes through here, which is the only
 * reason the connect/disconnect chirps stay in sync with the dashboard: one
 * place decides a half arrived or went away, so the sound and the pixels can
 * never disagree.
 *
 * `slot` is ZMK's peripheral index; `left` is which side of the screen that
 * turned out to be after CONFIG_NEXUS_SPLIT_SWAP_SIDES.
 */
static uint32_t set_link(int slot, enum nexus_link_state to)
{
	bool left = slot_is_left(slot);
	enum nexus_link_state *link =
		left ? &g_status.link_left : &g_status.link_right;

	if (*link == to) {
		return 0;
	}

	bool was_up = (*link == NEXUS_LINK_CONNECTED);
	bool now_up = (to == NEXUS_LINK_CONNECTED);

	*link = to;

	if (IS_ENABLED(CONFIG_NEXUS_SOUND_SPLIT) && was_up != now_up &&
	    k_uptime_get() > CONFIG_NEXUS_SOUND_SPLIT_SETTLE_MS) {
		nexus_sound_play(now_up ? (left ? NEXUS_SOUND_HALF_L_CONNECT
					        : NEXUS_SOUND_HALF_R_CONNECT)
					: (left ? NEXUS_SOUND_HALF_L_DISCONNECT
					        : NEXUS_SOUND_HALF_R_DISCONNECT));
	}

	return NEXUS_STATUS_LINKS;
}

static void stale_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	const int64_t now = k_uptime_get();
	uint32_t changed = 0;

	for (int i = 0; i < 2; i++) {
		uint8_t *batt = slot_is_left(i) ? &g_status.battery_left
					       : &g_status.battery_right;

		if (g_seen[i] == 0) {
			continue;
		}

		/*
		 * Batteries only. Link state used to be inferred here from the
		 * same silence, which was a mistake: a half with
		 * CONFIG_ZMK_SLEEP stops reporting the moment it dozes, so
		 * that fired a "disconnected" chirp at a keyboard that was
		 * working perfectly - the random beeping. The link now comes
		 * from Zephyr connection callbacks (split_conn.c), which know
		 * the difference.
		 *
		 * This timeout stays off by default anyway: an idle half still
		 * HAS a charge level, we just heard it a while ago, and
		 * blanking a good reading is worse than showing a slightly old
		 * number (Section 26).
		 */
		if (CONFIG_NEXUS_STATUS_STALE_MS > 0 &&
		    now - g_seen[i] > CONFIG_NEXUS_STATUS_STALE_MS &&
		    *batt != NEXUS_BATTERY_UNKNOWN) {
			*batt = NEXUS_BATTERY_UNKNOWN;
			changed |= NEXUS_STATUS_BATTERY;
		}
	}

	if (changed) {
		nexus_status_mark(changed);
	}

	k_work_reschedule_for_queue(nexus_workq(), &g_stale_work,
				    K_MSEC(CONFIG_NEXUS_STATUS_STALE_MS / 4));
}

struct nexus_status *nexus_status_mut(void)
{
	return &g_status;
}

const struct nexus_status *nexus_status_get(void)
{
	return &g_status;
}

void nexus_status_mark(uint32_t changed)
{
	atomic_or(&g_pending, (atomic_val_t)changed);
	k_work_submit_to_queue(nexus_workq(), &g_notify_work);
}

void nexus_status_peripheral_battery(uint8_t source, uint8_t level)
{
	if (source > 1) {
		/* ponytail: two halves is the only split topology NEXUS renders.
		 * Widen g_seen[]/the dashboard grid if a 3+ piece board shows up. */
		return;
	}

	bool left = slot_is_left(source);

	g_seen[source] = k_uptime_get();

	if (left) {
		g_status.battery_left = level;
	} else {
		g_status.battery_right = level;
	}

	nexus_status_mark(NEXUS_STATUS_BATTERY |
			  set_link(source, NEXUS_LINK_CONNECTED));
}

void nexus_status_half_link(int slot, bool connected)
{
	if (slot < 0 || slot > 1) {
		return;
	}

	uint32_t changed = set_link(slot, connected ? NEXUS_LINK_CONNECTED
						    : NEXUS_LINK_DISCONNECTED);

	if (!connected) {
		/*
		 * Deliberately keeps the last known charge. A half that just
		 * went away still had a battery level a moment ago, and "--"
		 * is for a level we never knew (Section 26). g_seen is left
		 * alone too, so reconnecting does not look like a fresh boot.
		 */
		changed |= NEXUS_STATUS_LINKS;
	}

	if (changed) {
		nexus_status_mark(changed);
	}
}

int nexus_status_subscribe(nexus_status_cb_t cb)
{
	for (int i = 0; i < NEXUS_STATUS_MAX_OBS; i++) {
		if (g_obs[i] == cb) {
			return 0;
		}
	}
	for (int i = 0; i < NEXUS_STATUS_MAX_OBS; i++) {
		if (g_obs[i] == NULL) {
			g_obs[i] = cb;
			/* Prime the new observer with the world as it stands. */
			cb(&g_status, NEXUS_STATUS_ALL);
			return 0;
		}
	}
	LOG_WRN("status observer table full, widget will not update");
	return -ENOSPC;
}

void nexus_status_unsubscribe(nexus_status_cb_t cb)
{
	for (int i = 0; i < NEXUS_STATUS_MAX_OBS; i++) {
		if (g_obs[i] == cb) {
			g_obs[i] = NULL;
			return;
		}
	}
}

enum nexus_battery_band nexus_battery_band(uint8_t percent)
{
	if (percent == NEXUS_BATTERY_UNKNOWN) {
		return NEXUS_BATT_UNKNOWN;
	}
	if (percent <= 10) {
		return NEXUS_BATT_CRITICAL;
	}
	if (percent <= 25) {
		return NEXUS_BATT_LOW;
	}
	if (percent <= 50) {
		return NEXUS_BATT_MEDIUM;
	}
	if (percent <= 75) {
		return NEXUS_BATT_GOOD;
	}
	return NEXUS_BATT_FULL;
}

/*
 * Hand-rolled rather than snprintf: readers of this run on the display work
 * queue, and picolibc's "%u" pulls in the double-capable formatter, which can
 * want 1-2 KB of stack per call. Overflowing that thread panics the kernel and
 * takes the keyboard with it - a battery label is not worth that risk, and the
 * whole job is four digits.
 */
const char *nexus_battery_text(uint8_t percent, char *buf, size_t len)
{
	if (len < 4) {
		if (len) {
			buf[0] = '\0';
		}
		return buf;
	}

	if (percent == NEXUS_BATTERY_UNKNOWN || percent > 100) {
		/* "--", never "0": an unknown half and a flat half are very
		 * different facts (Section 26). */
		buf[0] = '-';
		buf[1] = '-';
		buf[2] = '\0';
		return buf;
	}

	int i = 0;

	if (percent >= 100) {
		buf[i++] = '1';
		buf[i++] = '0';
		buf[i++] = '0';
	} else {
		if (percent >= 10) {
			buf[i++] = (char)('0' + percent / 10U);
		}
		buf[i++] = (char)('0' + percent % 10U);
	}
	buf[i] = '\0';
	return buf;
}

void nexus_status_init(void)
{
	/*
	 * A zero timeout means "keep the last level we heard", which is the
	 * default: halves with deep sleep stop reporting when idle, and
	 * blanking a good reading because the keyboard was resting is worse
	 * than showing a slightly old number. Not arming the sweep at all is
	 * also cheaper than arming it and returning early forever.
	 */
	if (CONFIG_NEXUS_STATUS_STALE_MS > 0) {
		k_work_reschedule_for_queue(nexus_workq(), &g_stale_work,
					    K_MSEC(CONFIG_NEXUS_STATUS_STALE_MS / 4));
	}
}
