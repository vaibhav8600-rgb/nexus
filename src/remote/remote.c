/*
 * Remote Input: the phone's side of the radio (docs/remote-input.md).
 *
 * The phone is kept apart from ZMK by three things, each covering a way it
 * could otherwise end up as one of ZMK's hosts:
 *
 *   Identity. It connects to "NEXUS Remote", an advertiser on a second
 *   Bluetooth identity, never to ZMK's. Its bonds live under that identity,
 *   so forgetting phones can never touch a host's or a half's bond.
 *
 *   Authorization. Zephyr has one GATT database for every connection, so
 *   the phone can see ZMK's HID service. A custom authorization callback
 *   refuses every read and write in it on the phone's link - the phone's OS
 *   cannot load the report map and so cannot adopt NEXUS as a keyboard,
 *   which is what would hide its own on-screen keyboard. The same callback
 *   keeps everyone but the phone out of this service.
 *
 *   Pairing. Per-connection auth callbacks (bt_conn_auth_cb_overlay) give
 *   the phone's link a passkey display and a pairing window, and leave
 *   ZMK's global callbacks exactly as they are for everything else.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include <zmk/ble.h>

#include <nexus/remote.h>
#include <nexus/sound.h>
#include <nexus/status.h>

#include "../nexus_priv.h"
#include "remote_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* Kconfig cannot say this without a dependency loop (see the Kconfig). */
BUILD_ASSERT(IS_ENABLED(CONFIG_SETTINGS),
	     "Remote Input keeps the phone's identity and bonds in settings: "
	     "enable CONFIG_SETTINGS");

#define PHONES_MAX 2
#define PAIR_WINDOW K_SECONDS(60)

/* 211.25 ms: on Apple's list of intervals iOS scans for well, and about
 * five packets a second - nothing next to the halves' connection events. */
#define ADV_INTERVAL 338

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

static bool g_ready;       /* identity exists, advertiser may start */
static uint8_t g_id;       /* the phone's identity; never BT_ID_DEFAULT */
static bool g_on = true;   /* remote mode, persisted */
static struct bt_le_ext_adv *g_adv;
static uint16_t g_hids_start, g_hids_end;

static struct k_spinlock g_lock;
static struct bt_conn *g_phone; /* guarded by g_lock */

static bt_addr_le_t g_paired_addr;
static uint8_t g_last_status[6];

static void adv_fn(struct k_work *work);
static void status_fn(struct k_work *work);
static void pair_close_fn(struct k_work *work);
static void zmk_fix_fn(struct k_work *work);
static void init_fn(struct k_work *work);
static K_WORK_DEFINE(g_adv_work, adv_fn);
static K_WORK_DELAYABLE_DEFINE(g_adv_retry, adv_fn);
static K_WORK_DELAYABLE_DEFINE(g_status_work, status_fn);
static K_WORK_DELAYABLE_DEFINE(g_pair_close, pair_close_fn);
static K_WORK_DEFINE(g_zmk_fix, zmk_fix_fn);
static K_WORK_DEFINE(g_init_work, init_fn);

/* A reference to the phone's connection, or NULL. Caller unrefs. */
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

static bool is_phone(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return g_ready && bt_conn_get_info(conn, &info) == 0 &&
	       info.id == g_id;
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

/* ---- GATT ---------------------------------------------------------------- */

static void status_bytes(uint8_t v[6])
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
	v[5] = BIT(0) | BIT(1) | BIT(2); /* text, consumer, hwheel */
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
	default:
		return BT_GATT_ERR(REMOTE_ERR_VALUE);
	}
	return len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t v[6];

	status_bytes(v);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, v, sizeof(v));
}

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
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ_LESC | BT_GATT_PERM_WRITE_LESC),
);

/* Primary service, then two attributes per characteristic: Status is 10. */
#define STATUS_ATTR (&nexus_remote_svc.attrs[10])

static bool in_service(const struct bt_gatt_attr *attr)
{
	return attr >= nexus_remote_svc.attrs &&
	       attr < nexus_remote_svc.attrs + nexus_remote_svc.attr_count;
}

static bool authorize(struct bt_conn *conn, const struct bt_gatt_attr *attr)
{
	bool phone = is_phone(conn);

	if (in_service(attr)) {
		return phone;
	}
	if (!phone || !g_hids_start) {
		return true;
	}

	/* The handle walk is only paid on the phone's link, and the phone
	 * has no business outside this service after discovery. */
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

/* ---- status -------------------------------------------------------------- */

static void status_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = phone_get();
	uint8_t v[6];

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
}

