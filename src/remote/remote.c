/*
 * Remote Input: the phone's side of the radio (docs/remote-input.md).
 *
 * The phone is a ZMK Bluetooth profile, and nothing more unusual than that
 * on the radio. It finds NEXUS through ZMK's own advertising, pairs into a
 * free profile slot, and reconnects the way any bonded host does - so ZMK's
 * Bluetooth runs exactly as it does without Remote Input, which is the one
 * thing every earlier design here broke:
 *
 *   - a second advertiser needs BT_EXT_ADV, which hangs this dongle in
 *     Bluetooth controller start-up;
 *   - a second identity cannot advertise on the one legacy advertiser while
 *     the dongle scans for and connects to its halves - they share a single
 *     random address - so nothing advertised at all.
 *
 * What keeps a phone in a profile from being a keyboard host:
 *
 *   Authorization. A custom GATT authorization callback refuses every read
 *   and write in ZMK's HID service on a phone's link, so the phone's OS can
 *   neither load the report map - and adopt NEXUS as a keyboard, hiding its
 *   own on-screen one - nor subscribe to a single report. Reports only ever
 *   go to the active profile anyway, and a phone's profile is only active
 *   for the seconds it takes to pair. The same callback keeps everyone but
 *   the phone out of the Remote Input service.
 *
 *   Pairing. Settings > PHONE opens a 60 s window and switches ZMK to a free
 *   profile, so its own advertising lets a phone in. Per-connection auth
 *   callbacks (bt_conn_auth_cb_overlay) give that one connection a passkey
 *   on the NEXUS screen; ZMK records the phone in that profile, NEXUS
 *   records its address as a phone, and the previous profile comes back.
 *
 *   Coming back. ZMK falls silent while its active profile's host is
 *   connected; in that gap NEXUS advertises for a paired phone on the same
 *   legacy advertiser, and hands it straight back when ZMK needs it.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include <dt-bindings/nexus.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>

#include <nexus/remote.h>
#include <nexus/sound.h>
#include <nexus/status.h>

#include "../nexus_priv.h"
#include "remote_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* Kconfig cannot say this without a dependency loop (see the Kconfig). */
BUILD_ASSERT(IS_ENABLED(CONFIG_SETTINGS),
	     "Remote Input keeps its paired phones in settings: "
	     "enable CONFIG_SETTINGS");

/* The phone's action range is the game layer, and nothing past it. */
BUILD_ASSERT(REMOTE_ACTION_FIRST == NEXUS_ACT_SELECT &&
		     REMOTE_ACTION_LAST == NEXUS_ACT_HOST &&
		     NEXUS_ACT_REMOTE_TOGGLE > REMOTE_ACTION_LAST,
	     "Remote Input's action range no longer matches dt-bindings/nexus.h");
BUILD_ASSERT(REMOTE_OUTPUT_USB == ZMK_TRANSPORT_USB &&
		     REMOTE_OUTPUT_BLE == ZMK_TRANSPORT_BLE,
	     "Remote Input's output numbers no longer match ZMK's");

#define STATUS_LEN 7

#define PHONES_MAX 2
#define PAIR_WINDOW K_SECONDS(60)

/* ZMK's pairing_complete must have filed the phone into the pairing profile
 * before that profile is switched away from. Both run on the Bluetooth
 * thread; this is ample. */
#define PAIR_SETTLE K_MSEC(500)

/* The app unsubscribed: drop the phone's link this long after, unless it
 * subscribes again - a page reload is an unsubscribe and a subscribe. */
#define APP_GONE_GRACE K_SECONDS(2)

/* 15 ms, no latency, 2 s timeout: the fastest link iOS allows, and a dead
 * phone noticed in two seconds rather than the dongle-wide eight. */
#define PHONE_CONN_PARAM BT_LE_CONN_PARAM(12, 12, 0, 200)

#define REMOTE_UUID(n)                                                         \
	BT_UUID_128_ENCODE(0x7e4e0000 + (n), 0x5c1a, 0x4b2e, 0x9d3f,           \
			   0x8a6b4c2d1e0fULL)

static const struct bt_uuid_128 k_svc = BT_UUID_INIT_128(REMOTE_UUID(1));
static const struct bt_uuid_128 k_mouse = BT_UUID_INIT_128(REMOTE_UUID(2));
static const struct bt_uuid_128 k_key = BT_UUID_INIT_128(REMOTE_UUID(3));
static const struct bt_uuid_128 k_text = BT_UUID_INIT_128(REMOTE_UUID(4));
static const struct bt_uuid_128 k_ctrl = BT_UUID_INIT_128(REMOTE_UUID(5));
static const struct bt_uuid_128 k_status = BT_UUID_INIT_128(REMOTE_UUID(6));

