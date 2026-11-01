/*
 * Buddy's face as numbers: the tween, the blinks, the glances, the pace of a
 * line and where it all lands on the panel. Host-side, no Zephyr:
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/buddy \
 *      tests/buddy/test_face_anim.c src/buddy/face_anim.c
 */

#include <stdio.h>
#include <string.h>

#include "face_anim.h"

static int g_fail;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
			g_fail++;                                              \
		}                                                              \
	} while (0)

#define FRAME 50U

static void test_names(void)
{
	for (int i = 0; i < FACE_EMOTIONS; i++) {
		CHECK(face_emotion_parse(face_emotion_names[i]) == i);
	}
	CHECK(face_emotion_parse("happy") == -1); /* the caller folds */
	CHECK(face_emotion_parse("") == -1);
	CHECK(face_emotion_parse("GRUMPY") == -1);
}

/* Every emotion is reached exactly, and in well under a second. */
static void test_tween(void)
{
	struct face_anim a;
	uint32_t t = 0;

	face_anim_init(&a, 1234, t);
	for (int e = 0; e < FACE_EMOTIONS; e++) {
		face_anim_set(&a, (enum face_emotion)e, t);
		for (int i = 0; i < 20; i++) {
			t += FRAME;
			face_anim_step(&a, t, FRAME);
		}
		for (int g = 0; g < F_GEOM; g++) {
			CHECK(a.q[g] == face_targets[e].g[g] * 16);
		}
		CHECK(a.colour_mix == 255);
		CHECK(a.colour == face_targets[e].colour);
	}

	/* A long stall is one slow frame, not a jump past the target. */
	face_anim_set(&a, FACE_SURPRISED, t);
	t += 5000;
	face_anim_step(&a, t, 5000);
	CHECK(a.q[F_H] > face_targets[FACE_SLEEPY].g[F_H] * 16 - 1);
	CHECK(a.q[F_H] <= face_targets[FACE_SURPRISED].g[F_H] * 16);
}

/* Blinks: 2.5-5 s apart, 140 ms long, now and then a quick double. */
static void test_blinks(bool slow)
{
	struct face_anim a;
	uint32_t starts[64];
	int n = 0;
	bool was = false;
	int doubles = 0;
	int min_h = 1000;

	face_anim_init(&a, 99, 0);
	a.slow_blinks = slow;
	for (uint32_t t = 10; t < 300000 && n < 64; t += 10) {
		struct face_frame f;

		face_anim_step(&a, t, 10);
		face_anim_frame(&a, t, &f);
		if (a.blinking && !was) {
			starts[n++] = t;
		}
		if (a.blinking && f.g[F_H] < min_h) {
			min_h = f.g[F_H];
		}
		was = a.blinking;
	}
	CHECK(n > 20);
	CHECK(min_h == 4); /* closed is a line, never nothing */

	uint32_t hi = slow ? 7500 : 5000;

	for (int i = 1; i < n; i++) {
		uint32_t gap = starts[i] - starts[i - 1];

		if (gap <= 140 + 70 + 20) {
			doubles++;
			CHECK(gap >= 140 + 70);
		} else {
			CHECK(gap >= 2500 + 140);
			CHECK(gap <= hi + 140 + 20);
		}
	}
	CHECK(doubles > 0);
	CHECK(doubles < n / 2);
}

/* Glances stay small, and stop for the faces that have nowhere to look. */
static void test_glances(void)
{
	struct face_anim a;
	uint32_t t = 0;
	int moved = 0;

	face_anim_init(&a, 7, t);
	for (int i = 0; i < 4000; i++) {
		t += 10;
		face_anim_step(&a, t, 10);
		CHECK(a.look_x >= -12 && a.look_x <= 12);
		CHECK(a.look_y >= -5 && a.look_y <= 5);
		CHECK(a.lq_x >= -12 * 16 && a.lq_x <= 12 * 16);
		moved += a.lq_x != 0;
	}
	CHECK(moved > 0);

	face_anim_set(&a, FACE_SLEEPY, t);
	for (int i = 0; i < 100; i++) {
		t += 10;
		face_anim_step(&a, t, 10);
	}
	CHECK(a.lq_x == 0 && a.lq_y == 0);

	/* A brain's look wins while it lasts, then lets go. */
	face_anim_set(&a, FACE_NEUTRAL, t);
	a.saccades = false;
	a.ext_x = -20;
	a.ext_y = 20;
	a.ext_until = t + 2000;
	for (int i = 0; i < 50; i++) {
		t += 10;
		face_anim_step(&a, t, 10);
	}
	CHECK(a.lq_x == -20 * 16 && a.lq_y == 20 * 16);
	for (int i = 0; i < 300; i++) {
		t += 10;
		face_anim_step(&a, t, 10);
	}
	CHECK(a.lq_x == 0 && a.lq_y == 0);
}

