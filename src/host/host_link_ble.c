/*
 * The host link over Bluetooth: one write-only characteristic on the
 * connection the host already has.
 *
 * The companion writes the same lines here that it writes to the USB serial
 * port, so the HOST screen works with the dongle on a charger and the laptop
 * reached over Bluetooth alone. Nothing is added to the radio for it: no
 * connection, no advertising, no connection parameters. It is a few more
 * attributes on a link ZMK already keeps.
 *
 * One way, like the serial port: there is nothing here to read, notify or
 * indicate. The handler runs on the Bluetooth RX thread and does what the
 * UART interrupt does - copy the bytes into this source's ring and post the
 * work. Lines are assembled and parsed on the NEXUS work queue
 * (host_link.c).
 */

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

#include <zmk/ble.h>

#include "host_priv.h"

/* Random, and unrelated to Remote Input's: a host and a phone are different
 * things to be. Recorded in docs/host-link.md. */
#define HOST_SVC_UUID                                                          \
	BT_UUID_128_ENCODE(0xbbb8ee45, 0x91c5, 0x40fb, 0x8c33, 0x1c8b9c4795ccULL)
#define HOST_RX_UUID                                                           \
	BT_UUID_128_ENCODE(0x40064c5a, 0xdc78, 0x4b9e, 0xb148, 0x9e13bcbd7708ULL)

static const struct bt_uuid_128 k_svc = BT_UUID_INIT_128(HOST_SVC_UUID);
static const struct bt_uuid_128 k_rx = BT_UUID_INIT_128(HOST_RX_UUID);

HOST_SOURCE_DEFINE(host_ble);

/*
 * The host ZMK is typing into, and nobody else: not another bonded host on
 * a profile that is not selected, not a Remote Input phone (its profile is
 * the active one only for the moment it pairs), not a keyboard half - on
 * those links the dongle is the central, not the peripheral.
 *
 * zmk_ble_active_profile_addr(), not ..._conn(): that one logs a warning
 * every time the profile is not connected, and returns a reference to drop.
 * The address is read without ZMK's lock, on another thread than the one
 * that changes it; a torn read can only take or refuse one write wrongly.
 */
static bool from_active_host(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return bt_conn_get_info(conn, &info) == 0 &&
	       info.role == BT_CONN_ROLE_PERIPHERAL &&
	       bt_addr_le_cmp(bt_conn_get_dst(conn),
			      zmk_ble_active_profile_addr()) == 0;
}

static ssize_t write_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf, uint16_t len, uint16_t offset,
			uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	/* Anyone else is not told no: a Write Without Response has no reply
	 * to carry it, and their bytes simply go nowhere. */
	if (len && from_active_host(conn)) {
		host_source_put_whole(&host_ble, buf, len);
		host_link_kick();
	}
	return len;
}

/*
 * Named to sort last. Static services are laid out in the database in the
 * order of their names (Zephyr's own are _1_gatt_svc and _2_gap_svc for the
 * same reason), and a service added in the middle moves the handle of every
 * attribute after it. Going last leaves each one where the peers bonded to
 * this dongle have it cached: ZMK's HID service for hosts, Remote Input's for
 * phones.
 *
 * Encrypted, which every bonded ZMK host is. Not authenticated: hosts pair
 * without a passkey, and what arrives here can only change what the HOST
 * screen shows.
 */
BT_GATT_SERVICE_DEFINE(zz_nexus_host_svc,
	BT_GATT_PRIMARY_SERVICE(&k_svc),
	BT_GATT_CHARACTERISTIC(&k_rx.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_rx, NULL),
);