static bool g_on = true; /* remote mode, persisted */
static uint16_t g_hids_start, g_hids_end;

static struct k_spinlock g_lock;
static struct bt_conn *g_phone;     /* the phone being served; g_lock */
static struct bt_conn *g_pair_conn; /* the connection pairing now; g_lock */

/* Paired phones, persisted. Read on the Bluetooth thread for every ATT op,
 * written there (pairing) and on the work queue (forgetting); a torn read
 * can only misjudge a phone for one request. */
static struct {
	uint8_t n;
	bt_addr_le_t addr[PHONES_MAX];
} g_phones;

static int g_pair_profile = -1; /* work queue only */
static bool g_just_paired;      /* the window is closing on a new pairing */
static int g_prev_profile;      /* work queue only */
static uint8_t g_last_status[STATUS_LEN];
static atomic_t g_output; /* REMOTE_OUTPUT_* asked for, until applied */

static void status_fn(struct k_work *work);
static void pair_open_fn(struct k_work *work);
static void pair_close_fn(struct k_work *work);
static void clear_fn(struct k_work *work);
static void output_fn(struct k_work *work);
static void drop_fn(struct k_work *work);
static void phones_save_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_status_work, status_fn);
static K_WORK_DEFINE(g_pair_open_work, pair_open_fn);
static K_WORK_DELAYABLE_DEFINE(g_pair_close, pair_close_fn);
static K_WORK_DEFINE(g_clear_work, clear_fn);
static K_WORK_DEFINE(g_output_work, output_fn);
static K_WORK_DELAYABLE_DEFINE(g_drop_work, drop_fn);
static K_WORK_DEFINE(g_phones_save, phones_save_fn);

/* A reference to the phone being served, or NULL. Caller unrefs. */
static struct bt_conn *phone_get(void)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&g_lock) {
		if (g_phone) {
			conn = bt_conn_ref(g_phone);
		}
	}
	return conn;
}

static bool is_peripheral_link(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return bt_conn_get_info(conn, &info) == 0 &&
	       info.role == BT_CONN_ROLE_PERIPHERAL;
}

static bool known_phone(const bt_addr_le_t *addr)
{
	for (int i = 0; i < g_phones.n && i < PHONES_MAX; i++) {
		if (!bt_addr_le_cmp(&g_phones.addr[i], addr)) {
			return true;
		}
	}
	return false;
}

/* A phone's link: a paired phone, or the one pairing right now. Kept out
 * of ZMK's HID service. */
static bool is_phone(struct bt_conn *conn)
{
	return conn == g_pair_conn ||
	       (is_peripheral_link(conn) && known_phone(bt_conn_get_dst(conn)));
}

/* Allowed into the Remote Input service: the phone being served, or the
 * one pairing. One phone at a time. */
static bool serving(struct bt_conn *conn)
{
	return conn == g_phone || conn == g_pair_conn;
}

/* The window is open exactly while its closing work is pending. */
static bool pair_open(void)
{
	return k_work_delayable_is_pending(&g_pair_close);
}

uint32_t nexus_remote_pair_remaining_s(void)
{
	k_ticks_t t = k_work_delayable_remaining_get(&g_pair_close);

	return (k_ticks_to_ms_floor32(t) + 999) / 1000;
}

/* On the system work queue, where ZMK saves its own profiles: a flash write
 * from a Bluetooth callback would stall the radio's host thread - halves
 * included - for as long as the erase takes. */
static void phones_save_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	settings_save_one("nexus/remote/phones", &g_phones, sizeof(g_phones));
}

static void phones_save(void)
{
	k_work_submit(&g_phones_save);
}

/* ---- GATT ---------------------------------------------------------------- */

