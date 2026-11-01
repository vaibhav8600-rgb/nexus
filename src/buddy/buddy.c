/*
 * Buddy: the face's model, its moods and its screen. See include/nexus/buddy.h
 * and docs/buddy.md.
 *
 * Three things decide what the face shows, in this order:
 *
 *   1. What the keyboard is doing that matters more than anything said about
 *      it: a half that dropped out, a battery nearly flat.
 *   2. A brain - the PC or the phone - while its lease lasts. Every line it
 *      sends renews the lease; silence for CONFIG_NEXUS_BUDDY_LEASE_S hands
 *      the face back.
 *   3. The local mood engine: typing fast, a new best, going quiet, coming
 *      back, the hour.
 *
 * Nothing here can reach typing. The mood engine reads the status model like
 * every widget does, the face draws on the NEXUS work queue like every screen
 * does, and a brain's lines arrive through the host link's parser, which runs
 * on that queue too. No thread, no heap, no ISR.
 */

#include <nexus/action.h>
#include <nexus/buddy.h>
#include <nexus/gfx.h>
#include <nexus/screen.h>
#include <nexus/settings.h>
#include <nexus/sound.h>
#include <nexus/status.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK)
#include <nexus/host.h>
#endif

#include "../nexus_priv.h"
#include "buddy_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#define S_MS(s) ((int64_t)(s) * MSEC_PER_SEC)
#define M_MS(m) S_MS((m) * 60)

/* The persona's never-list says no more than once every few minutes
 * unprompted. A half dropping out is not chatter, so those skip it. */
#define REMARK_GAP M_MS(3)
#define CUE_GAP M_MS(3)
#define STREAK_GAP M_MS(10)
#define IDLE_SLEEPY M_MS(5)
#define BUBBLE_HOLD S_MS(4)
/* A cue that waits for the typing to stop gives up after this. */
#define CUE_PATIENCE S_MS(10)
/* Frames: 20 per second with a computer on USB, 10 on a charger or a
 * battery, and two while the panel is blanked and nothing can be seen. */
#define FRAME_MS_USB 50
#define FRAME_MS_BATT 100
#define FRAME_MS_DARK 500

/* ---- state -------------------------------------------------------------- */

static struct face_anim g_anim;
static struct face_frame g_frame; /* what draw() paints        */
static struct face_frame g_drawn; /* what the panel last showed */
static int g_eye_y0, g_eye_y1;
static int64_t g_last_step;

static bool g_enabled = true;
static uint8_t g_sound = NEXUS_BUDDY_SOUND_CHIRPS;
static uint8_t g_pin = NEXUS_BUDDY_BRAIN_AUTO;

static enum nexus_buddy_source g_brain = NEXUS_BUDDY_LOCAL;
static int64_t g_lease_until;
static enum face_emotion g_brain_mood = FACE_NEUTRAL;
static bool g_brain_spoke;

static char g_line[FACE_LINE_MAX + 1];
static uint8_t g_shown;
static int64_t g_next_char;
static int64_t g_line_until;
static enum face_emotion g_line_mood;

static enum face_emotion g_mood = FACE_NEUTRAL;
static enum face_emotion g_flash;
static int64_t g_flash_until;

static uint16_t g_wpm_avg; /* 1/16 wpm */
static uint8_t g_wpm_best;
static int64_t g_best_at;
static int64_t g_streak_since;
static int64_t g_streak_said = -STREAK_GAP;

static int64_t g_idle_since; /* 0 while someone is typing */
static bool g_half_seen[2];
static bool g_half_down[2];
static bool g_batt_said;
static int64_t g_remarked = -REMARK_GAP;
static uint32_t g_late_said = UINT32_MAX;
static int64_t g_yawned;
static bool g_greeted;
static bool g_idle_pushed;

static int8_t g_cue = -1; /* the emotion waiting to chirp, or -1 */
static int64_t g_cue_until;
static int64_t g_cued = -CUE_GAP;

