/*
 * The companion link: lines of text the host writes, and what they mean.
 *
 * This file is the part no wire knows about - the model, the parser, the
 * staleness rule. How the bytes arrive is a transport's business
 * (host_link_usb.c); each one copies them into its own source and posts the
 * work below.
 *
 * Parsing happens on the NEXUS work queue, never where the bytes arrive. A
 * transport does the least it can: move bytes into a ring buffer and post
 * the work. The work item assembles lines and parses them. Model updates and
 * repaints from an interrupt would be a much worse bug than a few bytes of
 * latency.
 */

#include <nexus/host.h>
#include <nexus/screen.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

#include "../nexus_priv.h"
#include "host_priv.h"

static struct nexus_host g_host = {
	.cpu = NEXUS_HOST_UNKNOWN,
	.mem = NEXUS_HOST_UNKNOWN,
	.clock_sec = UINT32_MAX,
	.day = UINT32_MAX,
};

/*
 * Bytes, not lines, cross from a transport to the work item.
 *
 * This used to hand over one finished line at a time and drop any line that
 * arrived before the work item had taken the last. The reasoning was that
 * every field is a current value and the next one is a second away - which is
 * true of CPU and memory and false of everything else: the clock is resent
 * once a minute and the track only when it changes. And a companion writes
 * its lines back to back, so they arrive in one USB packet and one interrupt,
 * before the work item can possibly have run. Every line after the first was
 * lost, every time.
 *
 * A source's ring holds a whole burst, and its line buffer is touched only by
 * the work item, so there is nothing left to race. The lock is for the ring's
 * own indices, held for a memcpy.
 */
static void parse_work_fn(struct k_work *work);
static K_WORK_DEFINE(g_parse, parse_work_fn);

static void stale_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_stale, stale_work_fn);

const struct nexus_host *nexus_host(void)
{
	return &g_host;
}

uint32_t nexus_host_clock(void)
{
	if (g_host.clock_sec == UINT32_MAX) {
		return UINT32_MAX;
	}

	int64_t since = (k_uptime_get() - g_host.clock_at) / MSEC_PER_SEC;

	return (uint32_t)((g_host.clock_sec + since) % 86400U);
}

/* Whole days the kernel clock has run past the midnight clock_sec counts
 * from. Zero until the first midnight after a resync. */
static uint32_t days_since_clock(void)
{
	if (g_host.clock_sec == UINT32_MAX) {
		return 0;
	}

	int64_t since = (k_uptime_get() - g_host.clock_at) / MSEC_PER_SEC;

	return (uint32_t)((g_host.clock_sec + since) / 86400U);
}

uint32_t nexus_host_day(void)
{
	if (g_host.day == UINT32_MAX) {
		return UINT32_MAX;
	}
	return g_host.day + days_since_clock();
}

/* ---- parsing ------------------------------------------------------------ */

static uint32_t to_u32(const char *s, uint32_t max)
{
	uint32_t v = 0;

	if (*s < '0' || *s > '9') {
		return UINT32_MAX;
	}
	for (; *s >= '0' && *s <= '9'; s++) {
		v = v * 10U + (uint32_t)(*s - '0');
		if (v > max) {
			return UINT32_MAX;
		}
	}
	/* The whole value or nothing. "37abc" is not 37: it is a line that
	 * got mangled, and a mangled line is the one place a plausible wrong
	 * number could come from. */
	return *s == '\0' ? v : UINT32_MAX;
}

/*
 * Fold a host string into what the panel can actually draw.
 *
 * The font is ASCII 32..90 - space through Z, uppercase only - and gfx_text()
 * draws anything else as '?'. A track title from a real media player is full
 * of things outside that: lower case, em dashes, accents, whatever the artist
 * felt like. Unfolded, "Miles Davis - So What" arrives as a row of question
 * marks.
 *
 * Here rather than in the companions, because the constraint belongs to the
 * font, not to the host. Every companion that ever gets written benefits, and
 * none of them has to know what the dongle's font covers.
 *
 * Lower case folds up. Everything still out of range is dropped rather than
 * substituted: a dropped character leaves a readable title, and a line of '?'
 * where the accents were does not. UTF-8 multibyte sequences are all >127, so
 * they disappear by the same rule.
 */
