/*
 * Aggregated keyboard status model (Section 92).
 *
 * ZMK events land in src/status/zmk_events.c, which is the ONLY file in NEXUS
 * that includes a zmk/events/ header. Widgets read this struct and subscribe
 * for change notifications; they never query ZMK internals themselves.
 */
#ifndef NEXUS_STATUS_H_
#define NEXUS_STATUS_H_

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Battery / peripheral level is unknown - render "--", never "0" (Sec. 26). */
#define NEXUS_BATTERY_UNKNOWN 0xFF

enum nexus_link_state {
	NEXUS_LINK_DISCONNECTED = 0,
	NEXUS_LINK_CONNECTING,
	NEXUS_LINK_CONNECTED,
	NEXUS_LINK_RECONNECTING,
};

enum nexus_endpoint {
	NEXUS_ENDPOINT_NONE = 0,
	NEXUS_ENDPOINT_USB,
	NEXUS_ENDPOINT_BLE,
};

enum nexus_battery_band {
	NEXUS_BATT_UNKNOWN = 0,
	NEXUS_BATT_CRITICAL,   /*  0-10  */
	NEXUS_BATT_LOW,        /* 11-25  */
	NEXUS_BATT_MEDIUM,     /* 26-50  */
	NEXUS_BATT_GOOD,       /* 51-75  */
	NEXUS_BATT_FULL,       /* 76-100 */
};

/* Modifier bitmask, matching the HID modifier byte layout. */
#define NEXUS_MOD_CTRL  BIT(0)
#define NEXUS_MOD_SHIFT BIT(1)
#define NEXUS_MOD_ALT   BIT(2)
#define NEXUS_MOD_GUI   BIT(3)

/* Which fields changed, so a widget can repaint itself instead of the screen. */
#define NEXUS_STATUS_LAYER    BIT(0)
#define NEXUS_STATUS_WPM      BIT(1)
#define NEXUS_STATUS_BATTERY  BIT(2)
#define NEXUS_STATUS_ENDPOINT BIT(3)
#define NEXUS_STATUS_MODS     BIT(4)
#define NEXUS_STATUS_LOCKS    BIT(5)
#define NEXUS_STATUS_LINKS    BIT(6)
#define NEXUS_STATUS_JIGGLE   BIT(7)
#define NEXUS_STATUS_ALL      0xFF

struct nexus_status {
	const char *layer_name;
	uint8_t layer_index;

	uint8_t wpm;          /* smoothed for display (Section 27) */
	uint8_t wpm_raw;

	uint8_t battery_left;    /* 0-100 or NEXUS_BATTERY_UNKNOWN */
	uint8_t battery_right;
	uint8_t battery_dongle;

	enum nexus_link_state link_left;
	enum nexus_link_state link_right;
	enum nexus_link_state link_host;

	enum nexus_endpoint endpoint;
	uint8_t bt_profile;      /* 0-based active BLE profile */
	bool bt_profile_bonded;
	/* Whether that profile is actually connected right now. Tracked apart
	 * from link_host because the two answer different questions: you can
	 * be typing over USB while a BLE profile sits bonded and connected
	 * behind it, and the dashboard should say so rather than reporting
	 * BLE as broken whenever USB happens to be selected. */
	bool bt_connected;

	/* USB HID readiness, independent of which endpoint is selected - the
	 * dashboard reports each transport's own health, so "USB is plugged
	 * in but I am typing over BLE" is a state it can show. */
	bool usb_present;

	uint8_t modifiers;       /* NEXUS_MOD_* */
	bool caps_lock;
	bool num_lock;
	bool scroll_lock;

	/* Mouse jiggler, when CONFIG_NEXUS_ANTI_IDLE_STATUS is built. Always
	 * present in the struct so widgets need no #ifdef; simply never true
	 * without the module that reports it. */
	bool anti_idle;
};

/** Live model. Read-only for everything outside src/status/. */
const struct nexus_status *nexus_status_get(void);

typedef void (*nexus_status_cb_t)(const struct nexus_status *st, uint32_t changed);

/**
 * Subscribe to model changes. Observers are held in a static array sized by
 * the screens that exist, so there is no allocation and no unbounded list.
 * Callbacks run on ZMK's display work queue.
 */
int nexus_status_subscribe(nexus_status_cb_t cb);
void nexus_status_unsubscribe(nexus_status_cb_t cb);

enum nexus_battery_band nexus_battery_band(uint8_t percent);

/** Render a level as "57" or "--"; buf must hold at least 4 bytes. */
const char *nexus_battery_text(uint8_t percent, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_STATUS_H_ */
