/*
 * NEXUS platform bring-up.
 *
 * Everything optional is initialised here and every failure is logged and
 * survived. There is no code path in NEXUS that can stop ZMK from scanning,
 * pairing or typing - that is Requirement A, and it is the reason none of
 * these inits return an error to Zephyr (Sections 87, 113, 141-A).
 */

#include <nexus/nexus.h>
#include <nexus/sound.h>
#include <nexus/status.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "nexus_priv.h"

#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
#include <zmk/display.h>
#endif

LOG_MODULE_REGISTER(nexus, CONFIG_NEXUS_LOG_LEVEL);

static struct nexus_health g_health;

const struct nexus_health *nexus_health(void)
{
	return &g_health;
}

const char *nexus_board_name(void)
{
	return CONFIG_BOARD;
}

void nexus_health_set_display(bool ok) { g_health.display = ok; }
void nexus_health_set_backlight(bool ok) { g_health.backlight = ok; }
void nexus_health_set_buzzer(bool ok) { g_health.buzzer = ok; }
void nexus_health_set_button(bool ok) { g_health.button = ok; }
void nexus_health_set_games(bool ok) { g_health.games = ok; }

struct k_work_q *nexus_workq(void)
{
#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
	/* ZMK already runs one queue for the display. Reusing it is what keeps
	 * LVGL single-threaded and NEXUS thread-free (Sections 89, 122). */
	return zmk_display_work_q();
#else
	return &k_sys_work_q;
#endif
}

/*
 * Seeding reads ZMK's endpoint, keymap and HID state. Doing it from a short
 * delay rather than at SYS_INIT time avoids depending on the relative init
 * order of half a dozen ZMK subsystems, which is exactly the kind of thing
 * that changes between ZMK revisions.
 */
static void seed_work(struct k_work *work)
{
	ARG_UNUSED(work);
	nexus_status_seed();
}
static K_WORK_DELAYABLE_DEFINE(g_seed, seed_work);

static int nexus_init(void)
{
	LOG_INF("NEXUS v%s on %s", NEXUS_VERSION_STR, CONFIG_BOARD);

#if IS_ENABLED(CONFIG_NEXUS_SOUND)
	nexus_health_set_buzzer(nexus_buzzer_init() == 0);
	if (!g_health.buzzer) {
		LOG_WRN("no buzzer; sound disabled");
	}
#endif

#if IS_ENABLED(CONFIG_NEXUS_BUTTON)
	nexus_health_set_button(nexus_button_init() == 0);
	if (!g_health.button) {
		LOG_WRN("no action button; UI is display-only");
	}
#endif

	nexus_status_init();
	k_work_schedule_for_queue(nexus_workq(), &g_seed, K_MSEC(500));

	return 0;
}

SYS_INIT(nexus_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