/* The mouth opens on a blip and closes by itself; V holds it open. */
static void test_talk(void)
{
	struct face_anim a;
	struct face_frame f;
	uint32_t t = 0;

	face_anim_init(&a, 5, t);
	face_anim_blip(&a);
	CHECK(a.talk >= 153);
	face_anim_frame(&a, t, &f);
	CHECK(f.mouth == FACE_M_TALK && f.open == a.talk);
	for (int i = 0; i < 20; i++) {
		t += FRAME;
		face_anim_step(&a, t, FRAME);
	}
	CHECK(a.talk == 0);
	face_anim_frame(&a, t, &f);
	CHECK(f.mouth == FACE_M_LINE);

	a.talk_ext = 240;
	face_anim_frame(&a, t, &f);
	CHECK(f.mouth == FACE_M_TALK && f.open == 240);
}

/*
 * Where it lands: every emotion, glancing as far as it can, with a brain's
 * look on top, keeps its eyes between the header and the mouth and on the
 * panel. A face that clips into the name or off the edge is caught here and
 * not on hardware.
 */
static void test_bounds(void)
{
	for (int e = 0; e < FACE_EMOTIONS; e++) {
		for (int gx = -20; gx <= 20; gx += 40) {
			for (int gy = -20; gy <= 20; gy += 40) {
				struct face_frame f;
				int y0, y1;

				memset(&f, 0, sizeof(f));
				memcpy(f.g, face_targets[e].g, sizeof(f.g));
				f.g[F_GAZE_X] = (int16_t)(f.g[F_GAZE_X] + gx);
				f.g[F_GAZE_Y] = (int16_t)(f.g[F_GAZE_Y] + gy / 4);
				f.g[F_Y] = (int16_t)(f.g[F_Y] + 3); /* bob */
				f.extra = face_targets[e].extra;
				face_eye_rows(&f, &y0, &y1);
				CHECK(y0 >= FACE_HEADER_H);
				CHECK(y1 <= FACE_MOUTH_Y0);

				int half = f.g[F_W] / 2;
				int left = FACE_CX - f.g[F_SPACING] + f.g[F_GAZE_X] - half;
				int right = FACE_CX + f.g[F_SPACING] + f.g[F_GAZE_X] + half;

				CHECK(left >= 0 && right <= 240);
			}
		}
	}
}

static void test_speech(void)
{
	CHECK(face_char_ms('A') == 55);
	CHECK(face_char_ms(' ') == 110);
	CHECK(face_char_ms('.') == 220);
	CHECK(face_char_ms('!') == 220);
	CHECK(face_char_ms('?') == 220);

	/* A 24-character line is said in a few seconds, not a few tenths. */
	const char *line = "WHERE DID RIGHT GO?";
	uint32_t total = 0;

	for (const char *c = line; *c; c++) {
		total += face_char_ms(*c);
	}
	CHECK(total > 800 && total < 3000);

	CHECK(face_babble_hz(FACE_HAPPY, ' ') == 0);
	CHECK(face_babble_hz(FACE_HAPPY, '.') == 0);
	for (char c = 'A'; c <= 'Z'; c++) {
		uint16_t hz = face_babble_hz(FACE_HAPPY, c);

		CHECK(hz >= 1900 - 360 && hz <= 1900 + 360);
		/* The same letter, the same pitch, every time. */
		CHECK(hz == face_babble_hz(FACE_HAPPY, c));
		CHECK(face_babble_hz(FACE_SAD, c) < hz);
	}
}

int main(void)
{
	test_names();
	test_tween();
	test_blinks(false);
	test_blinks(true);
	test_glances();
	test_talk();
	test_bounds();
	test_speech();

	if (g_fail) {
		printf("%d check(s) failed\n", g_fail);
		return 1;
	}
	printf("face_anim: all checks passed\n");
	return 0;
}