static void status_bytes(uint8_t v[STATUS_LEN])
{
	const struct nexus_status *st = nexus_status_get();
	uint16_t free = remote_hid_text_free();

	v[0] = REMOTE_PROTO_VERSION;
	v[1] = (g_on ? BIT(0) : 0) | (st->usb_present ? BIT(1) : 0) |
	       (remote_hid_typing() ? BIT(2) : 0) | (pair_open() ? BIT(3) : 0);
	v[2] = (st->num_lock ? BIT(0) : 0) | (st->caps_lock ? BIT(1) : 0) |
	       (st->scroll_lock ? BIT(2) : 0);
	v[3] = (uint8_t)free;
	v[4] = (uint8_t)(free >> 8);
	/* text, consumer, hwheel, dongle controls, the HOST screen */
	v[5] = BIT(0) | BIT(1) | BIT(2) | BIT(3) |
	       (IS_ENABLED(CONFIG_NEXUS_HOST_LINK) ? BIT(4) : 0);
	/* What &out would show: the preferred transport, not the fallback. */
	v[6] = (uint8_t)zmk_endpoint_get_preferred_transport();
}

/* ZMK's endpoint calls save settings: the system work queue, as &out. */
static void output_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	zmk_endpoint_set_preferred_transport(
		(enum zmk_transport)atomic_get(&g_output));
	/* A preference ZMK cannot act on yet - BLE with no host connected -
	 * changes no endpoint and so raises nothing; tell the phone anyway. */
	remote_status_kick();
}

/* Common to every write: whole values only, and nothing while off. */
static ssize_t gate(uint16_t offset)
{
	if (offset) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (!g_on) {
		return BT_GATT_ERR(REMOTE_ERR_OFF);
	}
	return 0;
}

static ssize_t write_mouse(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset,
			   uint8_t flags)
{
	struct remote_mouse m;
	ssize_t err = gate(offset);
	int perr;

	if (err) {
		return err;
	}
	perr = remote_mouse_parse(buf, len, &m);
	if (perr) {
		return BT_GATT_ERR(perr);
	}
	remote_hid_mouse(&m);
	return len;
}

static ssize_t write_key(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	struct remote_key k;
	ssize_t err = gate(offset);
	int perr;

	if (err) {
		return err;
	}
	perr = remote_key_parse(buf, len, &k);
	if (!perr) {
		perr = remote_hid_key(&k);
	}
	return perr ? BT_GATT_ERR(perr) : len;
}

static ssize_t write_text(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	ssize_t err = gate(offset);
	int perr;

	if (err) {
		return err;
	}
	perr = remote_text_check(buf, len);
	if (!perr) {
		perr = remote_hid_text(buf, len);
	}
	if (!perr) {
		remote_status_kick();
	}
	return perr ? BT_GATT_ERR(perr) : len;
}

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	const uint8_t *b = buf;
	ssize_t err = gate(offset);

	if (err) {
		return err;
	}
	if (len < 1) {
		return BT_GATT_ERR(REMOTE_ERR_LEN);
	}

	switch (b[0]) {
	case REMOTE_CTRL_RELEASE_ALL:
		remote_hid_release_all(false);
		break;
	case REMOTE_CTRL_KEEPALIVE:
		remote_hid_keepalive();
		break;
	case REMOTE_CTRL_CANCEL_TEXT:
		remote_hid_release_all(true);
		break;
	case REMOTE_CTRL_TYPE_DELAY:
		if (len != 2) {
			return BT_GATT_ERR(REMOTE_ERR_LEN);
		}
		if (b[1] < REMOTE_TYPE_DELAY_MIN || b[1] > REMOTE_TYPE_DELAY_MAX) {
			return BT_GATT_ERR(REMOTE_ERR_VALUE);
		}
		remote_hid_set_delay(b[1]);
		break;
	case REMOTE_CTRL_IDENTIFY:
		nexus_sound_play(NEXUS_SOUND_SELECT);
		nexus_remote_screen_show(NEXUS_REMOTE_VIEW_HELLO, 0);
		break;
	case REMOTE_CTRL_ACTION: {
		int perr = remote_action_check(b, len);

		if (perr) {
			return BT_GATT_ERR(perr);
		}
		remote_hid_action(b[1], b[2]);
		break;
	}
	case REMOTE_CTRL_OUTPUT:
		if (len != 2) {
			return BT_GATT_ERR(REMOTE_ERR_LEN);
		}
		if (b[1] != REMOTE_OUTPUT_USB && b[1] != REMOTE_OUTPUT_BLE) {
			return BT_GATT_ERR(REMOTE_ERR_VALUE);
		}
		atomic_set(&g_output, b[1]);
		k_work_submit(&g_output_work);
		break;
	default:
		return BT_GATT_ERR(REMOTE_ERR_VALUE);
	}
	return len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t v[STATUS_LEN];

	status_bytes(v);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, v, sizeof(v));
}