static bool g_header_dirty;

static void anim_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_anim_work, anim_fn);

static bool on_screen(void)
{
	return nexus_screen_current() == &nexus_screen_buddy_def;
}

/* ---- words -------------------------------------------------------------- */

/* The bubble's font is ASCII 32..90: lower case folds up, the rest goes. */
static void fold(char *dst, const char *src, size_t max)
{
	size_t n = 0;

	for (; src && *src && n + 1U < max; src++) {
		char c = *src;

		if (c >= 'a' && c <= 'z') {
			c -= 'a' - 'A';
		}
		if ((unsigned char)c >= 32U && (unsigned char)c <= 90U) {
			dst[n++] = c;
		}
	}
	dst[n] = '\0';
}

static void say(const char *text, enum face_emotion mood)
{
	fold(g_line, text, sizeof(g_line));
	g_shown = 0;
	g_next_char = k_uptime_get();
	g_line_until = 0;
	g_line_mood = mood;
}

/* "HI " + owner + "!", or just "HI!" with no owner set. */
static void say_owner(const char *pre, const char *post, enum face_emotion e)
{
	char buf[FACE_LINE_MAX + 1];
	size_t n = 0;
	const char *parts[] = { pre, CONFIG_NEXUS_BUDDY_OWNER, post };

	for (size_t i = 0; i < ARRAY_SIZE(parts); i++) {
		for (const char *s = parts[i]; *s && n < FACE_LINE_MAX; s++) {
			buf[n++] = *s;
		}
	}
	buf[n] = '\0';
	say(buf, e);
}

/* Things the face says on its own, with the mood to say them in. */
static void remark(const char *text, enum face_emotion e, bool urgent)
{
	int64_t now = k_uptime_get();

	/* A brain is talking, or Buddy is asleep: the mood still shows, the
	 * words do not. */
	if (!g_enabled || g_brain != NEXUS_BUDDY_LOCAL) {
		return;
	}
	if (!urgent && now - g_remarked < REMARK_GAP) {
		return;
	}
	g_remarked = now;
	g_flash = e;
	g_flash_until = now + S_MS(3);
	say(text, e);
}

/* ---- sound -------------------------------------------------------------- */

BUILD_ASSERT(NEXUS_SOUND_BUDDY_LISTENING - NEXUS_SOUND_BUDDY_NEUTRAL ==
		     FACE_LISTENING - FACE_NEUTRAL,
	     "Buddy's cues follow enum face_emotion");

/* The host's clock is the only one the dongle has; without it, never. */
static bool hour_in(uint32_t from, uint32_t to)
{
#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK)
	uint32_t clock = nexus_host_clock();

	if (clock != UINT32_MAX) {
		uint32_t h = clock / 3600U;

		return from <= to ? (h >= from && h < to) : (h >= from || h < to);
	}
#else
	ARG_UNUSED(from);
	ARG_UNUSED(to);
#endif
	return false;
}

static bool quiet_hours(void)
{
	return hour_in(CONFIG_NEXUS_BUDDY_QUIET_START, CONFIG_NEXUS_BUDDY_QUIET_END);
}

/* ZMK's WPM decays for a few seconds after the last key, which is exactly
 * the pause a chirp should wait for. */
static bool typing(void)
{
	return nexus_status_get()->wpm > 0;
}

/* A cue for @p e. @p prompted: the user or a brain asked, so no rate limit.
 * Settling back to neutral on its own is not news, so only a brain's
 * neutral is heard. */
static void cue(enum face_emotion e, bool prompted)
{
	int64_t now = k_uptime_get();

	if ((e == FACE_NEUTRAL && !prompted) || !g_enabled ||
	    g_sound == NEXUS_BUDDY_SOUND_OFF) {
		return;
	}
	if (!prompted) {
		if (now - g_cued < CUE_GAP) {
			return;
		}
		g_cued = now;
	}
	g_cue = (int8_t)e;
	g_cue_until = now + CUE_PATIENCE;
}

