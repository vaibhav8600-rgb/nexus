/*
 * Display / backlight HAL (Sections 7-10, 71-72).
 *
 * UI and game code call these. Neither ever names a GPIO, a SPI bus or
 * "ST7789" - swapping in an ILI9341 is a devicetree change (Section 141-D).
 */
#ifndef NEXUS_DISPLAY_H_
#define NEXUS_DISPLAY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nexus_backlight_mode {
	NEXUS_BACKLIGHT_OFF = 0,
	NEXUS_BACKLIGHT_ON,
	/* Hardware has no controllable backlight (BL strapped to VCC). Reported
	 * honestly rather than pretending to dim (Section 10). */
	NEXUS_BACKLIGHT_FIXED,
};

/** True when brightness is a real PWM duty cycle rather than on/off. */
bool nexus_display_backlight_has_brightness(void);

enum nexus_backlight_mode nexus_display_backlight_mode(void);

/**
 * @param percent 0 = off, 100 = full. Rounded to on/off on GPIO-only hardware.
 * @return 0, or -ENODEV when there is no backlight to control.
 */
int nexus_display_backlight_set(uint8_t percent);

/** Blank/unblank the panel itself for idle power saving (Section 66). */
int nexus_display_sleep(bool sleep);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_DISPLAY_H_ */
