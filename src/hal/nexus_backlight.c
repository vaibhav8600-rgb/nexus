/*
 * Backlight HAL (Section 10).
 *
 * Three honest outcomes, decided by devicetree, never by wishful thinking:
 *   nexus-backlight-pwm alias -> real brightness
 *   nexus-backlight alias     -> on/off only
 *   neither (BL strapped to VCC, as the supplied wiring shows) -> FIXED
 *
 * The Settings screen reads nexus_display_backlight_has_brightness() and hides
 * the slider rather than pretending a GPIO can dim.
 */

#include <nexus/display.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#define BL_PWM_NODE DT_ALIAS(nexus_backlight_pwm)
#define BL_GPIO_NODE DT_ALIAS(nexus_backlight)

#define HAVE_PWM  DT_NODE_HAS_STATUS(BL_PWM_NODE, okay)
#define HAVE_GPIO DT_NODE_HAS_STATUS(BL_GPIO_NODE, okay)

#if HAVE_PWM
static const struct pwm_dt_spec bl_pwm = PWM_DT_SPEC_GET(BL_PWM_NODE);
#elif HAVE_GPIO
static const struct gpio_dt_spec bl_gpio = GPIO_DT_SPEC_GET(BL_GPIO_NODE, gpios);
#endif

static enum nexus_backlight_mode g_mode = NEXUS_BACKLIGHT_FIXED;

bool nexus_display_backlight_has_brightness(void)
{
	return HAVE_PWM && g_mode != NEXUS_BACKLIGHT_FIXED;
}

enum nexus_backlight_mode nexus_display_backlight_mode(void)
{
	return g_mode;
}

int nexus_display_backlight_set(uint8_t percent)
{
#if HAVE_PWM
	uint32_t period = bl_pwm.period;
	uint32_t pulse = (uint32_t)((uint64_t)period * MIN(percent, 100U) / 100U);

	g_mode = percent ? NEXUS_BACKLIGHT_ON : NEXUS_BACKLIGHT_OFF;
	return pwm_set_pulse_dt(&bl_pwm, pulse);
#elif HAVE_GPIO
	g_mode = percent ? NEXUS_BACKLIGHT_ON : NEXUS_BACKLIGHT_OFF;
	return gpio_pin_set_dt(&bl_gpio, percent ? 1 : 0);
#else
	ARG_UNUSED(percent);
	return -ENODEV;
#endif
}

int nexus_display_sleep(bool sleep)
{
	const struct device *disp = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		return -ENODEV;
	}

	/* Blank the panel first, then kill the backlight: the other order shows
	 * a frame of garbage on the way down. */
	int ret = sleep ? display_blanking_on(disp) : display_blanking_off(disp);

	if (g_mode != NEXUS_BACKLIGHT_FIXED) {
		nexus_display_backlight_set(sleep ? 0 : 100);
	}

	return ret;
}

int nexus_backlight_init(void)
{
#if HAVE_PWM || HAVE_GPIO
#if HAVE_PWM
	if (!pwm_is_ready_dt(&bl_pwm)) {
		return -ENODEV;
	}
#else
	if (!gpio_is_ready_dt(&bl_gpio) ||
	    gpio_pin_configure_dt(&bl_gpio, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
#endif
	g_mode = NEXUS_BACKLIGHT_ON;
	return nexus_display_backlight_set(100);
#else
	LOG_INF("no controllable backlight in devicetree - assuming always on");
	g_mode = NEXUS_BACKLIGHT_FIXED;
	return -ENODEV;
#endif
}