static void cue_pump(int64_t now)
{
	if (g_cue < 0) {
		return;
	}
	if (now > g_cue_until || quiet_hours() ||
	    g_sound == NEXUS_BUDDY_SOUND_OFF) {
		g_cue = -1;
		return;
	}
	if (typing()) {
		return;
	}
	nexus_sound_play((enum nexus_sound)(NEXUS_SOUND_BUDDY_NEUTRAL + g_cue));
	g_cue = -1;
}

static void babble(char c)
{
	if (g_sound != NEXUS_BUDDY_SOUND_CHATTY || quiet_hours() || typing()) {
		return;
	}

	uint16_t hz = face_babble_hz(g_line_mood, c == '?' ? 'A' : c);

	if (c == '?') {
		/* A question goes up at the end. */
		nexus_sound_blip(hz, 90, (uint16_t)(hz + 500));
	} else if (hz) {
		nexus_sound_blip(hz, 45, 0);
	}
}

/* ---- the brain ----------------------------------------------------------- */

static bool accepts(enum nexus_buddy_source from)
{
	switch (g_pin) {
	case NEXUS_BUDDY_BRAIN_PC:
		return from == NEXUS_BUDDY_USB || from == NEXUS_BUDDY_BLE;
	case NEXUS_BUDDY_BRAIN_PHONE:
		return from == NEXUS_BUDDY_PHONE;
	case NEXUS_BUDDY_BRAIN_LOCAL:
		return false;
	default:
		return true;
	}
}

static void take(enum nexus_buddy_source from, int64_t now)
{
	if (g_brain != from) {
		g_brain = from;
		g_header_dirty = true;
	}
	g_lease_until = now + S_MS(CONFIG_NEXUS_BUDDY_LEASE_S);
}

static void release(int64_t now)
{
	if (g_brain == NEXUS_BUDDY_LOCAL) {
		return;
	}
	/* A brain that was mid-conversation does not just vanish: a moment
	 * of sad, then the face is its own again. */
	if (g_brain_spoke) {
		g_flash = FACE_SAD;
		g_flash_until = now + 1500;
	}
	g_brain = NEXUS_BUDDY_LOCAL;
	g_brain_spoke = false;
	g_brain_mood = FACE_NEUTRAL;
	g_anim.talk_ext = 0;
	g_header_dirty = true;
}

/* "-12,8" into two values in [-20, 20]. */
static bool parse_look(const char *s, int *dx, int *dy)
{
	int v[2];

	for (int i = 0; i < 2; i++) {
		bool neg = *s == '-';
		int n = 0;

		s += neg;
		if (*s < '0' || *s > '9') {
			return false;
		}
		while (*s >= '0' && *s <= '9' && n <= 20) {
			n = n * 10 + (*s++ - '0');
		}
		if (n > 20) {
			return false;
		}
		v[i] = neg ? -n : n;
		if (i == 0 && *s++ != ',') {
			return false;
		}
	}
	if (*s) {
		return false;
	}
	*dx = v[0];
	*dy = v[1];
	return true;
}

