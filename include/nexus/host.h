/*
 * Host link (companion app over USB serial).
 *
 * Everything the dongle shows comes from ZMK, which knows about the keyboard
 * and nothing else. This is the other half: a small program on the machine
 * the dongle is plugged into, pushing what only that machine knows - the
 * time, how busy it is, what it is playing.
 *
 * Deliberately one-way and text. The dongle never answers, so the companion
 * cannot be used to drive the keyboard, and a line protocol can be read with
 * a serial terminal when it does not work. See docs/host-link.md.
 *
 * The link is optional in every sense: off by default, its devicetree node
 * disabled, and every field reads as unknown when nothing is connected. A
 * screen that shows host data must handle nexus_host()->link being false,
 * because that is the normal state for most of the day.
 */
#ifndef NEXUS_HOST_H_
#define NEXUS_HOST_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest now-playing string kept, including the terminator. */
#define NEXUS_HOST_TEXT 40

struct nexus_host {
	/** A companion has spoken recently. False means every field below is
	 *  stale and must be drawn as unknown, not as its last value. */
	bool link;

	/** 0-100, or NEXUS_HOST_UNKNOWN. */
	uint8_t cpu;
	uint8_t mem;
#define NEXUS_HOST_UNKNOWN 0xFF

	/** Whatever the host is playing, already truncated. Empty for none. */
	char now_playing[NEXUS_HOST_TEXT];

	/*
	 * Wall-clock, as seconds since midnight local time, plus the uptime it
	 * arrived at. The dongle has no RTC: this is the only real time it
	 * will ever see, and between updates it counts with k_uptime_get(),
	 * which drifts. The companion resends it every minute, so the drift
	 * never accumulates past that.
	 *
	 * Seconds-since-midnight rather than a Unix epoch because the dongle
	 * has no timezone database and no business having one - the host has
	 * already decided what the clock on its own wall says.
	 */
	uint32_t clock_sec;
	int64_t clock_at;
};

/** The live host model. Never NULL; all-unknown until a companion connects. */
const struct nexus_host *nexus_host(void);

/**
 * Seconds since local midnight right now, or UINT32_MAX if the host has
 * never sent a time. Counts forward from the last update with the kernel
 * uptime, and wraps at midnight.
 */
uint32_t nexus_host_clock(void);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_HOST_H_ */
