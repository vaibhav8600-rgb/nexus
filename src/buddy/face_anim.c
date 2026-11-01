/*
 * Buddy's face, as numbers. See face_anim.h.
 *
 * Integer only. A float here would pull soft-float or libm into a build that
 * has neither, for a face whose every coordinate is a whole pixel anyway; the
 * smoothing runs in 1/16 px and is rounded once, when a frame is taken.
 */

#include "face_anim.h"

#include <string.h>

/*
 * Starting points, tuned for a 240px panel seen from across a desk. Columns:
 *
 *   w   h   r  spacing  y  lid out/in/bottom  gaze x/y
 *
 * Sad drops the OUTER lid and angry the INNER one: that difference alone is
 * most of what tells the two apart, since both frown.
 */
const struct face_params face_targets[FACE_EMOTIONS] = {
	[FACE_NEUTRAL] = { { 56, 64, 16, 44, 0, 0, 0, 0, 0, 0 },
			   FACE_C_ACCENT, FACE_M_LINE, FACE_X_NONE },
	[FACE_HAPPY] = { { 60, 60, 28, 44, -4, 0, 0, 30, 0, 0 },
			 FACE_C_ACCENT, FACE_M_SMILE, FACE_X_NONE },
	[FACE_SAD] = { { 54, 48, 16, 44, 10, 22, 0, 0, 0, 0 },
		       FACE_C_GLOW_B, FACE_M_FROWN, FACE_X_NONE },
	[FACE_ANGRY] = { { 58, 52, 12, 44, 4, 0, 26, 0, 0, 0 },
			 FACE_C_ERROR, FACE_M_FROWN, FACE_X_NONE },
	[FACE_SURPRISED] = { { 52, 78, 26, 44, -6, 0, 0, 0, 0, 0 },
			     FACE_C_ACCENT, FACE_M_O, FACE_X_NONE },
	[FACE_SLEEPY] = { { 60, 12, 6, 44, 18, 0, 0, 0, 0, 0 },
			  FACE_C_MUTED, FACE_M_NONE, FACE_X_ZZZ },
	[FACE_LOVE] = { { 56, 56, 16, 44, 0, 0, 0, 0, 0, 0 },
			FACE_C_ACCENT_ALT, FACE_M_SMILE, FACE_X_HEARTS },
	[FACE_THINKING] = { { 46, 54, 16, 44, -6, 14, 0, 0, 16, -14 },
			    FACE_C_WARNING, FACE_M_NONE, FACE_X_DOTS },
	[FACE_LISTENING] = { { 58, 70, 22, 44, -6, 0, 0, 0, 0, 0 },
			     FACE_C_GLOW_B, FACE_M_NONE, FACE_X_BARS },
};

const char *const face_emotion_names[FACE_EMOTIONS] = {
	"NEUTRAL", "HAPPY", "SAD", "ANGRY", "SURPRISED",
	"SLEEPY", "LOVE", "THINKING", "LISTENING",
};

int face_emotion_parse(const char *s)
{
	for (int i = 0; i < FACE_EMOTIONS; i++) {
		if (strcmp(s, face_emotion_names[i]) == 0) {
			return i;
		}
	}
	return -1;
}

/* A blink is 140 ms, eyelid down and up again. Shorter reads as a flicker;
 * longer reads as the face falling asleep. */
#define BLINK_MS 140
/* The gap before the second half of a double blink. */
#define BLINK_AGAIN_MS 70