/* ---- advertising --------------------------------------------------------- */

static const struct bt_data k_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, REMOTE_UUID(1)),
};

/* The name does not fit beside a 128-bit UUID in 31 bytes, so it goes in the
 * scan response, which every phone asks for. */
static const struct bt_data k_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, "NEXUS Remote", 12),
};

static void adv_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_conn *conn = phone_get();
	int err;

	if (conn) {
		bt_conn_unref(conn);
		return; /* one phone at a time */
	}
	if (!g_ready) {
		return;
	}

	if (!g_adv) {
		struct bt_le_adv_param p = BT_LE_ADV_PARAM_INIT(
			BT_LE_ADV_OPT_CONN, ADV_INTERVAL, ADV_INTERVAL, NULL);

		p.id = g_id;
		err = bt_le_ext_adv_create(&p, NULL, &g_adv);
		if (!err) {
			err = bt_le_ext_adv_set_data(g_adv, k_ad, ARRAY_SIZE(k_ad),
						     k_sd, ARRAY_SIZE(k_sd));
		}
		if (err) {
			LOG_ERR("remote: advertiser setup failed (%d)", err);
			return;
		}
	}

	err = bt_le_ext_adv_start(g_adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err && err != -EALREADY) {
		/* Usually no free connection object yet - one is released
		 * just after a disconnect. Try again shortly. */
		LOG_DBG("remote: advertising deferred (%d)", err);
		k_work_schedule(&g_adv_retry, K_SECONDS(1));
	}
}

/* ---- connections --------------------------------------------------------- */

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *feat);
static void passkey_display(struct bt_conn *conn, unsigned int passkey);
static void pairing_cancel(struct bt_conn *conn);

static const struct bt_conn_auth_cb k_auth = {
	.pairing_accept = pairing_accept,
	.passkey_display = passkey_display,
	.cancel = pairing_cancel,
};

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	bool busy = false;

	if (!is_phone(conn)) {
		return;
	}
	if (err) {
		k_work_submit(&g_adv_work);
		return;
	}

	K_SPINLOCK(&g_lock) {
		if (g_phone) {
			busy = true;
		} else {
			g_phone = bt_conn_ref(conn);
		}
	}
	if (busy) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	/* Before anything can start SMP on this link. */
	bt_conn_auth_cb_overlay(conn, &k_auth);
	bt_conn_le_param_update(conn, PHONE_CONN_PARAM);

	memset(g_last_status, 0xFF, sizeof(g_last_status));
	remote_hid_set_delay(CONFIG_NEXUS_REMOTE_INPUT_TYPE_DELAY_MS);
	remote_status_kick();
	LOG_INF("remote: phone connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	bool ours = false;

	K_SPINLOCK(&g_lock) {
		if (g_phone == conn) {
			g_phone = NULL;
			ours = true;
		}
	}
	if (!ours) {
		return;
	}

	bt_conn_unref(conn);
	remote_hid_release_all(true);
	nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_PASSKEY);
	remote_status_kick();
	LOG_INF("remote: phone disconnected (0x%02x)", reason);
}

/* A connection object came free - the phone's, or one it was waiting for. */
static void on_recycled(void)
{
	k_work_submit(&g_adv_work);
}

BT_CONN_CB_DEFINE(nexus_remote_conn) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
};

/* ---- pairing ------------------------------------------------------------- */

struct bond_count {
	const bt_addr_le_t *except;
	int n;
};

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
	struct bond_count *c = user_data;

	if (bt_addr_le_cmp(&info->addr, c->except)) {
		c->n++;
	}
}

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *feat)
{
	ARG_UNUSED(feat);

	struct bond_count c = {.except = bt_conn_get_dst(conn)};

	if (!pair_open()) {
		LOG_WRN("remote: pairing refused, window closed");
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}

	bt_foreach_bond(g_id, count_bond, &c);
	if (c.n >= PHONES_MAX) {
		LOG_WRN("remote: pairing refused, %d phones already paired",
			PHONES_MAX);
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	return BT_SECURITY_ERR_SUCCESS;
}

static void passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(conn);

	nexus_remote_screen_show(NEXUS_REMOTE_VIEW_PASSKEY, passkey);
}