/*
 * The app arrived: a paired phone that came back through the window - with a
 * BLE host on the active profile, ZMK advertises only then - pairs nothing,
 * so nothing else would close the window, and ZMK would sit on the empty
 * profile, away from that host, for the rest of the minute. A phone that has
 * just paired is already closing it, after its settle time.
 */
static void app_arrived(void)
{
	if (pair_open() && !g_pair_conn && !g_just_paired) {
		nexus_remote_screen_show(NEXUS_REMOTE_VIEW_CONNECTED, 0);
		nexus_sound_play(NEXUS_SOUND_CONNECT);
		k_work_reschedule(&g_pair_close, K_NO_WAIT);
	}
}

/*
 * Status subscriptions, per connection, are the app coming and going. iOS
 * keeps a bonded link up after the app is closed - NEXUS listed as connected
 * in the phone's settings, the phone glyph lit - and while it does, the
 * phone's scan never offers NEXUS to the app again. So when the app leaves,
 * NEXUS drops the link itself. Only the phone being served.
 */
static ssize_t status_ccc_write(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	if (conn == g_phone) {
		if (value) {
			k_work_cancel_delayable(&g_drop_work);
			app_arrived();
		} else {
			k_work_reschedule(&g_drop_work, APP_GONE_GRACE);
		}
	}
	return sizeof(value);
}

/* _bt_gatt_ccc is Zephyr 4.1's name; later trees call it
 * bt_gatt_ccc_managed_user_data. */
static struct _bt_gatt_ccc g_status_ccc =
	BT_GATT_CCC_INITIALIZER(NULL, status_ccc_write, NULL);

/*
 * LESC permissions: an encrypted, MITM-protected link - which on this
 * service means a passkey read off the NEXUS screen. A phone that is merely
 * encrypted (Just Works) gets an authentication error, and pairs properly.
 */
BT_GATT_SERVICE_DEFINE(nexus_remote_svc,
	BT_GATT_PRIMARY_SERVICE(&k_svc),
	BT_GATT_CHARACTERISTIC(&k_mouse.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_LESC, NULL, write_mouse, NULL),
	BT_GATT_CHARACTERISTIC(&k_key.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE_LESC, NULL, write_key, NULL),
	BT_GATT_CHARACTERISTIC(&k_text.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE_LESC, NULL, write_text, NULL),
	BT_GATT_CHARACTERISTIC(&k_ctrl.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE_LESC, NULL, write_ctrl, NULL),
	BT_GATT_CHARACTERISTIC(&k_status.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_LESC, read_status, NULL, NULL),
	BT_GATT_CCC_MANAGED(&g_status_ccc,
			    BT_GATT_PERM_READ_LESC | BT_GATT_PERM_WRITE_LESC),
);

/* Primary service, then two attributes per characteristic: Status is 10. */
#define STATUS_ATTR (&nexus_remote_svc.attrs[10])

static void drop_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = phone_get();

	if (!conn) {
		return;
	}
	if (!bt_gatt_is_subscribed(conn, STATUS_ATTR, BT_GATT_CCC_NOTIFY)) {
		LOG_INF("remote: app gone, dropping the phone's link");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	bt_conn_unref(conn);
}

static bool in_service(const struct bt_gatt_attr *attr)
{
	return attr >= nexus_remote_svc.attrs &&
	       attr < nexus_remote_svc.attrs + nexus_remote_svc.attr_count;
}

/*
 * Service, include and characteristic declarations are the map, not the
 * contents. Refusing them breaks discovery itself - a phone that cannot list
 * the HID service's characteristics fails the whole connection, and a host
 * that cannot list ours may too - while refusing only the values keeps every
 * report, the report map and every CCC just as unreachable.
 */
static bool is_declaration(const struct bt_gatt_attr *attr)
{
	return !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) ||
	       !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_SECONDARY) ||
	       !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_INCLUDE) ||
	       !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CHRC);
}

static bool authorize(struct bt_conn *conn, const struct bt_gatt_attr *attr)
{
	if (is_declaration(attr)) {
		return true;
	}
	if (in_service(attr)) {
		return serving(conn);
	}
	if (!g_hids_start || !is_phone(conn)) {
		return true;
	}

	/* The handle walk is only paid on a phone's link, and a phone has no
	 * business outside this service after discovery. */
	uint16_t h = bt_gatt_attr_get_handle(attr);

	return h < g_hids_start || h > g_hids_end;
}