bool nexus_buddy_line(enum nexus_buddy_source from, char key, const char *val)
{
	if (key != 'E' && key != 'S' && key != 'V' && key != 'L' && key != 'B') {
		return false;
	}
	if (!g_enabled || !accepts(from)) {
		return true;
	}

	int64_t now = k_uptime_get();

	switch (key) {
	case 'B':
		if (val[0] == '0' && val[1] == '\0') {
			if (g_brain == from) {
				release(now);
			}
		} else if (val[0] == '1' && val[1] == '\0') {
			take(from, now);
		}
		break;
	case 'E': {
		char name[12];
		int e;

		fold(name, val, sizeof(name));
		e = face_emotion_parse(name);
		if (e < 0) {
			break; /* a newer brain's emotion: not an error */
		}
		take(from, now);
		if (g_brain_mood != (enum face_emotion)e) {
			g_brain_mood = (enum face_emotion)e;
			cue(g_brain_mood, true);
		}
		if (e == FACE_LOVE) {
			/* The shy trait: praise makes it look down. */
			g_anim.ext_x = 0;
			g_anim.ext_y = 8;
			g_anim.ext_until = (uint32_t)now + 1500U;
		}
		break;
	}
	case 'S':
		take(from, now);
		say(val, g_brain_mood);
		g_brain_spoke = true;
		break;
	case 'V':
		if (val[0] >= '0' && val[0] <= '4' && val[1] == '\0') {
			take(from, now);
			g_anim.talk_ext = (uint8_t)((val[0] - '0') * 60);
		}
		break;
	case 'L': {
		int dx, dy;

		if (parse_look(val, &dx, &dy)) {
			take(from, now);
			g_anim.ext_x = (int16_t)dx;
			g_anim.ext_y = (int16_t)dy;
			g_anim.ext_until = (uint32_t)now + 2000U;
		}
		break;
	}
	default:
		break;
	}
	return true;
}

const char *nexus_buddy_brain_name(void)
{
	switch (g_brain) {
	case NEXUS_BUDDY_USB:
	case NEXUS_BUDDY_BLE:
		return "PC";
	case NEXUS_BUDDY_PHONE:
		return "PHONE";
	default:
		return "LOCAL";
	}
}

/* ---- the mood engine ----------------------------------------------------- */

static const char *const k_half_gone[2] = { "WHERE DID LEFT GO?",
					    "WHERE DID RIGHT GO?" };

static void on_halves(const struct nexus_status *st)
{
	enum nexus_link_state s[2] = { st->link_left, st->link_right };

	for (int i = 0; i < 2; i++) {
		bool up = s[i] == NEXUS_LINK_CONNECTED;

		if (up) {
			g_half_seen[i] = true;
			if (g_half_down[i]) {
				g_half_down[i] = false;
				remark("YAY, BACK!", FACE_HAPPY, true);
				cue(FACE_HAPPY, true);
			}
		} else if (g_half_seen[i] && !g_half_down[i]) {
			/* Only a half that was here can go missing: at boot
			 * neither has arrived yet, and that is not sad. */
			g_half_down[i] = true;
			remark(k_half_gone[i], FACE_SAD, true);
			cue(FACE_SAD, true);
		}
	}
}

static void on_wpm(uint8_t wpm, int64_t now)
{
	if (wpm == 0) {
		g_streak_since = 0;
		return;
	}

	/* The owner's own pace, a slow average over every typing second. */
	g_wpm_avg = (uint16_t)(g_wpm_avg + ((int32_t)wpm * 16 - g_wpm_avg) / 32);

	if (wpm > g_wpm_best) {
		/* Only a best that stood for two minutes is worth beating:
		 * every second of a burst is otherwise a new record. */
		if (g_wpm_best >= 30 && now - g_best_at > M_MS(2)) {
			remark("NEW BEST!", FACE_SURPRISED, false);
			cue(FACE_SURPRISED, false);
		}
		g_wpm_best = wpm;
		g_best_at = now;
	}

	bool fast = wpm >= 25 && (uint32_t)wpm * 16U * 10U > g_wpm_avg * 13U;

	if (!fast) {
		g_streak_since = 0;
	} else if (!g_streak_since) {
		g_streak_since = now;
	} else if (now - g_streak_since >= S_MS(20) &&
		   now - g_streak_said >= STREAK_GAP) {
		g_streak_said = now;
		g_streak_since = 0;
		remark("NICE STREAK!", FACE_HAPPY, false);
		cue(FACE_HAPPY, false);
	}
}

static uint8_t lowest_battery(const struct nexus_status *st)
{
	uint8_t lo = NEXUS_BATTERY_UNKNOWN;

	if (st->battery_left <= 100) {
		lo = st->battery_left;
	}
	if (st->battery_right <= 100 && st->battery_right < lo) {
		lo = st->battery_right;
	}
	return lo;
}