static void pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_PASSKEY);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	if (!is_phone(conn)) {
		return;
	}

	LOG_INF("remote: phone paired%s", bonded ? " and bonded" : "");
	k_work_cancel_delayable(&g_pair_close);
	nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_PASSKEY);
	nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_WAIT);
	nexus_sound_play(NEXUS_SOUND_CONNECT);

	bt_addr_le_copy(&g_paired_addr, bt_conn_get_dst(conn));
	k_work_submit(&g_zmk_fix);
	remote_status_kick();
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	if (!is_phone(conn)) {
		return;
	}

	LOG_WRN("remote: pairing failed (%d)", reason);
	nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_PASSKEY);
	nexus_sound_play(NEXUS_SOUND_BACK);
}

static struct bt_conn_auth_info_cb g_auth_info = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/*
 * ponytail: ZMK's auth_pairing_complete() (app/src/ble.c) checks the role of
 * a new pairing but not its identity, so the phone's pairing is written into
 * the active host profile whenever that profile is open. Nothing is ever
 * sent to it - ZMK looks profiles up on the default identity - but the
 * profile would read as taken. Put it back. The real fix is one
 * `if (info.id != BT_ID_DEFAULT) return;` upstream; drop this when it lands.
 */
static void zmk_fix_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (bt_addr_le_cmp(zmk_ble_active_profile_addr(), &g_paired_addr) == 0) {
		LOG_INF("remote: returning the host profile ZMK gave the phone");
		zmk_ble_clear_bonds();
	}
}

void remote_pair_cancel(void)
{
	struct bt_conn *conn = phone_get();

	k_work_cancel_delayable(&g_pair_close);
	if (conn) {
		bt_conn_auth_cancel(conn); /* harmless if nothing is pending */
		bt_conn_unref(conn);
	}
	remote_status_kick();
}

static void pair_close_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_WAIT);
	remote_status_kick();
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
		k_work_reschedule(&g_pair_close, PAIR_WINDOW);
		nexus_remote_screen_show(NEXUS_REMOTE_VIEW_WAIT, 0);
		nexus_sound_play(NEXUS_SOUND_MENU_OPEN);
		remote_status_kick();
		return true;

	case NEXUS_ACTION_REMOTE_CLEAR:
		if (g_ready) {
			/* Every bond under the phone identity, and nothing
			 * else: the halves and the hosts live on the default
			 * one. Disconnects a connected phone too. */
			bt_unpair(g_id, NULL);
			LOG_INF("remote: paired phones forgotten");
		}
		nexus_sound_play(NEXUS_SOUND_BACK);
		return true;

	default:
		return false;
	}
}

/* ---- bring-up ------------------------------------------------------------ */

/*
 * After settings have loaded, never before: the phone's identity is itself
 * a stored setting, and creating it early would mint a new address on every
 * boot and orphan every phone's bond.
 */
static void init_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t n = ARRAY_SIZE(addrs);
	int id;

	if (g_ready) {
		return;
	}

	bt_id_get(addrs, &n);
	id = n > 1 ? 1 : bt_id_create(NULL, NULL);
	if (id < 1) {
		LOG_ERR("remote: no identity for the phone (%d)", id);
		return;
	}

	g_id = (uint8_t)id;
	bt_gatt_foreach_attr(0x0001, 0xFFFF, find_hids, NULL);
	if (g_hids_start && !g_hids_end) {
		g_hids_end = 0xFFFF;
	}
	g_ready = true;

	LOG_INF("remote: identity %d, remote %s", g_id, g_on ? "on" : "off");
	k_work_submit(&g_adv_work);
	remote_status_kick();
}

static int remote_set(const char *name, size_t len, settings_read_cb read_cb,
		      void *cb_arg)
{
	uint8_t v;

	if (settings_name_steq(name, "on", NULL) && len == sizeof(v) &&
	    read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
		g_on = v != 0;
	}
	return 0;
}

static int remote_commit(void)
{
	k_work_submit(&g_init_work);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nexus_remote, "nexus/remote", NULL, remote_set,
			       remote_commit, NULL);

static int remote_init(void)
{
	bt_gatt_authorization_cb_register(&k_authz);
	bt_conn_auth_info_cb_register(&g_auth_info);
	nexus_status_subscribe(on_nexus_status);
	return 0;
}

SYS_INIT(remote_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
