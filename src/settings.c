/*
 * Persisted user settings. See include/nexus/settings.h.
 *
 * One key, one small struct. Three separate keys would mean three flash
 * writes for one SAVE and three chances to come back half-applied.
 */

#include <nexus/display.h>
#if IS_ENABLED(CONFIG_NEXUS_GAMES)
#include <nexus/game.h>
#endif
#include <nexus/settings.h>
#include <nexus/sound.h>
#include <nexus/theme.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include "nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#define NEXUS_PREFS_KEY "nexus/ui/prefs"
/*
 * Bumped for game_speed. prefs_set() ignores a struct whose version does not
 * match and falls back to defaults, so an older saved blob is discarded
 * rather than read with the fields shifted - the settings are worth less than
 * the chance of applying a garbage brightness.
 */
#define NEXUS_PREFS_VERSION 2

struct nexus_prefs {
	uint8_t version;
	uint8_t sound;      /* 0 or 1        */
	uint8_t theme;      /* index         */
	uint8_t brightness; /* 0-100 percent */
	uint8_t game_speed; /* 1-5           */
};

static bool g_dirty;

#if IS_ENABLED(CONFIG_NEXUS_SETTINGS_PERSIST)
static void autosave_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_autosave, autosave_fn);

static void autosave_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_dirty) {
		nexus_settings_save();
	}
}

void nexus_settings_save_deferred(void)
{
	g_dirty = true;
	/* RESCHEDULE, not schedule: each click should push the deadline out,
	 * so a long spin writes once at the end rather than once at the start
	 * and then again for everything after it. */
	k_work_reschedule_for_queue(nexus_workq(), &g_autosave,
				    K_MSEC(CONFIG_NEXUS_SETTINGS_AUTOSAVE_MS));
}
#else
void nexus_settings_save_deferred(void)
{
	g_dirty = true;
}
#endif

bool nexus_settings_dirty(void)
{
	return g_dirty;
}

void nexus_settings_touch(void)
{
	g_dirty = true;
}

#if IS_ENABLED(CONFIG_NEXUS_SETTINGS_PERSIST)

static void apply(const struct nexus_prefs *p)
{
	nexus_sound_set_enabled(p->sound != 0);
	nexus_theme_set_index(p->theme);
#if IS_ENABLED(CONFIG_NEXUS_GAMES)
	nexus_game_speed_set(p->game_speed);
#endif

	/*
	 * Only records the level. The panel may not exist yet - settings load
	 * during ZMK init, well before the first frame - so the backlight HAL
	 * stores it and applies it when it comes up.
	 */
	nexus_display_backlight_set(p->brightness);
}

static int prefs_set(const char *name, size_t len, settings_read_cb read_cb,
		     void *cb_arg)
{
	struct nexus_prefs p;

	if (!settings_name_steq(name, "prefs", NULL)) {
		return 0; /* not ours; do not fail the whole settings load */
	}
	if (len != sizeof(p)) {
		LOG_WRN("saved settings are %u bytes, expected %u - ignoring",
			(unsigned int)len, (unsigned int)sizeof(p));
		return 0;
	}
	if (read_cb(cb_arg, &p, sizeof(p)) < 0) {
		return -EIO;
	}
	if (p.version != NEXUS_PREFS_VERSION) {
		/* A future or older layout. Defaults are always safe, so fall
		 * back rather than apply fields that may have moved. */
		LOG_WRN("settings version %u != %u - using defaults", p.version,
			NEXUS_PREFS_VERSION);
		return 0;
	}

	apply(&p);
	g_dirty = false;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nexus_ui, "nexus/ui", NULL, prefs_set, NULL,
			       NULL);

int nexus_settings_save(void)
{
	struct nexus_prefs p = {
		.version = NEXUS_PREFS_VERSION,
		.sound = nexus_sound_enabled() ? 1 : 0,
		.theme = nexus_theme_index(),
		.brightness = nexus_display_backlight_level(),
#if IS_ENABLED(CONFIG_NEXUS_GAMES)
		.game_speed = nexus_game_speed(),
#endif
	};

	int ret = settings_save_one(NEXUS_PREFS_KEY, &p, sizeof(p));

	if (ret) {
		/* Requirement A: a failed write is a failed write, not a
		 * reason to take anything else down. */
		LOG_WRN("settings save failed (%d); running config unchanged",
			ret);
		return ret;
	}

	g_dirty = false;
	return 0;
}

#else /* built without persistence */

int nexus_settings_save(void)
{
	return -ENOTSUP;
}

#endif /* CONFIG_NEXUS_SETTINGS_PERSIST */