static void on_status(const struct nexus_status *st, uint32_t changed)
{
	int64_t now = k_uptime_get();

	if (changed & NEXUS_STATUS_LINKS) {
		on_halves(st);
	}
	if (changed & NEXUS_STATUS_WPM) {
		on_wpm(st->wpm, now);
	}
	if (changed & NEXUS_STATUS_BATTERY) {
		uint8_t lo = lowest_battery(st);

		if (lo < 15 && !g_batt_said) {
			g_batt_said = true;
			remark("LOW BATTERY", FACE_SLEEPY, true);
		} else if (lo != NEXUS_BATTERY_UNKNOWN && lo >= 20) {
			g_batt_said = false;
		}
	}
	if (changed & NEXUS_STATUS_ACTIVE) {
		if (st->user_active) {
			if (g_idle_since && now - g_idle_since > IDLE_SLEEPY) {
				/* Woken up: a surprised blink first. */
				g_flash = FACE_SURPRISED;
				g_flash_until = now + 800;
			}
			g_idle_since = 0;
			if (g_idle_pushed && on_screen()) {
				g_idle_pushed = false;
				nexus_screen_pop();
			}
		} else {
			g_idle_since = now;
		}
	}
}

static bool late(void)
{
	return hour_in(23, 5);
}

static bool music(void)
{
#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK)
	const struct nexus_host *h = nexus_host();

	return h->link && h->now_playing[0] && !h->paused;
#else
	return false;
#endif
}

static void night(int64_t now)
{
	if (!late()) {
		return;
	}
#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK)
	uint32_t day = nexus_host_day();

	if (day != g_late_said) {
		g_late_said = day;
		remark("ITS LATE... BED?", FACE_SLEEPY, false);
	}
#endif
	if (now - g_yawned > M_MS(8)) {
		g_yawned = now;
		g_flash = FACE_SLEEPY;
		g_flash_until = now + S_MS(2);
	}
}

static enum face_emotion evaluate(int64_t now)
{
	const struct nexus_status *st = nexus_status_get();

	if (!g_enabled || g_idle_pushed) {
		return FACE_SLEEPY;
	}
	if (g_half_down[0] || g_half_down[1]) {
		return FACE_SAD;
	}
	if (lowest_battery(st) < 15) {
		return FACE_SLEEPY;
	}
	if (g_brain != NEXUS_BUDDY_LOCAL) {
		return g_brain_mood;
	}
	if (now < g_flash_until) {
		return g_flash;
	}
	if (g_idle_since && now - g_idle_since > IDLE_SLEEPY) {
		return FACE_SLEEPY;
	}
	return FACE_NEUTRAL;
}

/* ---- speech ------------------------------------------------------------- */

static bool reveal(int64_t now)
{
	size_t len = strlen(g_line);

	if (!len) {
		return false;
	}
	if (g_shown < len) {
		if (now < g_next_char) {
			return false;
		}

		char c = g_line[g_shown++];

		g_next_char = now + face_char_ms(c);
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			face_anim_blip(&g_anim);
		}
		babble(c);
		return true;
	}
	if (!g_line_until) {
		g_line_until = now + BUBBLE_HOLD;
	} else if (now > g_line_until) {
		g_line[0] = '\0';
		g_line_until = 0;
		return true;
	}
	return false;
}

/* ---- frames ------------------------------------------------------------- */

static bool colour_moved(const struct face_frame *a, const struct face_frame *b)
{
	return a->colour != b->colour || a->colour_prev != b->colour_prev ||
	       a->colour_mix != b->colour_mix;
}

/*
 * Repaint the bands that changed and nothing else. A blink is the eyes'
 * rows; a word is the bubble's; the header only when the brain changes. On a
 * face at rest this paints nothing at all, and that is most of the time.
 */
