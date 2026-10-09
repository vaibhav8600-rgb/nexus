/*
 * The host link over USB: a CDC serial port the host writes lines to.
 *
 * A second serial interface rather than the one ZMK Studio already uses.
 * Studio's port carries its own protobuf RPC and Studio expects to own it;
 * sharing it would mean forking ZMK's RPC to carry unrelated traffic, and the
 * two would fight over the port whenever Studio was open. USB gives us as
 * many interfaces as the endpoints allow, and this costs one.
 *
 * The interrupt does the least it can: move bytes into the source's ring and
 * post the work. Lines are assembled and parsed on the NEXUS work queue
 * (host_link.c). Model updates and repaints from an ISR would be a much worse
 * bug than a few bytes of latency.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "host_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#define LINK_NODE DT_NODELABEL(nexus_host_cdc)

#if !DT_NODE_HAS_STATUS(LINK_NODE, okay)
#error "CONFIG_NEXUS_HOST_LINK needs the nexus_host_cdc node enabled - see docs/host-link.md"
#endif

static const struct device *const g_uart = DEVICE_DT_GET(LINK_NODE);

HOST_SOURCE_DEFINE(host_usb);

static void uart_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}

	bool any = false;

	while (uart_irq_rx_ready(dev)) {
		uint8_t buf[16];
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}

		/*
		 * The FIFO must be drained whether or not the ring has room,
		 * or the interrupt stays asserted and fires forever. So a
		 * full ring drops what does not fit - which takes a host
		 * sending 256 bytes faster than the work queue runs. The
		 * torn line that leaves is rejected whole by to_u32(), not
		 * misread as a number.
		 */
		host_source_put(&host_usb, buf, (uint32_t)n);
		any = true;
	}

	if (any) {
		host_link_kick();
	}
}

int host_link_usb_init(void)
{
	if (!device_is_ready(g_uart)) {
		LOG_WRN("host link: %s not ready", g_uart->name);
		return -ENODEV;
	}

	int ret = uart_irq_callback_user_data_set(g_uart, uart_cb, NULL);

	if (ret) {
		LOG_WRN("host link: no interrupt-driven UART (%d)", ret);
		return ret;
	}

	uart_irq_rx_enable(g_uart);
	LOG_INF("host link ready on %s", g_uart->name);
	return 0;
}
