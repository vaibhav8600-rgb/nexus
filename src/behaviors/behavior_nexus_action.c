/*
 * &nexus_action keymap behavior (Sections 13, 96-97).
 *
 * A key bound to this dispatches exactly the same logical action the physical
 * button produces, so `&nexus_action NEXUS_ACT_ROTATE` plays Tetris from the
 * keyboard with no game-specific code anywhere near the keymap (Section 42).
 *
 * It is a normal devicetree-declared ZMK behavior with one parameter, which is
 * what lets ZMK Studio represent and reassign it (Section 97).
 */

#define DT_DRV_COMPAT zmk_behavior_nexus_action

#include <nexus/action.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding,
		      struct zmk_behavior_binding_event event)
{
	ARG_UNUSED(event);

	/* Fire on press, not release: UI navigation should feel immediate, and
	 * dispatch only queues work - it never blocks the keymap thread. Press
	 * rather than dispatch, so a game can also ask whether it is held. */
	nexus_action_press((enum nexus_action)binding->param1);
	return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
		       struct zmk_behavior_binding_event event)
{
	ARG_UNUSED(event);

	/* Clears the held flag, and ends an auto-repeat if this action was one
	 * that repeats. The release was being discarded, which is why holding
	 * a direction key did exactly as much as tapping it once. */
	nexus_action_release((enum nexus_action)binding->param1);
	return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_nexus_action_api = {
	.binding_pressed = on_pressed,
	.binding_released = on_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
			CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
			&behavior_nexus_action_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