static void repaint(bool bubble)
{
	const struct face_frame *f = &g_frame;
	const struct face_frame *d = &g_drawn;
	bool colour = colour_moved(f, d);
	bool phase = f->phase != d->phase;
	bool eyes = colour || f->extra != d->extra ||
		    memcmp(f->g, d->g, sizeof(f->g)) != 0 ||
		    (phase && (f->extra == FACE_X_ZZZ || f->extra == FACE_X_DOTS));
	bool mouth = colour || f->mouth != d->mouth || f->open != d->open ||
		     f->extra != d->extra ||
		     f->g[F_GAZE_X] / 3 != d->g[F_GAZE_X] / 3 ||
		     (phase && f->extra == FACE_X_BARS);
	int y0, y1;

	face_eye_rows(f, &y0, &y1);
	if (eyes) {
		nexus_screen_invalidate_rows(MIN(y0, g_eye_y0), MAX(y1, g_eye_y1));
	}
	g_eye_y0 = y0;
	g_eye_y1 = y1;
	if (mouth) {
		nexus_screen_invalidate_rows(FACE_MOUTH_Y0, FACE_MOUTH_Y1);
	}
	if (bubble) {
		nexus_screen_invalidate_rows(FACE_BUBBLE_Y,
					     FACE_BUBBLE_Y + FACE_BUBBLE_H);
	}
	if (g_header_dirty) {
		g_header_dirty = false;
		nexus_screen_invalidate_rows(0, FACE_HEADER_H);
	}
	g_drawn = *f;
	nexus_screen_paint();
}

static uint32_t frame_ms(void)
{
	if (nexus_screen_dimmed()) {
		return FRAME_MS_DARK;
	}
	return nexus_status_get()->usb_present ? FRAME_MS_USB : FRAME_MS_BATT;
}

static void anim_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!on_screen()) {
		return;
	}

	int64_t now = k_uptime_get();
	uint32_t dt = (uint32_t)(now - g_last_step);
	const struct nexus_status *st = nexus_status_get();

	g_last_step = now;

	if (g_brain != NEXUS_BUDDY_LOCAL && now >= g_lease_until) {
		release(now);
	}
	night(now);

	bool bubble = reveal(now);
	enum face_emotion e = evaluate(now);

	if (e != g_mood) {
		g_mood = e;
		face_anim_set(&g_anim, e, (uint32_t)now);
		if (g_brain == NEXUS_BUDDY_LOCAL) {
			cue(e, false);
		}
	}
	cue_pump(now);

	/* On a battery the glances stop a minute after the last key: they are
	 * frames nobody is there to see. */
	bool away = !st->usb_present && g_idle_since &&
		    now - g_idle_since > S_MS(60);

	g_anim.saccades = e != FACE_SLEEPY && e != FACE_LOVE &&
			  e != FACE_THINKING && !away;
	g_anim.slow_blinks = late();
	g_anim.bob = e == FACE_NEUTRAL && music();

	face_anim_step(&g_anim, (uint32_t)now, dt);
	face_anim_frame(&g_anim, (uint32_t)now, &g_frame);
	repaint(bubble);

	k_work_reschedule_for_queue(nexus_workq(), &g_anim_work,
				    K_MSEC(frame_ms()));
}

/* ---- screen ------------------------------------------------------------- */

static void draw_header(const struct nexus_theme *t)
{
	const char *brain = nexus_buddy_brain_name();
	int bw = gfx_text_w(brain, NEXUS_TXT_CAPTION);
	int lw = gfx_text_w("BRAIN: ", NEXUS_TXT_CAPTION);
	int x = GFX_W - NEXUS_PAD - bw;

	gfx_text(NEXUS_PAD, 8, CONFIG_NEXUS_BUDDY_NAME, NEXUS_TXT_LABEL,
		 t->accent, GFX_OPAQUE);
	gfx_text(x - lw, 12, "BRAIN: ", NEXUS_TXT_CAPTION, t->caption,
		 GFX_OPAQUE);
	gfx_text(x, 12, brain, NEXUS_TXT_CAPTION, t->value, GFX_OPAQUE);
}