uint32_t face_rand(uint32_t *state)
{
	uint32_t x = *state ? *state : 0x2545F491u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* A value in [lo, hi]. */
static int32_t rand_in(struct face_anim *a, int32_t lo, int32_t hi)
{
	return lo + (int32_t)(face_rand(&a->rng) % (uint32_t)(hi - lo + 1));
}

static bool due(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

/*
 * Exponential approach, as the fraction of the gap to close this step.
 * dt / (dt + tau) is 1 - e^(-dt/tau) to within a few percent across every
 * frame length the face runs at, without a table or an exp().
 */
static uint16_t gain(uint32_t dt, uint32_t tau)
{
	return (uint16_t)(dt * 256U / (dt + tau));
}

static int16_t approach(int16_t cur, int16_t tgt, uint16_t k)
{
	int32_t d = (int32_t)tgt - cur;
	int32_t step = d * k / 256;

	/* Always arrive: without this the last 1/16 px of every move is a
	 * gap the fraction rounds to nothing, and the face never settles. */
	if (step == 0 && d != 0) {
		step = d > 0 ? 1 : -1;
	}
	return (int16_t)(cur + step);
}

static int16_t round_q4(int32_t v)
{
	return (int16_t)(v >= 0 ? (v + 8) / 16 : -((-v + 8) / 16));
}

static void schedule_blink(struct face_anim *a, uint32_t now)
{
	int32_t gap = rand_in(a, 2500, 5000);

	/* The sleepy trait: late at night the lids are in less of a hurry. */
	if (a->slow_blinks) {
		gap = gap * 3 / 2;
	}
	a->next_blink = now + (uint32_t)gap;
}

void face_anim_init(struct face_anim *a, uint32_t seed, uint32_t now)
{
	memset(a, 0, sizeof(*a));
	a->rng = seed ? seed : 1u;
	a->target = face_targets[FACE_NEUTRAL];
	for (int i = 0; i < F_GEOM; i++) {
		a->q[i] = (int16_t)(a->target.g[i] * 16);
	}
	a->colour = a->colour_prev = a->target.colour;
	a->colour_mix = 255;
	a->saccades = true;
	schedule_blink(a, now);
	a->next_look = now + (uint32_t)rand_in(a, 900, 2700);
}

void face_anim_set(struct face_anim *a, enum face_emotion e, uint32_t now)
{
	if ((unsigned int)e >= FACE_EMOTIONS) {
		return;
	}

	const struct face_params *t = &face_targets[e];

	if (t->colour != a->colour) {
		a->colour_prev = a->colour;
		a->colour = t->colour;
		a->colour_mix = 0;
	}
	a->target = *t;

	/* Eyes that are shut, hearts or looking up in thought have nowhere
	 * to glance. */
	a->saccades = e != FACE_SLEEPY && e != FACE_LOVE && e != FACE_THINKING;
	if (!a->saccades) {
		a->look_x = a->look_y = 0;
	} else {
		a->next_look = now + (uint32_t)rand_in(a, 300, 900);
	}
}

void face_anim_blip(struct face_anim *a)
{
	a->talk = (uint8_t)rand_in(a, 153, 255);
}

void face_anim_step(struct face_anim *a, uint32_t now, uint32_t dt)
{
	/* A long gap - the screen was away, the queue was busy - is not a
	 * reason to jump. Catch up as though it were one slow frame. */
	if (dt > 200U) {
		dt = 200U;
	}

	uint16_t k = gain(dt, 90);

	for (int i = 0; i < F_GEOM; i++) {
		a->q[i] = approach(a->q[i], (int16_t)(a->target.g[i] * 16), k);
	}

	if (a->colour_mix < 255) {
		uint32_t m = a->colour_mix + dt * 255U / 150U;

		a->colour_mix = (uint8_t)(m > 255U ? 255U : m);
	}

	if (a->blinking) {
		if (due(now, a->blink_at + BLINK_MS)) {
			a->blinking = false;
			if (a->blink_again) {
				a->blink_again = false;
				a->next_blink = now + BLINK_AGAIN_MS;
			} else {
				schedule_blink(a, now);
			}
		}
	} else if (due(now, a->next_blink)) {
		a->blinking = true;
		a->blink_at = now;
		/* One in six is a double. Always single looks mechanical. */
		a->blink_again = face_rand(&a->rng) % 6U == 0U;
	}

	if (a->saccades) {
		if (due(now, a->next_look)) {
			a->look_x = (int16_t)rand_in(a, -12, 12);
			a->look_y = (int16_t)rand_in(a, -5, 5);
			a->next_look = now + (uint32_t)rand_in(a, 900, 2700);
		}
	} else {
		a->look_x = a->look_y = 0;
	}

	int16_t tx = (int16_t)(a->look_x * 16);
	int16_t ty = (int16_t)(a->look_y * 16);

	if (!due(now, a->ext_until)) {
		tx = (int16_t)(a->ext_x * 16);
		ty = (int16_t)(a->ext_y * 16);
	}

	/* Faster than the emotion tween: a glance is a flick, about 80 ms. */
	uint16_t kg = gain(dt, 35);

	a->lq_x = approach(a->lq_x, tx, kg);
	a->lq_y = approach(a->lq_y, ty, kg);

	if (a->talk) {
		uint32_t left = a->talk - (uint32_t)a->talk * gain(dt, 100) / 256U;

		a->talk = (uint8_t)(left < 8U ? 0U : left);
	}
}

void face_anim_frame(const struct face_anim *a, uint32_t now,
		     struct face_frame *f)
{
	memset(f, 0, sizeof(*f));

	for (int i = 0; i < F_GEOM; i++) {
		f->g[i] = round_q4(a->q[i]);
	}
	f->g[F_GAZE_X] = (int16_t)(f->g[F_GAZE_X] + round_q4(a->lq_x));
	f->g[F_GAZE_Y] = (int16_t)(f->g[F_GAZE_Y] + round_q4(a->lq_y));

	if (a->blinking) {
		/* Distance from the middle of the blink, so the lid closes and
		 * opens in the same time. The lids scale with the eye, or a sad
		 * lid deeper than a closing eye would swallow it whole. */
		int32_t half = BLINK_MS / 2;
		int32_t t = (int32_t)(now - a->blink_at);
		int32_t d = t < half ? half - t : t - half;

		if (d > half) {
			d = half;
		}

		int32_t h = f->g[F_H] * d / half;

		f->g[F_H] = (int16_t)(h < 4 ? 4 : h);
		f->g[F_LID_OUT] = (int16_t)(f->g[F_LID_OUT] * d / half);
		f->g[F_LID_IN] = (int16_t)(f->g[F_LID_IN] * d / half);
		f->g[F_LID_BOT] = (int16_t)(f->g[F_LID_BOT] * d / half);
	}

	if (a->bob) {
		/* The music trait: a gentle three-pixel nod, about 100 bpm. */
		uint32_t p = now % 600U;
		int32_t tri = (int32_t)(p < 300U ? p : 600U - p);

		f->g[F_Y] = (int16_t)(f->g[F_Y] + tri * 6 / 300 - 3);
	}

	f->colour = a->colour;
	f->colour_prev = a->colour_prev;
	f->colour_mix = a->colour_mix;

	uint8_t open = a->talk > a->talk_ext ? a->talk : a->talk_ext;

	if (open > 24U) {
		f->mouth = FACE_M_TALK;
		f->open = open;
	} else {
		f->mouth = a->target.mouth;
	}

	f->extra = a->target.extra;
	if (f->extra == FACE_X_DOTS || f->extra == FACE_X_BARS ||
	    f->extra == FACE_X_ZZZ) {
		f->phase = (uint8_t)((now / 250U) & 7U);
	}
}

void face_eye_rows(const struct face_frame *f, int *y0, int *y1)
{
	int cy = FACE_EYE_Y + f->g[F_Y] + f->g[F_GAZE_Y];
	int top, bot;

	if (f->extra == FACE_X_HEARTS) {
		int r = f->g[F_W] / 4;

		top = cy - r - r / 2;
		bot = cy + f->g[F_W] / 2;
	} else {
		top = cy - f->g[F_H] / 2;
		bot = top + f->g[F_H];
	}

	/* A pixel of slack either side for the anti-aliased edge. */
	top -= 1;
	bot += 1;

	if (f->extra == FACE_X_ZZZ || f->extra == FACE_X_DOTS) {
		if (FACE_EXTRA_Y0 < top) {
			top = FACE_EXTRA_Y0;
		}
		if (FACE_EXTRA_Y1 > bot) {
			bot = FACE_EXTRA_Y1;
		}
	}
	*y0 = top;
	*y1 = bot;
}

/* ---- speech ------------------------------------------------------------- */

uint16_t face_char_ms(char c)
{
	switch (c) {
	case ' ':
		return 110;
	case ',':
		return 160;
	case '.':
	case '!':
	case '?':
		return 220;
	default:
		return 55;
	}
}

uint16_t face_babble_hz(enum face_emotion e, char c)
{
	bool alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');

	if (!alnum) {
		return 0;
	}

	int32_t base;

	switch (e) {
	case FACE_HAPPY:
		base = 1900;
		break;
	case FACE_SAD:
		base = 1150;
		break;
	case FACE_SURPRISED:
		base = 2300;
		break;
	case FACE_SLEEPY:
		base = 1300;
		break;
	default:
		base = 1700;
		break;
	}
	return (uint16_t)(base + ((int32_t)((unsigned char)c * 37U % 9U) - 4) *
					 90);
}
