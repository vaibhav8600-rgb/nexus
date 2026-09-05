/*
 * Split half connect / disconnect, observed rather than guessed.
 *
 * ZMK's split central raises no events at all - not for connect, not for
 * disconnect (`grep raise_zmk_ app/src/split/bluetooth/central.c` returns
 * nothing), and snake-module's peripheral_status.c handler is an empty stub
 * with "do we need this ?" in it. So there is nothing in ZMK to subscribe to.
 *
 * The previous attempt inferred a disconnect from a half going quiet for
 * 150 s. That was wrong twice over: it announced nothing for two and a half
 * minutes after a half actually went away, and it announced a disconnect
 * every time a half simply dozed off between battery reports - which is what
 * the random beeping was.
 *
 * Zephyr's own connection callbacks are the real signal, and they are
 * available on the dongle because the dongle IS the BLE central for both
 * halves. ZMK itself registers three separate callback sets (ble.c,
 * split/central.c, split/peripheral.c), so registering a fourth is the
 * intended way to use this API, not a trick.
 *
 * Role is what separates a half from the host: on a link to a keyboard half
 * the dongle is the central, on the link to the PC it is the peripheral.
 */

#include <nexus/sound.h>
#include <nexus/status.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/*
 * Slots are keyed by peer address, assigned in first-seen order - the same
 * order ZMK's own reserve_peripheral_slot() uses, so slot 0 here is slot 0
 * there and the chirp agrees with the battery card. If a board ever pairs in
 * the other order, CONFIG_NEXUS_SPLIT_SWAP_SIDES flips both together, because
 * both go through the same slot-to-side mapping in status.c.
 */
static bt_addr_le_t g_peer[2];
static bool g_used[2];
static bool g_up[2];

/*
 * Address -> slot, with a fallback that matters more than the fast path.
 *
 * A BLE peer does not have to present the same address every time. With
 * privacy enabled a peripheral advertises a resolvable private address that
 * rotates, and what `bt_conn_get_dst()` hands back can differ between one
 * connection and the next for the same physical half.
 *
 * The first version of this only matched on the address and then took a free
 * slot. After one disconnect/reconnect cycle that produced exactly the bug
 * reported: the disconnect matched the recorded address and chirped, the
 * reconnect arrived under a new address, found no match and no free slot,
 * returned -1, and the connect cue was silently dropped. "Disconnect works,
 * connect does not" is that -1.
 *
 * So an unrecognised address now claims a slot that is currently down, which
 * is almost certainly the same half coming back under a new address. Only a
 * genuine third peripheral - two halves already connected - is refused.
 */
static int slot_for(const bt_addr_le_t *addr)
{
	for (int i = 0; i < 2; i++) {
		if (g_used[i] && bt_addr_le_cmp(&g_peer[i], addr) == 0) {
			return i;
		}
	}
	for (int i = 0; i < 2; i++) {
		if (!g_used[i]) {
			bt_addr_le_copy(&g_peer[i], addr);
			g_used[i] = true;
			return i;
		}
	}
	for (int i = 0; i < 2; i++) {
		if (!g_up[i]) {
			bt_addr_le_copy(&g_peer[i], addr);
			return i;
		}
	}
	/* ponytail: two halves is the only split topology NEXUS renders.
	 * A third peripheral is ignored rather than evicting a live one. */
	return -1;
}

/* True when this link is one of the keyboard halves rather than the host. */
static bool is_peripheral_half(struct bt_conn *conn)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0) {
		return false;
	}
	if (info.type != BT_CONN_TYPE_LE) {
		return false;
	}
	return info.role == BT_CONN_ROLE_CENTRAL;
}

/* Slot for an address we have already seen, or -1. No allocation. */
static int known_slot(const bt_addr_le_t *addr)
{
	for (int i = 0; i < 2; i++) {
		if (g_used[i] && bt_addr_le_cmp(&g_peer[i], addr) == 0) {
			return i;
		}
	}
	return -1;
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U) {
		return;
	}

	const bt_addr_le_t *addr = bt_conn_get_dst(conn);
	int slot = known_slot(addr);

	/*
	 * An address we have already accepted as a half is a half, whatever
	 * bt_conn_get_info() reports this time round. The role test is only
	 * needed to tell a NEW peer from the host, and leaning on it for every
	 * event made the connect path strictly more fragile than the
	 * disconnect path - which is the shape of the bug being chased here:
	 * powering a half off chirps every time, powering it back on does not.
	 */
	if (slot < 0) {
		if (!is_peripheral_half(conn)) {
			LOG_DBG("ignoring connection: not a peripheral half");
			return;
		}
		slot = slot_for(addr);
	}

	if (slot >= 0) {
		LOG_DBG("half %d connected", slot);
		g_up[slot] = true;
		nexus_status_half_link(slot, true);
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	const bt_addr_le_t *addr = bt_conn_get_dst(conn);
	int slot = known_slot(addr);

	if (slot < 0) {
		if (!is_peripheral_half(conn)) {
			return;
		}
		slot = slot_for(addr);
	}

	if (slot >= 0) {
		LOG_DBG("half %d disconnected (reason %u)", slot, reason);
		g_up[slot] = false;
		nexus_status_half_link(slot, false);
	}
}

BT_CONN_CB_DEFINE(nexus_split_conn) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};
