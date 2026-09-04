/*
 * Action button HAL (Sections 11, 94).
 *
 * GPIO interrupt -> debounce -> gesture. The long press fires the moment the
 * threshold is crossed (it does not wait for release), which is what makes
 * "hold to exit" feel right, and it explicitly cannot decay into a burst of
 * short presses because the release path checks whether long already fired.
 */

#include <nexus/action.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#define BUTTON_NODE DT_ALIAS(nexus_button)

#if DT_NODE_HAS_STATUS(BUTTON_NODE, okay)

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
static struct gpio_callback cb_data;

static bool held;
static bool long_fired;

static void long_press_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(long_work, long_press_work);

static void debounce_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(debounce_work, debounce_work_fn);

static void long_press_work(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!held || long_fired) {
		return;
	}

	long_fired = true;
	nexus_action_button_event(true);
}

static void debounce_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int level = gpio_pin_get_dt(&btn);

	if (level < 0) {
		return;
	}

	bool now = (level != 0);

	if (now == held) {
		/* Bounce that settled back where it started - nothing happened. */
		return;
	}

	held = now;

	if (held) {
		long_fired = false;
		k_work_reschedule(&long_work,
				  K_MSEC(CONFIG_NEXUS_BUTTON_LONG_PRESS_MS));
	} else {
		k_work_cancel_delayable(&long_work);
		if (!long_fired) {
			nexus_action_button_event(false);
		}
	}
}

static void button_isr(const struct device *port, struct gpio_callback *cb,
		       gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Reschedule, so a bouncing contact keeps pushing the decision out
	 * instead of producing one event per bounce edge. */
	k_work_reschedule(&debounce_work, K_MSEC(CONFIG_NEXUS_BUTTON_DEBOUNCE_MS));
}

int nexus_button_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&btn)) {
		LOG_WRN("action button GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&btn, GPIO_INPUT);
	if (ret) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);
	if (ret) {
		return ret;
	}

	gpio_init_callback(&cb_data, button_isr, BIT(btn.pin));
	return gpio_add_callback(btn.port, &cb_data);
}

#else /* no button wired */

int nexus_button_init(void)
{
	return -ENODEV;
}

#endif