static const struct bt_gatt_authorization_cb k_authz = {
	.read_authorize = authorize,
	.write_authorize = authorize,
};

/* Find ZMK's HID service's handle range once; the database is static. */
static uint8_t find_hids(const struct bt_gatt_attr *attr, uint16_t handle,
			 void *user_data)
{
	ARG_UNUSED(user_data);

	if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) &&
	    bt_uuid_cmp(attr->uuid, BT_UUID_GATT_SECONDARY)) {
		return BT_GATT_ITER_CONTINUE;
	}
	if (g_hids_start) {
		g_hids_end = handle - 1;
		return BT_GATT_ITER_STOP;
	}
	if (!bt_uuid_cmp(attr->user_data, BT_UUID_HIDS)) {
		g_hids_start = handle;
	}
	return BT_GATT_ITER_CONTINUE;
}

/* ---- advertising for a paired phone -------------------------------------- */

/*
 * A paired phone comes back only through advertising, and ZMK advertises
 * only while its active profile is open or that profile's host is away. In
 * the gap - a host connected on the active profile, a phone paired, none
 * connected - NEXUS advertises instead, under the same name, so the app finds
 * it again without a trip to Settings > PHONE.
 *
 * The advertiser is ZMK's the rest of the time. NEXUS lets go before its own
 * profile switches, and on any change re-checks after ADV_SETTLE - ZMK
 * updates its advertising first, on disconnect from its own work item. If
 * ZMK was refused the advertiser while NEXUS held it, NEXUS stops and asks
 * ZMK to look again: zmk_ble_set_device_name() with the name it already has
 * writes nothing (Zephyr returns early on the same name) and runs ZMK's own
 * update_advertising().
 *
 * Nothing new pairs through it: outside the window ZMK refuses pairing to a
 * profile that is not open, and NEXUS overlays nothing.
 */
/* The hand-back renames to the same name, which only a dynamic name can. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_NEXUS_REMOTE_INPUT_PHONE_ADV) ||
		     IS_ENABLED(CONFIG_BT_DEVICE_NAME_DYNAMIC),
	     "NEXUS_REMOTE_INPUT_PHONE_ADV needs CONFIG_BT_DEVICE_NAME_DYNAMIC, "
	     "which ZMK turns on");

#define ADV_SETTLE   K_MSEC(200)
#define ADV_NAME_MAX 26 /* 31 bytes of advertising data, less flags and header */

static atomic_t g_adv_ours; /* set: NEXUS is advertising, not ZMK */
static char g_adv_name[ADV_NAME_MAX];
static struct bt_data g_adv_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, g_adv_name, 0),
};
static const struct bt_data k_adv_sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, REMOTE_UUID(1)),
};

static bool phone_waiting(void)
{
	return IS_ENABLED(CONFIG_NEXUS_REMOTE_INPUT_PHONE_ADV) &&
	       g_phones.n > 0 && !g_phone && !g_pair_conn && !pair_open() &&
	       !zmk_ble_active_profile_is_open() &&
	       zmk_ble_active_profile_is_connected();
}

/* Stop advertising for phones. @return whether NEXUS was. Work queue. */
static bool adv_release(void)
{
	if (!atomic_cas(&g_adv_ours, 1, 0)) {
		return false;
	}
	bt_le_adv_stop();
	return true;
}

/* ZMK's own update_advertising(), through its public API. */
static void adv_hand_back(const char *name)
{
	/* ZMK names are 16 characters at most; a longer one is left alone
	 * rather than renamed by truncation. */
	char again[33];

	if (strlen(name) < sizeof(again)) {
		strcpy(again, name);
		zmk_ble_set_device_name(again);
	}
}

static void adv_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	const char *name = bt_get_name();
	size_t full = strlen(name);
	size_t len = MIN(full, sizeof(g_adv_name));

	if (!phone_waiting()) {
		if (adv_release()) {
			adv_hand_back(name);
		}
		return;
	}
	if (atomic_get(&g_adv_ours)) {
		return;
	}

	memcpy(g_adv_name, name, len);
	g_adv_ad[1].type = len < full ? BT_DATA_NAME_SHORTENED
				      : BT_DATA_NAME_COMPLETE;
	g_adv_ad[1].data_len = (uint8_t)len;

	int err = bt_le_adv_start(
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
				BT_GAP_ADV_FAST_INT_MAX_2, NULL),
		g_adv_ad, ARRAY_SIZE(g_adv_ad), k_adv_sd, ARRAY_SIZE(k_adv_sd));

	if (err == 0) {
		atomic_set(&g_adv_ours, 1);
		LOG_INF("remote: advertising for a paired phone");
	} else if (err != -EALREADY) {
		/* -EALREADY is ZMK advertising after all: nothing to do. */
		LOG_WRN("remote: phone advertising failed (%d)", err);
	}
}
static K_WORK_DELAYABLE_DEFINE(g_adv_work, adv_fn);

