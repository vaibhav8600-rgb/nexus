/*
 * NEXUS Smart ZMK Dongle - platform entry points.
 *
 * Design contract (Requirements Section 87/141-A): every subsystem here is
 * NON-CRITICAL. Each init returns an error instead of asserting, and every
 * public call is a no-op when its subsystem failed to come up. ZMK must keep
 * typing even with the display unplugged.
 */
#ifndef NEXUS_NEXUS_H_
#define NEXUS_NEXUS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEXUS_VERSION_MAJOR 1
#define NEXUS_VERSION_MINOR 0
#define NEXUS_VERSION_PATCH 0
#define NEXUS_VERSION_STR "1.0.0"

/* Branding is configuration, never source (Section 22 / 141-B). */
#define NEXUS_BRAND    CONFIG_NEXUS_BRAND
#define NEXUS_PRODUCT  CONFIG_NEXUS_PRODUCT
#define NEXUS_SUBTITLE CONFIG_NEXUS_SUBTITLE

/** Which optional subsystems actually came up on this boot. */
struct nexus_health {
	bool display;
	bool backlight;
	bool buzzer;
	bool button;
	bool games;
};

const struct nexus_health *nexus_health(void);

/** Human-readable one-liner for the diagnostics screen. */
const char *nexus_board_name(void);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_NEXUS_H_ */
