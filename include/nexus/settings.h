/*
 * Persisted user settings (Sections 62, 107).
 *
 * Sound, theme and brightness are editable at runtime, and without this they
 * reset to their Kconfig defaults every time the dongle is unplugged - which
 * makes the Settings screen feel like it did not work.
 *
 * Saving is explicit, from a SAVE row in the menu, for the reason Section 107
 * gives about high scores: settings writes hit flash, and cycling through
 * seven themes should not cost seven erase cycles. Changes apply immediately;
 * only committing them is deferred to a button press.
 */
#ifndef NEXUS_SETTINGS_H_
#define NEXUS_SETTINGS_H_

#include <zephyr/kernel.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True when something has changed since the last save or load. */
bool nexus_settings_dirty(void);

/** Note that a user-visible setting changed. */
void nexus_settings_touch(void);

/**
 * Write the current sound / theme / brightness to storage.
 *
 * @return 0 on success, -ENOTSUP when built without persistence, or a
 *         negative errno from the settings backend. Never fatal: a failed
 *         save leaves the running configuration untouched.
 */
int nexus_settings_save(void);

/**
 * Save a few seconds from now, restarting the clock on each call.
 *
 * For controls that change value many times per gesture - an encoder emits a
 * detent per click - where saving each step would burn a flash erase cycle per
 * click. The setting applies instantly either way; only the write waits until
 * you have stopped turning.
 */
void nexus_settings_save_deferred(void);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_SETTINGS_H_ */
