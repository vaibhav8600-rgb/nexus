/*
 * Passive buzzer HAL (Sections 15, 73).
 *
 * A passive buzzer has no oscillator of its own - it needs a driven waveform,
 * so this is PWM at 50% duty, not a GPIO toggle. Nothing above this file knows
 * a PWM exists; the volume pot on the module stays the analogue volume control.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#define BUZZER_NODE DT_ALIAS(nexus_buzzer)

#if DT_NODE_HAS_STATUS(BUZZER_NODE, okay)

static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(BUZZER_NODE);
static bool ready;

int nexus_buzzer_init(void)
{
	if (!pwm_is_ready_dt(&buzzer)) {
		LOG_WRN("buzzer PWM not ready - sound disabled, keyboard unaffected");
		return -ENODEV;
	}

	ready = true;
	nexus_buzzer_tone(0);
	return 0;
}

bool nexus_buzzer_ready(void)
{
	return ready;
}

int nexus_buzzer_tone(uint16_t freq_hz)
{
	if (!ready) {
		return -ENODEV;
	}

	if (freq_hz == 0) {
		/* Park the line low rather than leaving a DC level on the coil. */
		return pwm_set_dt(&buzzer, PWM_HZ(1000), 0);
	}

	uint32_t period = PWM_HZ(freq_hz);

	return pwm_set_dt(&buzzer, period, period / 2U);
}

#else /* no buzzer wired */

int nexus_buzzer_init(void)
{
	return -ENODEV;
}

bool nexus_buzzer_ready(void)
{
	return false;
}

int nexus_buzzer_tone(uint16_t freq_hz)
{
	ARG_UNUSED(freq_hz);
	return -ENODEV;
}

#endif