static void adv_check(void)
{
	if (IS_ENABLED(CONFIG_NEXUS_REMOTE_INPUT_PHONE_ADV)) {
		k_work_reschedule(&g_adv_work, ADV_SETTLE);
	}
}

/* A peripheral connection, through either advertiser, ends advertising. */
static void adv_consumed(void)
{
	atomic_set(&g_adv_ours, 0);
}

/* ---- status -------------------------------------------------------------- */

static void status_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = phone_get();
	uint8_t v[STATUS_LEN];

	nexus_status_remote((g_on ? NEXUS_REMOTE_ON : 0) |
			    (conn ? NEXUS_REMOTE_PHONE : 0) |
			    (remote_hid_typing() ? NEXUS_REMOTE_TYPING : 0));
	if (!conn) {
		return;
	}

	status_bytes(v);
	if (memcmp(v, g_last_status, sizeof(v)) != 0) {
		/* Fails harmlessly when the app has not subscribed. */
		if (bt_gatt_notify(conn, STATUS_ATTR, v, sizeof(v)) == 0) {
			memcpy(g_last_status, v, sizeof(v));
		}
	}
	bt_conn_unref(conn);
}

/* Coalesced: many kicks inside 100 ms are one notification, which is the
 * protocol's rate limit while text is typing. */
void remote_status_kick(void)
{
	k_work_schedule(&g_status_work, K_MSEC(100));
}

static void on_nexus_status(const struct nexus_status *st, uint32_t changed)
{
	ARG_UNUSED(st);

	if (changed & (NEXUS_STATUS_LOCKS | NEXUS_STATUS_ENDPOINT)) {
		remote_status_kick();
	}
	/* The active profile switched, or its host came or went. */
	if (changed & NEXUS_STATUS_ENDPOINT) {
		adv_check();
	}
}

/* ---- connections --------------------------------------------------------- */

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *feat);
static void passkey_display(struct bt_conn *conn, unsigned int passkey);
static void pairing_cancel(struct bt_conn *conn);
static void passkey_gone(void);

static const struct bt_conn_auth_cb k_auth = {
	.pairing_accept = pairing_accept,
	.passkey_display = passkey_display,
	.cancel = pairing_cancel,
};

static void serve(struct bt_conn *conn)
{
	bool busy = false;

	K_SPINLOCK(&g_lock) {
		if (g_phone) {
			busy = g_phone != conn;
		} else {
			g_phone = bt_conn_ref(conn);
		}
	}
	if (busy) {
		/* ponytail: one phone served at a time; a second paired phone
		 * stays connected but the service refuses it. */
		return;
	}

	bt_conn_le_param_update(conn, PHONE_CONN_PARAM);
	memset(g_last_status, 0xFF, sizeof(g_last_status));
	remote_hid_set_delay(CONFIG_NEXUS_REMOTE_INPUT_TYPE_DELAY_MS);
	remote_status_kick();
	LOG_INF("remote: phone connected");
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	bool candidate = false;

	if (!is_peripheral_link(conn)) {
		return;
	}
	/* Whoever it was, the advertising they connected through has ended. */
	adv_consumed();
	adv_check();
	if (err) {
		return;
	}
	if (known_phone(bt_conn_get_dst(conn))) {
		serve(conn);
		return;
	}
	if (!pair_open()) {
		return; /* a host, as far as NEXUS is concerned: ZMK's business */
	}

	/* The window is open, so whatever connects now is taken to be the
	 * phone that was asked for. A host that connects in the same minute
	 * gets the phone's treatment - documented, and the window is short. */
	K_SPINLOCK(&g_lock) {
		if (!g_pair_conn) {
			g_pair_conn = bt_conn_ref(conn);
			candidate = true;
		}
	}
	if (candidate) {
		/* Before anything can start SMP on this link. */
		bt_conn_auth_cb_overlay(conn, &k_auth);
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	bool phone = false;
	bool pairing = false;

	if (is_peripheral_link(conn)) {
		adv_check();
	}

	K_SPINLOCK(&g_lock) {
		if (g_phone == conn) {
			g_phone = NULL;
			phone = true;
		}
		if (g_pair_conn == conn) {
			g_pair_conn = NULL;
			pairing = true;
		}
	}
	if (pairing) {
		bt_conn_unref(conn);
		passkey_gone();
	}
	if (!phone) {
		return;
	}

	bt_conn_unref(conn);
	/* A drop pending for this phone must not land on the next one. */
	k_work_cancel_delayable(&g_drop_work);
	remote_hid_release_all(true);
	remote_status_kick();
	LOG_INF("remote: phone disconnected (0x%02x)", reason);
}

BT_CONN_CB_DEFINE(nexus_remote_conn) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* ---- pairing ------------------------------------------------------------- */

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *feat)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(feat);

	if (!pair_open()) {
		LOG_WRN("remote: pairing refused, window closed");
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	return BT_SECURITY_ERR_SUCCESS;
}

