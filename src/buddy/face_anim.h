/*
 * Buddy's face, as numbers: what each emotion looks like, and how the face
 * moves from one to the next.
 *
 * Plain C, no Zephyr, the way tetris_core.c and host_lines.c are, so the
 * timing that makes the face feel alive - blinks, glances, the pace of a line
 * being spoken - is tested on the host in milliseconds rather than watched on
 * hardware in minutes. face.c draws what this decides; buddy.c decides when.
 *
 * Every time is a uint32_t of milliseconds from whatever clock the caller
 * keeps. Nothing here reads a clock of its own.
 */
#ifndef NEXUS_BUDDY_FACE_ANIM_H_
#define NEXUS_BUDDY_FACE_ANIM_H_

#include <stdbool.h>
#include <stdint.h>

/* ---- layout: shared by face.c, buddy.c, the test and render_ui.py -------- */

#define FACE_CX 120
#define FACE_EYE_Y 96    /* the eyes' centre line, before an emotion moves it */
#define FACE_MOUTH_Y 160
#define FACE_HEADER_H 28 /* name and brain, rows 0..27 */
#define FACE_EXTRA_Y0 28 /* ZZZ and the thinking dots, up and to the right */
#define FACE_EXTRA_Y1 80
#define FACE_MOUTH_Y0 140
#define FACE_MOUTH_Y1 184
#define FACE_BUBBLE_Y 188
#define FACE_BUBBLE_H 38

/* The longest line the bubble shows. The persona's own limit, and what the
 * font fits at caption size. */
#define FACE_LINE_MAX 24

enum face_emotion {
	FACE_NEUTRAL = 0,
	FACE_HAPPY,
	FACE_SAD,
	FACE_ANGRY,
	FACE_SURPRISED,
	FACE_SLEEPY,
	FACE_LOVE,
	FACE_THINKING,
	FACE_LISTENING,
	FACE_EMOTIONS,
};

/* A theme slot, never a colour: the face has to look right in all seven
 * palettes, DAYLIGHT's light ground included. face.c maps these. */
enum face_colour {
	FACE_C_ACCENT = 0,
	FACE_C_GLOW_B,
	FACE_C_ERROR,
	FACE_C_ACCENT_ALT,
	FACE_C_WARNING,
	FACE_C_MUTED,
};

enum face_mouth {
	FACE_M_NONE = 0,
	FACE_M_LINE,
	FACE_M_SMILE,
	FACE_M_FROWN,
	FACE_M_O,
	FACE_M_TALK,
};

enum face_extra {
	FACE_X_NONE = 0,
	FACE_X_HEARTS, /* the eyes themselves become hearts */
	FACE_X_DOTS,
	FACE_X_BARS,
	FACE_X_ZZZ,
};

/* The geometry, as an array so one loop tweens all of it. Pixels. */
enum {
	F_W = 0,   /* eye width                                         */
	F_H,       /* eye height                                        */
	F_R,       /* eye corner radius                                 */
	F_SPACING, /* centre to centre, halved                          */
	F_Y,       /* offset from FACE_EYE_Y                            */
	F_LID_OUT, /* top lid drop at the outer edge: sad              */
	F_LID_IN,  /* top lid drop at the inner edge: angry            */
	F_LID_BOT, /* bottom cut: the happy crescent                    */
	F_GAZE_X,  /* where the eyes rest when nothing else moves them  */
	F_GAZE_Y,
	F_GEOM,
};

struct face_params {
	int16_t g[F_GEOM];
	uint8_t colour; /* enum face_colour */
	uint8_t mouth;  /* enum face_mouth  */
	uint8_t extra;  /* enum face_extra  */
};

/* What the emotions look like at rest. Indexed by enum face_emotion. */
extern const struct face_params face_targets[FACE_EMOTIONS];

/* "NEUTRAL" ... "LISTENING", as the protocol and persona.json spell them. */
extern const char *const face_emotion_names[FACE_EMOTIONS];

/** @return the emotion named by @p s (upper case), or -1. */
int face_emotion_parse(const char *s);

/*
 * One frame, resolved to whole pixels: what face.c draws and what buddy.c
 * compares to decide whether anything needs repainting at all. Two frames that
 * compare equal draw identically.
 */
struct face_frame {
	int16_t g[F_GEOM]; /* blink, glance and bob already applied */
	uint8_t colour;
	uint8_t colour_prev;
	uint8_t colour_mix; /* 0 = all colour_prev, 255 = all colour */
	uint8_t mouth;
	uint8_t open;       /* FACE_M_TALK: how open, 0-255 */
	uint8_t extra;
	uint8_t phase;      /* the extras' animation step, 0-7 */
};

struct face_anim {
	struct face_params target;
	int16_t q[F_GEOM]; /* current geometry, 1/16 px */
	uint8_t colour, colour_prev, colour_mix;

	uint32_t rng;

	uint32_t blink_at;    /* the blink in progress started here...     */
	bool blinking;        /* ...if this is set                         */
	bool blink_again;     /* a double blink: one more after this one   */
	uint32_t next_blink;

	int16_t look_x, look_y; /* the glance being made, px             */
	int16_t lq_x, lq_y;     /* where the eyes are on the way, 1/16 px */
	uint32_t next_look;

	int16_t ext_x, ext_y;   /* a brain's L: look there until ext_until */
	uint32_t ext_until;

	uint8_t talk;     /* mouth opening from babble, decays by itself */
	uint8_t talk_ext; /* from a brain's V, held until it says 0      */

	/* Personality switches, set by buddy.c from the mood. */
	bool saccades;    /* glance around; off for sleepy, love, thinking */
	bool slow_blinks; /* late at night                                 */
	bool bob;         /* music is playing                              */
};

void face_anim_init(struct face_anim *a, uint32_t seed, uint32_t now);

/** Move toward @p e. The change is tweened, not cut. */
void face_anim_set(struct face_anim *a, enum face_emotion e, uint32_t now);

/** Advance @p dt ms to @p now. Cheap; call it every frame. */
void face_anim_step(struct face_anim *a, uint32_t now, uint32_t dt);

/** The frame to draw at @p now. */
void face_anim_frame(const struct face_anim *a, uint32_t now,
		     struct face_frame *f);

/** A babble blip: open the mouth, 0.6 to 1.0 of the way, at random. */
void face_anim_blip(struct face_anim *a);

/** Rows [y0, y1) the eyes and their extras cover in @p f. */
void face_eye_rows(const struct face_frame *f, int *y0, int *y1);

/* ---- speech ------------------------------------------------------------- */

/** How long to wait after revealing @p c before revealing the next one. */
uint16_t face_char_ms(char c);

/**
 * The babble blip for @p c, said with @p e: 0 for a silent character. Letters
 * and digits wobble around the emotion's own pitch, so the same line sounds
 * the same every time and a sad one sounds lower than a happy one.
 */
uint16_t face_babble_hz(enum face_emotion e, char c);

/** xorshift32, for anything that should look random but be repeatable. */
uint32_t face_rand(uint32_t *state);

#endif /* NEXUS_BUDDY_FACE_ANIM_H_ */
