/*
 * Buddy: an animated face on the dongle, with moods, short lines and chirps
 * (CONFIG_NEXUS_BUDDY). docs/buddy.md has the whole of it.
 *
 * The face lives here; a brain, if there is one, lives on the PC or the phone
 * and sends lines - E for an emotion, S for something to say - over the host
 * link. With no brain the dongle runs a small mood engine of its own, so the
 * face is never just sitting there.
 *
 * Everything below compiles to nothing without CONFIG_NEXUS_BUDDY, so callers
 * stay unconditional.
 */
#ifndef NEXUS_BUDDY_H_
#define NEXUS_BUDDY_H_

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Who is driving the face. */
enum nexus_buddy_source {
	NEXUS_BUDDY_LOCAL = 0, /* the mood engine: no brain connected */
	NEXUS_BUDDY_USB,       /* a PC companion over the host link's USB */
	NEXUS_BUDDY_BLE,       /* a PC companion over the host's Bluetooth */
	NEXUS_BUDDY_PHONE,     /* NEXUS Remote                            */
};

/* BUDDY > SOUND. */
enum nexus_buddy_sound {
	NEXUS_BUDDY_SOUND_OFF = 0,
	NEXUS_BUDDY_SOUND_CHIRPS, /* a cue per emotion, now and then */
	NEXUS_BUDDY_SOUND_CHATTY, /* and a blip per letter spoken    */
	NEXUS_BUDDY_SOUND_MODES,
};

/* BUDDY > BRAIN: who may drive the face. AUTO is whoever spoke last. */
enum nexus_buddy_brain {
	NEXUS_BUDDY_BRAIN_AUTO = 0,
	NEXUS_BUDDY_BRAIN_PC,
	NEXUS_BUDDY_BRAIN_PHONE,
	NEXUS_BUDDY_BRAIN_LOCAL,
	NEXUS_BUDDY_BRAINS,
};

#if IS_ENABLED(CONFIG_NEXUS_BUDDY)

struct nexus_screen;
extern const struct nexus_screen nexus_screen_buddy_def;

/** Subscribe the mood engine. @return 0, or a negative errno. */
int nexus_buddy_init(void);

/**
 * A host-link line that may be Buddy's: E, S, V, L or B. On the NEXUS work
 * queue, from the parser.
 *
 * @return true if @p key is one of Buddy's, whatever its value - those lines
 *         belong to the face and never to the HOST screen.
 */
bool nexus_buddy_line(enum nexus_buddy_source from, char key, const char *val);

bool nexus_buddy_enabled(void);
void nexus_buddy_set_enabled(bool on);
enum nexus_buddy_sound nexus_buddy_sound_mode(void);
void nexus_buddy_set_sound_mode(enum nexus_buddy_sound mode);
enum nexus_buddy_brain nexus_buddy_brain_pin(void);
void nexus_buddy_set_brain_pin(enum nexus_buddy_brain pin);

/** "LOCAL", "PC" or "PHONE": who is driving the face right now. */
const char *nexus_buddy_brain_name(void);

/** Persist the three settings above. Called by nexus_settings_save(). */
int nexus_buddy_prefs_save(void);

/**
 * The display is about to blank for idleness. With
 * CONFIG_NEXUS_BUDDY_IDLE_FACE and a computer on USB, Buddy takes the screen
 * instead, asleep, and this returns true: do not blank.
 */
bool nexus_buddy_idle_face(void);

#endif /* CONFIG_NEXUS_BUDDY */

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_BUDDY_H_ */