static void fold(char *dst, const char *src, size_t max)
{
	size_t n = 0;

	for (; *src && n + 1U < max; src++) {
		char c = *src;

		if (c >= 'a' && c <= 'z') {
			c -= 'a' - 'A';
		}
		if ((unsigned char)c < 32U || (unsigned char)c > 90U) {
			continue;
		}
		dst[n++] = c;
	}
	dst[n] = '\0';
}

/*
 * One line. The first character is the field; anything after a single space
 * is its value. Unknown fields are ignored rather than rejected, so a newer
 * companion talking to older firmware degrades instead of failing.
 */
static void parse_line(const char *line)
{
	char key = line[0];
	const char *val = line[1] == ' ' ? &line[2] : &line[1];
	bool was_linked = g_host.link;
	uint32_t n;

	/*
	 * Every branch repaints only if the value actually moved, and only the
	 * card that shows it. A companion sends the same CPU figure for
	 * seconds at a time, and a number that has not changed is not news.
	 */
	switch (key) {
	case 'C':
		n = to_u32(val, 100);
		n = (n == UINT32_MAX) ? NEXUS_HOST_UNKNOWN : n;
		if (g_host.cpu != (uint8_t)n) {
			g_host.cpu = (uint8_t)n;
			nexus_host_screen_dirty(NEXUS_HOST_F_LOAD);
		}
		break;
	case 'M':
		n = to_u32(val, 100);
		n = (n == UINT32_MAX) ? NEXUS_HOST_UNKNOWN : n;
		if (g_host.mem != (uint8_t)n) {
			g_host.mem = (uint8_t)n;
			nexus_host_screen_dirty(NEXUS_HOST_F_LOAD);
		}
		break;
	case 'T': {
		n = to_u32(val, 86399);
		if (n == UINT32_MAX) {
			break;
		}

		uint32_t was = nexus_host_clock();
		uint32_t today = nexus_host_day();

		/* The minute on the face is what is drawn, so a resync that
		 * lands in the same minute is not worth a frame. */
		if (was / 60U != n / 60U) {
			nexus_host_screen_dirty(NEXUS_HOST_F_CLOCK);
		}
		g_host.clock_sec = n;
		g_host.clock_at = k_uptime_get();

		/*
		 * Carry the date across. g_host.day is filed against the day
		 * the clock counts from, and that just moved - without this a
		 * T on its own, after the kernel clock had passed midnight,
		 * put the date back a day. Near midnight the two clocks can
		 * also disagree about which day it is by a few seconds; more
		 * than half a day apart means one has wrapped and one has not.
		 */
		if (today != UINT32_MAX) {
			if (was != UINT32_MAX && was > n + 43200U) {
				today++;        /* the host is past midnight; we were not */
			} else if (was != UINT32_MAX && n > was + 43200U && today) {
				today--;        /* we were past midnight; the host is not */
			}
			g_host.day = today;
		}
		break;
	}
	case 'N': {
		char clean[NEXUS_HOST_TEXT];

		/* Compared after folding, not before: two titles that differ
		 * only in case or in an accent draw identically, and a repaint
		 * that changes nothing on screen is a wasted frame. */
		fold(clean, val, sizeof(clean));
		if (strcmp(g_host.now_playing, clean)) {
			strcpy(g_host.now_playing, clean);
			nexus_host_screen_dirty(NEXUS_HOST_F_NP);
		}
		break;
	}
	case 'A': {
		char clean[NEXUS_HOST_TEXT];

		fold(clean, val, sizeof(clean));
		if (strcmp(g_host.artist, clean)) {
			strcpy(g_host.artist, clean);
			nexus_host_screen_dirty(NEXUS_HOST_F_NP);
		}
		break;
	}
	case 'P': {
		/* 1 playing, 0 paused. Anything else is not news. */
		n = to_u32(val, 1);
		if (n == UINT32_MAX) {
			break;
		}

		bool paused = n == 0U;

		if (g_host.paused != paused) {
			g_host.paused = paused;
			nexus_host_screen_dirty(NEXUS_HOST_F_NP);
		}
		break;
	}
	case 'D': {
		/* Up to 2517. Past that, somebody else's problem. */
		n = to_u32(val, 200000);
		if (n == UINT32_MAX) {
			break;
		}
		if (nexus_host_day() != n) {
			nexus_host_screen_dirty(NEXUS_HOST_F_CLOCK);
		}

		/*
		 * Stored against the day clock_sec counts from, so the date and
		 * the time turn over at the same midnight. Straight after a T
		 * this subtracts zero; it matters when the kernel clock is a
		 * few seconds past midnight and the host is not yet.
		 */
		uint32_t past = days_since_clock();

		g_host.day = n > past ? n - past : 0U;
		break;
	}
	case 'X':
		/* The companion is going away and says so, rather than leaving
		 * numbers on screen that stopped being true when it quit.
		 *
		 * Not the clock or the date: those stay true for as long as
		 * the dongle keeps power, which is what lets the companion be
		 * something that ran once this morning rather than something
		 * that must be running all day. */
		g_host.cpu = NEXUS_HOST_UNKNOWN;
		g_host.mem = NEXUS_HOST_UNKNOWN;
		g_host.now_playing[0] = '\0';
		g_host.artist[0] = '\0';
		g_host.paused = false;
		g_host.link = false;
		nexus_host_screen_dirty(NEXUS_HOST_F_LINK);
		return;
	default:
		return;
	}

	g_host.link = true;
	if (!was_linked) {
		/* Coming up is the one time the whole screen changes: the
		 * header, and every dash that becomes a number. */
		nexus_host_screen_dirty(NEXUS_HOST_F_LINK);
	}
}

