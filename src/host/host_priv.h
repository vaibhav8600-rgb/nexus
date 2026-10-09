/*
 * Host link internals, shared by the core (host_link.c) and its transports.
 * Not part of the module API.
 *
 * A transport's whole job is to get bytes off its wire and into its source:
 * host_source_put() and host_link_kick(), from whatever context the wire
 * calls it in. Assembling lines, parsing them, the model and the screen all
 * happen on the NEXUS work queue, in host_link.c.
 */
#ifndef NEXUS_HOST_PRIV_H_
#define NEXUS_HOST_PRIV_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/ring_buffer.h>

/* Longest line we will look at. Anything longer is truncated rather than
 * split, so a runaway host cannot desynchronise the parser. */
#define HOST_LINE_MAX 72

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
	char line[HOST_LINE_MAX];
	uint8_t line_len;
};

#define HOST_SOURCE_DEFINE(name)                                               \
	RING_BUF_DECLARE(name##_ring, HOST_RING_SIZE);                         \
	struct host_source name = { .ring = &name##_ring }

/**
 * Bytes off the wire. Any context, an interrupt included: a lock and a
 * memcpy. What does not fit is dropped.
 */
void host_source_put(struct host_source *src, const uint8_t *buf, uint32_t len);

/** There are bytes to look at: run the parser on the NEXUS work queue. */
void host_link_kick(void);

/* ---- host_link_usb.c ----------------------------------------------------- */

extern struct host_source host_usb;

/** Bring the USB serial port up. @return 0, or a negative errno. */
int host_link_usb_init(void);

#endif /* NEXUS_HOST_PRIV_H_ */