static void draw_bubble(const struct nexus_theme *t)
{
	char shown[FACE_LINE_MAX + 1];
	/* Laid out for the whole line, so the words already shown stay put
	 * while the rest arrives. */
	int scale = gfx_text_w(g_line, NEXUS_TXT_BODY) <= NEXUS_CONTENT_W - 16
			    ? NEXUS_TXT_BODY
			    : NEXUS_TXT_CAPTION;
	int x = GFX_W / 2 - gfx_text_w(g_line, scale) / 2;

	memcpy(shown, g_line, g_shown);
	shown[g_shown] = '\0';

	nexus_draw_card(NEXUS_PAD, FACE_BUBBLE_Y, NEXUS_CONTENT_W, FACE_BUBBLE_H);
	gfx_text(x, FACE_BUBBLE_Y + (FACE_BUBBLE_H - gfx_text_h(scale)) / 2,
		 shown, scale, t->value, GFX_OPAQUE);
}

static void buddy_draw(void)
{
	const struct nexus_theme *t = nexus_theme();

	if (gfx_hits(0, FACE_HEADER_H)) {
		draw_header(t);
	}
	face_draw(&g_frame);
	if (g_line[0] && gfx_hits(FACE_BUBBLE_Y, FACE_BUBBLE_H)) {
		draw_bubble(t);
	}
}

static void buddy_enter(void)
{
	int64_t now = k_uptime_get();

	g_last_step = now;
	face_anim_frame(&g_anim, (uint32_t)now, &g_frame);
	g_drawn = g_frame;
	face_eye_rows(&g_frame, &g_eye_y0, &g_eye_y1);

	if (!g_greeted) {
		g_greeted = true;
		say_owner(sizeof(CONFIG_NEXUS_BUDDY_OWNER) > 1 ? "HI " : "HI",
			  "!", FACE_HAPPY);
		g_flash = FACE_HAPPY;
		g_flash_until = now + S_MS(3);
		cue(FACE_HAPPY, true);
	}
	k_work_reschedule_for_queue(nexus_workq(), &g_anim_work, K_NO_WAIT);
}

static void buddy_exit(void)
{
	k_work_cancel_delayable(&g_anim_work);
	g_idle_pushed = false;
}

static const char *const k_pokes[] = {
	"HEHE!", "BOOP!", "THAT TICKLES!", "HI THERE!", "STILL HERE!",
};

static bool buddy_action(enum nexus_action action)
{
	/* Pressing anything means someone is looking, so the idle face is
	 * now just the face. */
	g_idle_pushed = false;

	if (action != NEXUS_ACTION_SELECT) {
		return false;
	}

	int64_t now = k_uptime_get();
	/* One more choice when there is an owner: their name. */
	uint32_t choices = ARRAY_SIZE(k_pokes) +
			   (sizeof(CONFIG_NEXUS_BUDDY_OWNER) > 1 ? 1U : 0U);
	uint32_t pick = face_rand(&g_anim.rng) % choices;

	g_flash = FACE_HAPPY;
	g_flash_until = now + 2500;
	cue(FACE_HAPPY, true);
	if (g_brain == NEXUS_BUDDY_LOCAL) {
		if (pick == ARRAY_SIZE(k_pokes)) {
			say_owner("", "!", FACE_HAPPY);
		} else {
			say(k_pokes[pick], FACE_HAPPY);
		}
	}
	return true;
}

/*
 * NORMAL, not FAST: FAST is what tells the blanking timeout a game is being
 * played. The face animates on its own work item instead, and paints only
 * what moved.
 */