static void passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(conn);

	nexus_remote_screen_show(NEXUS_REMOTE_VIEW_PASSKEY, passkey);
}

/* The passkey is no longer wanted. With the window still open, back to its
 * countdown - a retry is one tap away - rather than to nothing. */
static void passkey_gone(void)
{
	if (pair_open()) {
		nexus_remote_screen_show(NEXUS_REMOTE_VIEW_WAIT, 0);
	} else {
		nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_PASSKEY);
	}
}

static void pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	passkey_gone();
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	bool ours = false;

	K_SPINLOCK(&g_lock) {
		if (conn == g_pair_conn) {
			g_pair_conn = NULL;
			ours = true;
		}
	}
	if (!ours) {
		return;
	}

	const bt_addr_le_t *addr = bt_conn_get_dst(conn);

	if (!known_phone(addr) && g_phones.n < PHONES_MAX) {
		bt_addr_le_copy(&g_phones.addr[g_phones.n], addr);
		g_phones.n++;
		phones_save();
	}
	LOG_INF("remote: phone paired%s", bonded ? " and bonded" : "");

	serve(conn);
	bt_conn_unref(conn); /* g_pair_conn's reference; serve() took its own */

	g_just_paired = true;
	nexus_remote_screen_show(NEXUS_REMOTE_VIEW_PAIRED, 0);
	nexus_sound_play(NEXUS_SOUND_CONNECT);

	/* Close the window - which switches back to the previous profile -
	 * once ZMK has filed the phone into the pairing one. */
	k_work_reschedule(&g_pair_close, PAIR_SETTLE);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	if (conn != g_pair_conn) {
		return;
	}

	/* The window stays open: a mistyped passkey can be tried again. */
	LOG_WRN("remote: pairing failed (%d)", reason);
	passkey_gone();
	nexus_sound_play(NEXUS_SOUND_BACK);
}

static struct bt_conn_auth_info_cb g_auth_info = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/*
 * Open the window: switch ZMK to a free profile, so ZMK's own advertising
 * and pairing rules let a phone in. The highest free slot, to leave the low
 * ones - the ones BT_SEL reaches first - for hosts.
 */
static void pair_open_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int free_slot = -1;

	if (pair_open()) {
		/* Already open: just give it its full minute again. */
		k_work_reschedule(&g_pair_close, PAIR_WINDOW);
		return;
	}
	if (g_phones.n < PHONES_MAX) {
		for (int i = ZMK_BLE_PROFILE_COUNT - 1; i >= 0; i--) {
			if (zmk_ble_profile_is_open(i)) {
				free_slot = i;
				break;
			}
		}
	}
	if (free_slot < 0) {
		LOG_WRN("remote: no room for a phone (%d paired)", g_phones.n);
		nexus_remote_screen_show(NEXUS_REMOTE_VIEW_FULL, 0);
		nexus_sound_play(NEXUS_SOUND_BACK);
		return;
	}

	g_prev_profile = zmk_ble_active_profile_index();
	g_pair_profile = free_slot;
	/* The advertiser back to ZMK first, or ZMK's switch cannot start it. */
	adv_release();
	if (free_slot != g_prev_profile) {
		zmk_ble_prof_select(free_slot);
	}

	k_work_reschedule(&g_pair_close, PAIR_WINDOW);
	nexus_remote_screen_show(NEXUS_REMOTE_VIEW_WAIT, 0);
	nexus_sound_play(NEXUS_SOUND_MENU_OPEN);
	remote_status_kick();
}

