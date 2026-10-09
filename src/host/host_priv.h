/*
 * Host link internals, shared by the core (host_link.c) and its transports.
 * Not part of the module API.
 *
 * A transport's whole job is to get bytes off its wire and into its source:
 * host_source_put() or host_source_put_whole(), then host_link_kick(), from
 * whatever context the wire calls it in. Assembling lines, parsing them, the model and the screen all
 * happen on the NEXUS work queue, in host_link.c.
 */
#ifndef NEXUS_HOST_PRIV_H_
#define NEXUS_HOST_PRIV_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/ring_buffer.h>

#include "host_lines.h"

/* A ring holds a whole burst: a full update is under 150 bytes. */
#define HOST_RING_SIZE 256

/*
 * One wire, and everything that belongs to it alone. Two wires must never
 * share a line buffer: their bytes can arrive in the same instant, and a
 * line assembled from both is a number nobody sent.
 */
struct host_source {
	struct ring_buf *ring;
	/* For the ring's own indices, held for a memcpy. */
	struct k_spinlock lock;
	/* The line being assembled. Work queue only. */
	struct host_lines lines;
	/* The last thing put was a gap marker. Under the lock. */
	bool gapped;
};

#define HOST_SOURCE_DEFINE(name)                                               \
	RING_BUF_DECLARE(name##_ring, HOST_RING_SIZE);                         \
	struct host_source name = { .ring = &name##_ring }

/**
 * Bytes off the wire. Any context, an interrupt included: a lock and a
 * memcpy. What does not fit is dropped.
 */
void host_source_put(struct host_source *src, const uint8_t *buf, uint32_t len);

/**
 * The same, all or nothing, for a wire that delivers in packets. A packet
 * the ring cannot take whole is dropped whole, and one HOST_LINES_GAP goes
 * in its place, so the line it was part of is skipped rather than read with
 * a hole in it. A byte of the ring is always kept free for that marker.
 */
void host_source_put_whole(struct host_source *src, const uint8_t *buf,
			   uint32_t len);

/** There are bytes to look at: run the parser on the NEXUS work queue. */
void host_link_kick(void);

/* ---- host_link_usb.c, with CONFIG_NEXUS_HOST_LINK_USB -------------------- */

extern struct host_source host_usb;

/** Bring the USB serial port up. @return 0, or a negative errno. */
int host_link_usb_init(void);

/* ---- host_link_ble.c, with CONFIG_NEXUS_HOST_LINK_BLE -------------------- */

extern struct host_source host_ble;

#endif /* NEXUS_HOST_PRIV_H_ */