/* ---- sources ------------------------------------------------------------ */

void host_source_put(struct host_source *src, const uint8_t *buf, uint32_t len)
{
	k_spinlock_key_t key = k_spin_lock(&src->lock);

	ring_buf_put(src->ring, buf, len);
	k_spin_unlock(&src->lock, key);
}

void host_link_kick(void)
{
	k_work_submit_to_queue(nexus_workq(), &g_parse);
}

/** Everything one source has waiting. @return how many lines it parsed. */
static unsigned int drain(struct host_source *src)
{
	uint8_t chunk[32];
	uint32_t got;
	unsigned int lines = 0;

	do {
		k_spinlock_key_t key = k_spin_lock(&src->lock);

		got = ring_buf_get(src->ring, chunk, sizeof(chunk));
		k_spin_unlock(&src->lock, key);

		lines += host_lines_feed(&src->lines, chunk, got, parse_line);
	} while (got == sizeof(chunk));

	return lines;
}

static void parse_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	unsigned int lines = 0;

#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK_USB)
	lines += drain(&host_usb);
#endif
	if (lines == 0) {
		return;
	}

	/*
	 * Restart the staleness timer on every line. A companion that is
	 * killed, or a cable pulled, stops sending - and the screen has to
	 * stop claiming a 3% CPU that was true a minute ago.
	 */
	k_work_reschedule_for_queue(nexus_workq(), &g_stale,
				    K_SECONDS(CONFIG_NEXUS_HOST_STALE_S));
}

static void stale_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!g_host.link) {
		return;
	}
	g_host.link = false;
	nexus_host_screen_dirty(NEXUS_HOST_F_LINK);
}

int nexus_host_link_init(void)
{
	/* With no transport built the HOST screen still has something true
	 * to show - how long the host has been connected - so that is not an
	 * error. */
#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK_USB)
	return host_link_usb_init();
#else
	return 0;
#endif
}