/* The window closes - paired, cancelled or timed out: back to the profile
 * that was active before it opened. */
static void pair_close_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	adv_release();
	if (g_pair_profile >= 0 &&
	    zmk_ble_active_profile_index() == g_pair_profile &&
	    g_prev_profile != g_pair_profile) {
		zmk_ble_prof_select(g_prev_profile);
	}
	g_pair_profile = -1;
	g_just_paired = false;
	adv_check();

	nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_WAIT);
	remote_status_kick();
}

void remote_pair_cancel(void)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&g_lock) {
		if (g_pair_conn) {
			conn = bt_conn_ref(g_pair_conn);
		}
	}
	if (conn) {
		bt_conn_auth_cancel(conn); /* harmless if nothing is pending */
		bt_conn_unref(conn);
	}
	/* Close now rather than when the minute runs out. */
	k_work_reschedule(&g_pair_close, K_NO_WAIT);
}

/*
 * Forget the phones: clear each one's ZMK profile - which also deletes its
 * bond and drops it if connected - and nothing else. ZMK clears only the
 * active profile, so step to each phone's, clear it, and step back.
 */
static void clear_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int prev = zmk_ble_active_profile_index();

	adv_release();
	for (int i = 0; i < g_phones.n && i < PHONES_MAX; i++) {
		int idx = zmk_ble_profile_index(&g_phones.addr[i]);

		if (idx >= 0) {
			zmk_ble_prof_select(idx);
			zmk_ble_clear_bonds();
		}
	}
	if (zmk_ble_active_profile_index() != prev) {
		zmk_ble_prof_select(prev);
	}

	memset(&g_phones, 0, sizeof(g_phones));
	phones_save();
	LOG_INF("remote: paired phones forgotten");
}

/* ---- on / off, and the keymap actions ------------------------------------ */

static void set_on(bool on)
{
	uint8_t v = on;

	g_on = on;
	if (!on) {
		remote_hid_release_all(true);
	}
	/* A key press, not an encoder: one flash write per toggle is fine. */
	settings_save_one("nexus/remote/on", &v, sizeof(v));
	remote_status_kick();
}

bool nexus_remote_action(enum nexus_action action)
{
	switch (action) {
	case NEXUS_ACTION_REMOTE_TOGGLE:
		set_on(!g_on);
		nexus_sound_play(g_on ? NEXUS_SOUND_CONNECT
				      : NEXUS_SOUND_DISCONNECT);
		return true;

	case NEXUS_ACTION_REMOTE_PAIR:
		/* ZMK's profile calls belong on the system work queue, where
		 * ZMK's own behaviors make them. */
		k_work_submit(&g_pair_open_work);
		return true;

	case NEXUS_ACTION_REMOTE_CLEAR:
		k_work_submit(&g_clear_work);
		nexus_sound_play(NEXUS_SOUND_BACK);
		return true;

	default:
		return false;
	}
}

/* ---- bring-up ------------------------------------------------------------ */

static int remote_set(const char *name, size_t len, settings_read_cb read_cb,
		      void *cb_arg)
{
	uint8_t v;

	if (settings_name_steq(name, "on", NULL) && len == sizeof(v) &&
	    read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
		g_on = v != 0;
	} else if (settings_name_steq(name, "phones", NULL) &&
		   len == sizeof(g_phones)) {
		if (read_cb(cb_arg, &g_phones, sizeof(g_phones)) !=
			    sizeof(g_phones) ||
		    g_phones.n > PHONES_MAX) {
			memset(&g_phones, 0, sizeof(g_phones));
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nexus_remote, "nexus/remote", NULL, remote_set,
			       NULL, NULL);

static int remote_init(void)
{
	/* The GATT database is static, so ZMK's HID service sits at the same
	 * handles from boot to power-off. */
	bt_gatt_foreach_attr(0x0001, 0xFFFF, find_hids, NULL);
	if (g_hids_start && !g_hids_end) {
		g_hids_end = 0xFFFF;
	}

	bt_gatt_authorization_cb_register(&k_authz);
	bt_conn_auth_info_cb_register(&g_auth_info);
	nexus_status_subscribe(on_nexus_status);
	return 0;
}

SYS_INIT(remote_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