const struct nexus_screen nexus_screen_buddy_def = {
	.name = "BUDDY",
	.enter = buddy_enter,
	.exit = buddy_exit,
	.draw = buddy_draw,
	.action = buddy_action,
	.refresh = NEXUS_REFRESH_NORMAL,
	.btn_short = NEXUS_ACTION_SELECT, /* poke         */
	.btn_long = NEXUS_ACTION_BACK,    /* back out     */
};

bool nexus_buddy_idle_face(void)
{
#if IS_ENABLED(CONFIG_NEXUS_BUDDY_IDLE_FACE)
	/* USB: a computer is there, so power is free and someone is near.
	 * On a charger or a battery the panel blanks as it always has. */
	if (!g_enabled || !nexus_status_get()->usb_present) {
		return false;
	}
	if (!on_screen()) {
		nexus_screen_push(&nexus_screen_buddy_def);
		g_idle_pushed = true;
	}
	return true;
#else
	return false;
#endif
}

/* ---- settings ----------------------------------------------------------- */

bool nexus_buddy_enabled(void)
{
	return g_enabled;
}

void nexus_buddy_set_enabled(bool on)
{
	g_enabled = on;
	if (!on) {
		release(k_uptime_get());
		g_line[0] = '\0';
		g_cue = -1;
	}
}

enum nexus_buddy_sound nexus_buddy_sound_mode(void)
{
	return (enum nexus_buddy_sound)g_sound;
}

void nexus_buddy_set_sound_mode(enum nexus_buddy_sound mode)
{
	if ((unsigned int)mode < NEXUS_BUDDY_SOUND_MODES) {
		g_sound = (uint8_t)mode;
	}
}

enum nexus_buddy_brain nexus_buddy_brain_pin(void)
{
	return (enum nexus_buddy_brain)g_pin;
}

void nexus_buddy_set_brain_pin(enum nexus_buddy_brain pin)
{
	if ((unsigned int)pin >= NEXUS_BUDDY_BRAINS) {
		return;
	}
	g_pin = (uint8_t)pin;
	if (g_brain != NEXUS_BUDDY_LOCAL && !accepts(g_brain)) {
		release(k_uptime_get());
	}
}

#if IS_ENABLED(CONFIG_NEXUS_SETTINGS_PERSIST)
/*
 * Its own key, not three more bytes in nexus/ui/prefs. A build that turns
 * Buddy off again then finds the main settings exactly as it left them,
 * rather than a blob of the wrong size that it would throw away whole.
 */
#define BUDDY_PREFS_VERSION 1

struct buddy_prefs {
	uint8_t version;
	uint8_t enabled;
	uint8_t sound;
	uint8_t brain;
};

static int prefs_set(const char *name, size_t len, settings_read_cb read_cb,
		     void *cb_arg)
{
	struct buddy_prefs p;

	if (!settings_name_steq(name, "prefs", NULL)) {
		return 0;
	}
	if (len != sizeof(p) ||
	    read_cb(cb_arg, &p, sizeof(p)) != (ssize_t)sizeof(p) ||
	    p.version != BUDDY_PREFS_VERSION) {
		return 0; /* defaults are always safe */
	}
	g_enabled = p.enabled != 0;
	nexus_buddy_set_sound_mode((enum nexus_buddy_sound)p.sound);
	nexus_buddy_set_brain_pin((enum nexus_buddy_brain)p.brain);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nexus_buddy, "nexus/buddy", NULL, prefs_set,
			       NULL, NULL);

int nexus_buddy_prefs_save(void)
{
	struct buddy_prefs p = {
		.version = BUDDY_PREFS_VERSION,
		.enabled = g_enabled ? 1 : 0,
		.sound = g_sound,
		.brain = g_pin,
	};

	return settings_save_one("nexus/buddy/prefs", &p, sizeof(p));
}
#else
int nexus_buddy_prefs_save(void)
{
	return 0;
}
#endif

int nexus_buddy_init(void)
{
	face_anim_init(&g_anim, k_cycle_get_32(), (uint32_t)k_uptime_get());
	return nexus_status_subscribe(on_status);
}
