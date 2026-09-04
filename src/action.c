/*
 * Central action dispatcher (Sections 12-13).
 *
 * Three input sources - the physical button, a &nexus_action keymap key, and
 * the UI itself - all arrive here as the same logical verbs. Nothing downstream
 * can tell them apart, which is what makes the button context-sensitive
 * without a per-screen if-ladder.
 */

#include <nexus/action.h>
#include <nexus/settings.h>
#include <nexus/theme.h>
#include <nexus/sound.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "nexus_priv.h"

#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
#include <nexus/screen.h>
#endif

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* Actions arrive from ISR context (button) and from ZMK's behavior thread, but
 * must be handled where LVGL lives. A tiny queue is the whole hand-off. */
K_MSGQ_DEFINE(g_actions, sizeof(uint8_t), 8, 1);

static void handle(enum nexus_action action)
{
#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
	const struct nexus_screen *cur = nexus_screen_current();

	if (cur && cur->action && cur->action(action)) {
		return;
	}

	/* Fallbacks for anything the active screen did not claim. */
	switch (action) {
	case NEXUS_ACTION_HOME:
		nexus_sound_play(NEXUS_SOUND_BACK);
		nexus_screen_home();
		break;
	case NEXUS_ACTION_BACK:
		nexus_sound_play(NEXUS_SOUND_BACK);
		nexus_screen_pop();
		break;
#if IS_ENABLED(CONFIG_NEXUS_GAME_CENTER)
	case NEXUS_ACTION_GAME_CENTER:
		nexus_sound_play(NEXUS_SOUND_MENU_OPEN);
		nexus_screen_push(&nexus_screen_game_center_def);
		break;
#endif
	case NEXUS_ACTION_MENU:
		nexus_sound_play(NEXUS_SOUND_MENU_OPEN);
		nexus_screen_push(&nexus_screen_settings_def);
		break;
	case NEXUS_ACTION_THEME_NEXT:
	case NEXUS_ACTION_THEME_PREV:
		/*
		 * Direct access, no menu. Applies on the spot and schedules a
		 * save for once you stop turning - an encoder fires a detent
		 * per click, and a flash write per click is not a trade worth
		 * making (Section 107).
		 */
		nexus_theme_cycle(action == NEXUS_ACTION_THEME_NEXT ? 1 : -1);
		nexus_settings_save_deferred();
		nexus_screen_invalidate();
		nexus_sound_play(NEXUS_SOUND_SELECT);
		break;
	case NEXUS_ACTION_SAVE:
		/* Works from any screen, so you can commit a theme change from
		 * the keyboard without walking back to the SAVE row. */
		nexus_sound_play(nexus_settings_save() == 0
					 ? NEXUS_SOUND_MENU_SELECT
					 : NEXUS_SOUND_BACK);
		break;
	default:
		LOG_DBG("action %d unhandled on screen %s", action,
			cur ? cur->name : "(none)");
		break;
	}
#else
	ARG_UNUSED(action);
#endif
}

static void drain(struct k_work *work)
{
	ARG_UNUSED(work);

	uint8_t action;

	while (k_msgq_get(&g_actions, &action, K_NO_WAIT) == 0) {
		handle((enum nexus_action)action);
	}

#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
	/*
	 * Once, after the whole queue is drained rather than once per action,
	 * so holding a key down still costs one repaint per batch.
	 */
	nexus_screen_render_now();
#endif
}
static K_WORK_DEFINE(g_drain, drain);

void nexus_action_dispatch(enum nexus_action action)
{
	uint8_t a = (uint8_t)action;

	if (k_msgq_put(&g_actions, &a, K_NO_WAIT) != 0) {
		/* Dropping a queued action is strictly better than blocking an
		 * ISR or the keymap thread (Section 141-A). */
		LOG_WRN("action queue full, dropped %d", action);
		return;
	}

	k_work_submit_to_queue(nexus_workq(), &g_drain);
}

void nexus_action_button_event(bool long_press)
{
#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
	const struct nexus_screen *cur = nexus_screen_current();

	if (cur == NULL) {
		return;
	}

	nexus_action_dispatch(long_press ? cur->btn_long : cur->btn_short);
#else
	ARG_UNUSED(long_press);
#endif
}
