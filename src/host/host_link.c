/*
 * The companion link: a USB CDC serial the host writes lines to.
 *
 * A second serial interface rather than the one ZMK Studio already uses.
 * Studio's port carries its own protobuf RPC and Studio expects to own it;
 * sharing it would mean forking ZMK's RPC to carry unrelated traffic, and the
 * two would fight over the port whenever Studio was open. USB gives us as
 * many interfaces as the endpoints allow, and this costs one.
 *
 * Parsing happens on the NEXUS work queue, never in the UART interrupt. The
 * ISR does the least it can: copy bytes into a line buffer, and when a line
 * completes, hand it over and post the work. Model updates and repaints from
 * an ISR would be a much worse bug than a few bytes of latency.
 */

#include <nexus/host.h>
#include <nexus/screen.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#define LINK_NODE DT_NODELABEL(nexus_host_cdc)

#if !DT_NODE_HAS_STATUS(LINK_NODE, okay)
#error "CONFIG_NEXUS_HOST_LINK needs the nexus_host_cdc node enabled - see docs/host-link.md"
#endif

static const struct device *const g_uart = DEVICE_DT_GET(LINK_NODE);

/* Longest line we will look at. Anything longer is truncated rather than
 * split, so a runaway host cannot desynchronise the parser. */
#define LINE_MAX 72

static struct nexus_host g_host = {
	.cpu = NEXUS_HOST_UNKNOWN,
	.mem = NEXUS_HOST_UNKNOWN,
	.clock_sec = UINT32_MAX,
};

/*
 * Two buffers, because the ISR fills one while the work item reads the other.
 * A single buffer would need a lock held across a parse, in an interrupt.
 */
static char g_rx[LINE_MAX];
static uint8_t g_rx_len;
static char g_line[LINE_MAX];
static atomic_t g_line_ready;

static void parse_work_fn(struct k_work *work);
static K_WORK_DEFINE(g_parse, parse_work_fn);

static void stale_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_stale, stale_work_fn);

const struct nexus_host *nexus_host(void)
{
	return &g_host;
}

uint32_t nexus_host_clock(void)
{
	if (g_host.clock_sec == UINT32_MAX) {
		return UINT32_MAX;
	}

	int64_t since = (k_uptime_get() - g_host.clock_at) / MSEC_PER_SEC;

	return (uint32_t)((g_host.clock_sec + since) % 86400U);
}

/* ---- parsing ------------------------------------------------------------ */

static uint32_t to_u32(const char *s, uint32_t max)
{
	uint32_t v = 0;

	if (*s < '0' || *s > '9') {
		return UINT32_MAX;
	}
	for (; *s >= '0' && *s <= '9'; s++) {
		v = v * 10U + (uint32_t)(*s - '0');
		if (v > max) {
			return UINT32_MAX;
		}
	}
	return v;
}

/*
 * One line. The first character is the field; anything after a single space
 * is its value. Unknown fields are ignored rather than rejected, so a newer
 * companion talking to older firmware degrades instead of failing.
 */
static void parse_line(const char *line)
{
	char key = line[0];
	const char *val = line[1] == ' ' ? &line[2] : &line[1];
	uint32_t n;

	switch (key) {
	case 'C':
		n = to_u32(val, 100);
		g_host.cpu = (n == UINT32_MAX) ? NEXUS_HOST_UNKNOWN
					       : (uint8_t)n;
		break;
	case 'M':
		n = to_u32(val, 100);
		g_host.mem = (n == UINT32_MAX) ? NEXUS_HOST_UNKNOWN
					       : (uint8_t)n;
		break;
	case 'T':
		n = to_u32(val, 86399);
		if (n != UINT32_MAX) {
			g_host.clock_sec = n;
			g_host.clock_at = k_uptime_get();
		}
		break;
	case 'N':
		strncpy(g_host.now_playing, val, NEXUS_HOST_TEXT - 1);
		g_host.now_playing[NEXUS_HOST_TEXT - 1] = '\0';
		break;
	case 'X':
		/* The companion is going away and says so, rather than leaving
		 * numbers on screen that stopped being true when it quit. */
		g_host.cpu = NEXUS_HOST_UNKNOWN;
		g_host.mem = NEXUS_HOST_UNKNOWN;
		g_host.now_playing[0] = '\0';
		g_host.link = false;
		nexus_screen_invalidate();
		return;
	default:
		return;
	}

	g_host.link = true;
	nexus_screen_invalidate();
}

static void parse_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_cas(&g_line_ready, 1, 0)) {
		return;
	}
	parse_line(g_line);

	/*
	 * Restart the staleness timer on every line. A companion that is
	 * killed, or a cable pulled, stops sending - and the screen has to
	 * stop claiming a 3% CPU that was true a minute ago.
	 */
	k_work_reschedule_for_queue(nexus_workq(), &g_stale,
				    K_SECONDS(CONFIG_NEXUS_HOST_STALE_S));
}

static void stale_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!g_host.link) {
		return;
	}
	g_host.link = false;
	nexus_screen_invalidate();
}

/* ---- the wire ----------------------------------------------------------- */

static void uart_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		uint8_t c;

		if (uart_fifo_read(dev, &c, 1) != 1) {
			break;
		}
		if (c == '\r') {
			continue;
		}
		if (c != '\n') {
			/* Truncate rather than wrap: the tail of an over-long
			 * line is dropped, the next line still parses. */
			if (g_rx_len < LINE_MAX - 1) {
				g_rx[g_rx_len++] = (char)c;
			}
			continue;
		}
		if (g_rx_len == 0) {
			continue;
		}
		g_rx[g_rx_len] = '\0';

		/*
		 * If the work item has not consumed the last line yet, this
		 * one replaces it. Dropping the older of two lines is right
		 * for a feed of current values - none of them are events, and
		 * the next update is a second away.
		 */
		memcpy(g_line, g_rx, (size_t)g_rx_len + 1U);
		atomic_set(&g_line_ready, 1);
		g_rx_len = 0;
		k_work_submit_to_queue(nexus_workq(), &g_parse);
	}
}

int nexus_host_link_init(void)
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
