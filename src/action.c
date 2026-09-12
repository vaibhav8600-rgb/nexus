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

/*
 * Auto-repeat for held movement keys.
 *
 * ZMK fires a behavior once per press; nothing repeats it. So holding K to
 * soft-drop, or J to slide a paddle, did exactly as much as tapping once -
 * you had to machine-gun the key to cross the screen.
 *
 * Only movement repeats. MENU, SELECT, HOME and friends would open a screen
 * per tick, which is not a feature.
 */
static bool action_repeats(enum nexus_action a)
{
	switch (a) {
	case NEXUS_ACTION_LEFT:
	case NEXUS_ACTION_RIGHT:
	case NEXUS_ACTION_UP:
	case NEXUS_ACTION_DOWN:
		return true;
	default:
		/* Not DROP: it is a hard drop in Tetris and a launch in
		 * Breakout, both one-shot by definition. */
		return false;
	}
}

static enum nexus_action g_held;

/*
 * One bit per action whose key is down. Written from the keymap's thread,
 * read from the display work queue by a game's tick, hence atomic.
 */
static atomic_t g_down;
BUILD_ASSERT(NEXUS_ACT_THEME_PREV < 32,
	     "g_down is one word - widen it before adding action 32");

/* Forward-declared so the handler can reschedule its own work item, which it
 * cannot do if the item is defined after it. */
static void repeat_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_repeat_work, repeat_fn);

static void repeat_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_held == NEXUS_ACTION_NONE) {
		return;
	}
	nexus_action_dispatch(g_held);
	k_work_reschedule_for_queue(nexus_workq(), &g_repeat_work,
				    K_MSEC(CONFIG_NEXUS_ACTION_REPEAT_MS));
}

void nexus_action_press(enum nexus_action action)
{
	if ((unsigned int)action < 32U) {
		atomic_set_bit(&g_down, action);
	}
	nexus_action_dispatch(action);
}

bool nexus_action_held(enum nexus_action action)
{
	return (unsigned int)action < 32U && atomic_test_bit(&g_down, action);
}

void nexus_action_release(enum nexus_action action)
{
	if ((unsigned int)action < 32U) {
		atomic_clear_bit(&g_down, action);
	}

	/*
	 * Only the key that started the repeat may stop it. Releasing J while
	 * L is already held would otherwise cancel L's repeat and leave the
	 * paddle stuck mid-slide, which is exactly what a player does when
	 * they change direction in a hurry.
	 */
	if (g_held == action) {
		g_held = NEXUS_ACTION_NONE;
		k_work_cancel_delayable(&g_repeat_work);
	}
}

void nexus_action_dispatch(enum nexus_action action)
{
	uint8_t a = (uint8_t)action;

	if (action == NEXUS_ACTION_NONE) {
		/* A screen that declares no gesture for this input. Silently
		 * doing nothing is the whole point of the sentinel. */
		return;
	}

	if (action_repeats(action) && g_held != action) {
		/*
		 * Arm on the first press, with a longer delay before the
		 * repeat starts than between repeats: without that gap a
		 * single deliberate tap turns into two or three moves and the
		 * game feels like it is fighting you.
		 */
		g_held = action;
		k_work_reschedule_for_queue(nexus_workq(), &g_repeat_work,
					    K_MSEC(CONFIG_NEXUS_ACTION_REPEAT_DELAY_MS));
	}

	if (k_msgq_put(&g_actions, &a, K_NO_WAIT) != 0) {
		/* Dropping a queued action is strictly better than blocking an
		 * ISR or the keymap thread (Section 141-A). */
		LOG_WRN("action queue full, dropped %d", action);
		return;
	}

	k_work_submit_to_queue(nexus_workq(), &g_drain);
}

#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
/*
 * Double-tap, and why the single tap has to wait for it.
 *
 * A second tap can only be recognised by NOT acting on the first one until
 * the window closes. That is real latency on the primary gesture, so it is
 * paid only on screens that declare btn_double - everywhere else a tap still
 * dispatches the instant the button comes up.
 *
 * g_tap_armed is a separate flag and not "g_tap_pending != 0", which is what
 * this was and why the Game Center's button did nothing at all:
 * NEXUS_ACT_SELECT is 0, so storing btn_short as its own pending-flag stored
 * a falsy value, the timeout never fired the tap, and a second tap re-armed
 * instead of counting as a double. An enum whose first member is 0 cannot
 * double as a sentinel.
 */
static bool g_tap_armed;
static enum nexus_action g_tap_pending;
/*
 * Which screen armed it. A tap held for the double-tap window can outlive the
 * screen that started it - a half connecting, a game ending - and firing a
 * stale SELECT into whatever screen arrived next is worse than losing the
 * tap.
 */
static const struct nexus_screen *g_tap_screen;

static void tap_disarm(void)
{
	g_tap_armed = false;
	g_tap_screen = NULL;
}

static void tap_timeout(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_tap_armed && nexus_screen_current() == g_tap_screen) {
		enum nexus_action a = g_tap_pending;

		tap_disarm();
		nexus_action_dispatch(a);
		return;
	}
	tap_disarm();
}
static K_WORK_DELAYABLE_DEFINE(g_tap_work, tap_timeout);
#endif

void nexus_action_button_event(bool long_press)
{
#if IS_ENABLED(CONFIG_NEXUS_DISPLAY)
	const struct nexus_screen *cur = nexus_screen_current();

	if (cur == NULL) {
		return;
	}

	if (long_press) {
		/* A hold cancels a tap that was still waiting for its pair;
		 * otherwise releasing from a hold could fire both. */
		k_work_cancel_delayable(&g_tap_work);
		tap_disarm();
		nexus_action_dispatch(cur->btn_long);
		return;
	}

	if (cur->btn_double == NEXUS_ACTION_NONE) {
		/* No double on this screen: dispatch now and pay no latency.
		 * Every screen that never thought about the gesture lands
		 * here, because an omitted field is NEXUS_ACTION_NONE. */
		nexus_action_dispatch(cur->btn_short);
		return;
	}

	if (g_tap_armed && g_tap_screen == cur) {
		k_work_cancel_delayable(&g_tap_work);
		tap_disarm();
		nexus_action_dispatch(cur->btn_double);
		return;
	}

	g_tap_armed = true;
	g_tap_pending = cur->btn_short;
	g_tap_screen = cur;
	k_work_reschedule_for_queue(nexus_workq(), &g_tap_work,
				    K_MSEC(CONFIG_NEXUS_BUTTON_DOUBLE_MS));
#else
	ARG_UNUSED(long_press);
#endif
}
