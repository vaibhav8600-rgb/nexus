/*
 * HOST - what the machine on the other end of the cable knows.
 *
 * The clock is the reason this screen exists. The dongle has no RTC, so the
 * only real time it will ever see is the time a companion tells it, and a
 * clock is the one thing a screen on a desk is expected to have.
 *
 * Everything here has to survive the companion not being there, which is the
 * normal state: no cable, a phone on the other end, or simply nothing
 * running. So the screen degrades in steps rather than to a wall of dashes:
 *
 *   companion running   clock and date, CPU, RAM, what is playing
 *   companion ran once  the clock and date carry on - they stay true for as
 *                       long as the dongle has power
 *   nothing installed   how long this host has been connected, which the
 *                       dongle knows by itself
 *   no host at all      dashes, and the words for why
 *
 * Unknown draws as dashes in the muted colour, never as a stale number - a
 * CPU meter frozen at 3% is worse than an empty one, because it looks like it
 * is working.
 */

#include <nexus/gfx.h>
#include <nexus/host.h>
#include <nexus/nexus.h>
#include <nexus/screen.h>
#include <nexus/status.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "../nexus_priv.h"

/* ---- layout ------------------------------------------------------------- */

#define HDR_Y 4
#define PILL_H 17
#define CLOCK_Y 31
#define CLOCK_H 68
#define METER_Y 106
#define METER_H 58
#define NP_Y 171
#define NP_H 60

#define STAT_W 108 /* two of them and a 6px gutter fill the content width */
#define TILE 26    /* icon tile in a stat card */
#define ART 38     /* the now-playing art tile */
#define EQ_ROOM 26 /* what the level bars take from the title */

/*
 * The clock numerals: the display face at 2x, 28px. It was 3x, and from a few
 * feet away the time was all there was - the date under it was 7px and the
 * track 7px or 14px. Everything a glance is for is 14px or more now, and the
 * rows the clock gave back went to the now-playing card.
 */
#define BIG 2
#define LINE_Y (CLOCK_Y + 46) /* the date, or what stands in for it */

/* The now-playing art tile, centred in its card, and the level bars beside
 * the text, standing on a common baseline. */
#define ART_Y (NP_Y + (NP_H - ART) / 2)
#define EQ_BASE (ART_Y + ART - 4)
#define EQ_MAX 24

/* ---- icons: 1-bit, row-major, bit N = column N, for gfx_glyph() ---------- */

#define MON_W 11
#define MON_H 8
static const uint16_t ic_monitor[MON_H] = {
	0x7FF, 0x401, 0x401, 0x401, 0x401, 0x7FF, 0x020, 0x0F8,
};

#define ICON_H 9
#define CHIP_W 9
static const uint16_t ic_chip[ICON_H] = {
	0x054, 0x0FE, 0x183, 0x0BA, 0x1BB, 0x0BA, 0x183, 0x0FE, 0x054,
};

/* A module: three chips in a frame over its contact strip, notched. */
#define RAM_W 12
static const uint16_t ic_ram[ICON_H] = {
	0x000, 0xFFF, 0x801, 0xB6D, 0xB6D, 0x801, 0xFFF, 0x000, 0x7DE,
};

#define NOTE_W 9
static const uint16_t ic_note[ICON_H] = {
	0x1F8, 0x1F8, 0x108, 0x108, 0x108, 0x1CE, 0x1EF, 0x1EF, 0x0C6,
};

/* ---- numerals ----------------------------------------------------------- */

/*
 * Clock numerals in the display face. The face has digits and letters and
 * nothing else - every punctuation glyph in it is blank, because the wordmark
 * never needed one - so the two a clock uses are drawn, in face pixels, at any
 * scale. The colon is two squares in a four-pixel cell, which is also what a
 * clock face wants: a full-width colon cell pushes the hours and minutes apart
 * until they read as two numbers. The dash is a bar in a digit's cell, so
 * "--:--" holds the width of the time it stands in for.
 */
int nexus_host_numerals_w(const char *s, int scale)
{
	int w = 0;

	for (; *s; s++) {
		w += *s == ':' ? 4 * scale : gfx_face_w("0", scale) + scale;
	}
	return w > 0 ? w - scale : 0;
}

int nexus_host_numerals(int x, int y, const char *s, int scale, gfx_color c)
{
	char one[2] = { 0, 0 };

	for (; *s; s++) {
		if (*s == ':') {
			gfx_rect(x + scale, y + 4 * scale, 2 * scale, 2 * scale, c,
				 GFX_OPAQUE);
			gfx_rect(x + scale, y + 9 * scale, 2 * scale, 2 * scale, c,
				 GFX_OPAQUE);
			x += 4 * scale;
			continue;
		}
		if (*s == '-') {
			gfx_rect(x + 2 * scale, y + 6 * scale, 6 * scale, 2 * scale,
				 c, GFX_OPAQUE);
			x += gfx_face_w("0", scale) + scale;
			continue;
		}
		one[0] = *s;
		gfx_face_text(x, y, one, scale, c, GFX_OPAQUE);
		x += gfx_face_w(one, scale) + scale;
	}
	return x;
}

/* ---- the clock card ----------------------------------------------------- */

/* One stretch of the clock line: numerals at BIG, or a unit beside them. */
struct run {
	const char *s;
	bool big;
};

static int run_w(const struct run *r)
{
	return r->big ? nexus_host_numerals_w(r->s, BIG) : gfx_face_w(r->s, 1);
}

/* Tight before a unit, so "PM" and "H" belong to their number; wide after
 * one, so "2H 14M" does not run together into a single word. */
static int run_gap(const struct run *r)
{
	return r->big ? 8 : 4;
}

static void draw_runs(int y, const struct run *runs, int n, gfx_color big_c,
		      gfx_color unit_c)
{
	int total = 0;

	for (int i = 0; i < n; i++) {
		total += run_w(&runs[i]) + (i ? run_gap(&runs[i]) : 0);
	}

	int x = GFX_W / 2 - total / 2;

	for (int i = 0; i < n; i++) {
		const struct run *r = &runs[i];

		if (i) {
			x += run_gap(r);
		}
		if (!r->big) {
			/* On the numerals' baseline, not their top. */
			gfx_face_text(x, y + gfx_face_h(BIG) - gfx_face_h(1),
				      r->s, 1, unit_c, GFX_OPAQUE);
			x += run_w(r);
			continue;
		}

		nexus_host_numerals(x, y, r->s, BIG, big_c);
		x += run_w(r);
	}
}

static const char *const k_wday[7] = {
	"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};
static const char *const k_month[12] = {
	"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
	"JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

bool nexus_host_time_text(char *buf, int len, const char **suffix)
{
	uint32_t sec = nexus_host_clock();
	uint32_t hour = sec / 3600U;
	int n = 0;

	*suffix = NULL;
	if (sec == UINT32_MAX) {
		return false;
	}

	if (!IS_ENABLED(CONFIG_NEXUS_HOST_CLOCK_24H)) {
		*suffix = hour < 12U ? "AM" : "PM";
		hour %= 12U;
		if (hour == 0U) {
			hour = 12U; /* midnight and noon are 12, not 0 */
		}
	}

	/* Padded on a 24 hour clock so the colon never moves; unpadded on a 12
	 * hour one, because "01:32 PM" is not what a clock face says. */
	gfx_utoa(hour, buf, len, *suffix ? 0 : 2);
	while (buf[n]) {
		n++;
	}
	buf[n++] = ':';
	gfx_utoa((sec % 3600U) / 60U, &buf[n], len - n, 2);
	return true;
}

/*
 * Days since 1970-01-01 to "SUN", "13 SEP" and "2026": Howard Hinnant's
 * civil_from_days. Pure integer arithmetic, correct for every Gregorian date
 * this can be handed - leap years, 2000 and 2100 included - with no table and
 * no library.
 */
void nexus_host_date_text(uint32_t days, char *wday, char *dmon, char *yyyy)
{
	uint32_t z = days + 719468U;
	uint32_t era = z / 146097U;
	uint32_t doe = z % 146097U;
	uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
	uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
	uint32_t mp = (5U * doy + 2U) / 153U;
	uint32_t mday = doy - (153U * mp + 2U) / 5U + 1U;
	uint32_t mon = mp < 10U ? mp + 2U : mp - 10U; /* 0-based */
	/* The algorithm's year starts in March, so January and February
	 * belong to the next calendar year. */
	uint32_t year = yoe + era * 400U + (mon <= 1U ? 1U : 0U);

	strcpy(wday, k_wday[(days + 4U) % 7U]); /* 1970-01-01 was a Thursday */
	gfx_utoa(mday, dmon, 3, 0);
	strcat(dmon, " ");
	strcat(dmon, k_month[mon]);
	gfx_utoa(year, yyyy, 5, 0);
}

/*
 * What the clock card shows, reduced to a number that changes exactly when
 * the card would. The tick compares it; the card repaints only when it moves.
 */
static uint32_t clock_key(void)
{
	uint32_t sec = nexus_host_clock();

	if (sec != UINT32_MAX) {
		return sec / 60U;
	}

	const struct nexus_status *st = nexus_status_get();

	if (!st->host_up) {
		return UINT32_MAX;
	}
	return 0x10000U + (uint32_t)((k_uptime_get() - st->host_up_at) / 60000);
}

/*
 * The line under the clock, centred: the date, or what stands in for it. The
 * display face at 1x, 14px and bold, in the caption colour - large enough to
 * read with the time, quiet enough not to compete with it. The face has
 * letters, digits and spaces, which is all any of these lines use.
 */
static void draw_line(const char *s)
{
	gfx_face_text(GFX_W / 2 - gfx_face_w(s, 1) / 2, LINE_Y, s, 1,
		      nexus_theme()->caption, GFX_OPAQUE);
}

static void draw_clock(void)
{
	if (!gfx_hits(CLOCK_Y, CLOCK_H)) {
		return;
	}

	const struct nexus_theme *t = nexus_theme();
	const struct nexus_status *st = nexus_status_get();
	const char *suffix;
	int y = CLOCK_Y + 9;
	char a[8];
	char b[8];
	char line[16]; /* "WED 30 SEP 2026" and its terminator, exactly */

	nexus_draw_card(NEXUS_PAD, CLOCK_Y, NEXUS_CONTENT_W, CLOCK_H);
	gfx_round_frame(NEXUS_PAD, CLOCK_Y, NEXUS_CONTENT_W, CLOCK_H, t->radius,
			t->accent, 70);

	/*
	 * Seconds are deliberately absent: they would repaint this card once a
	 * second forever, which is the one thing a screen that sits idle on a
	 * desk must not do.
	 */
	if (nexus_host_time_text(a, sizeof(a), &suffix)) {
		struct run runs[2] = { { a, true }, { suffix, false } };

		draw_runs(y, runs, suffix ? 2 : 1, t->value, t->caption);

		uint32_t day = nexus_host_day();

		if (day != UINT32_MAX) {
			char wd[4];
			char dm[8];
			char yy[5];

			nexus_host_date_text(day, wd, dm, yy);
			strcpy(line, wd);
			strcat(line, " ");
			strcat(line, dm);
			strcat(line, " ");
			strcat(line, yy);
			draw_line(line);
		} else {
			draw_line("HOST TIME"); /* an older companion: no date */
		}
		return;
	}

	if (st->host_up) {
		/*
		 * No companion has ever set the clock, but a host is here - and
		 * how long it has been here is something the dongle knows with
		 * nothing installed on the other end. Hours and minutes, never
		 * H:MM, so it cannot be mistaken for a time of day.
		 */
		uint32_t mins = (uint32_t)((k_uptime_get() - st->host_up_at) / 60000);
		uint32_t hrs = MIN(mins / 60U, 999U);
		struct run runs[4];
		int n = 0;

		if (hrs) {
			runs[n++] = (struct run){ gfx_utoa(hrs, a, sizeof(a), 0), true };
			runs[n++] = (struct run){ "H", false };
			runs[n++] = (struct run){ gfx_utoa(mins % 60U, b, sizeof(b), 2),
						  true };
		} else {
			runs[n++] = (struct run){ gfx_utoa(mins, b, sizeof(b), 0), true };
		}
		runs[n++] = (struct run){ "M", false };

		draw_runs(y, runs, n, t->value, t->caption);
		draw_line("SINCE CONNECTED");
		return;
	}

	struct run dashes = { "--:--", true };

	draw_runs(y, &dashes, 1, t->muted, t->muted);
	draw_line("NO HOST");
}

/* ---- the rest ----------------------------------------------------------- */

static void draw_header(const struct nexus_host *h)
{
	if (!gfx_hits(HDR_Y, 20)) {
		return;
	}

	const struct nexus_theme *t = nexus_theme();
	gfx_color on = h->link ? t->success : t->muted;
	/* The link light, in words rather than a dot: this screen is mostly
	 * empty without it, and "why is it empty" should be answered on the
	 * screen asking the question. */
	const char *state = h->link ? "LINKED" : "NO LINK";
	int pw = gfx_text_w(state, NEXUS_TXT_CAPTION) + 26;
	int px = GFX_W - NEXUS_PAD - pw;

	gfx_glyph(NEXUS_PAD, HDR_Y + 3, ic_monitor, MON_W, MON_H, 2,
		  h->link ? t->accent : t->muted, GFX_OPAQUE);
	nexus_draw_label(NEXUS_PAD + 28, HDR_Y + 3, "HOST");

	gfx_round_rect(px, HDR_Y + 2, pw, PILL_H, PILL_H / 2, on, 40);
	gfx_round_frame(px, HDR_Y + 2, pw, PILL_H, PILL_H / 2, on, 110);
	gfx_disc(px + 10, HDR_Y + 10, 3, on, GFX_OPAQUE);
	gfx_text(px + 18, HDR_Y + 7, state, NEXUS_TXT_CAPTION, on, GFX_OPAQUE);
}

static void draw_stat(int x, const char *label, const uint16_t *icon, int iw,
		      uint8_t pct, gfx_color accent)
{
	if (!gfx_hits(METER_Y, METER_H)) {
		return;
	}

	const struct nexus_theme *t = nexus_theme();
	bool known = pct != NEXUS_HOST_UNKNOWN;
	/* Over 80% in the warning colour: the number is the reading, the
	 * colour is the judgement, same as the battery meters. */
	bool hot = known && pct >= 80U;
	gfx_color c = known ? accent : t->muted;
	char buf[8];

	nexus_draw_card(x, METER_Y, STAT_W, METER_H);
	gfx_round_frame(x, METER_Y, STAT_W, METER_H, t->radius, c, 60);
	gfx_round_rect(x + 8, METER_Y + 8, TILE, TILE, 7, c, 45);
	gfx_glyph(x + 8 + (TILE - iw * 2) / 2, METER_Y + 8 + (TILE - ICON_H * 2) / 2,
		  icon, iw, ICON_H, 2, c, GFX_OPAQUE);
	nexus_draw_caption(x + 42, METER_Y + 9, label);

	if (!known) {
		gfx_text(x + 42, METER_Y + 20, "--", NEXUS_TXT_VALUE, t->muted,
			 GFX_OPAQUE);
		nexus_draw_meter(x + 8, METER_Y + 44, STAT_W - 16, 6, 0, c);
		return;
	}

	gfx_utoa(pct, buf, sizeof(buf), 0);
	gfx_text(x + 42, METER_Y + 19, buf, NEXUS_TXT_VALUE,
		 hot ? t->warning : t->value, GFX_OPAQUE);
	gfx_text(x + 42 + gfx_text_w(buf, NEXUS_TXT_VALUE) + 3,
		 METER_Y + 19 + gfx_text_h(NEXUS_TXT_VALUE) -
			 gfx_text_h(NEXUS_TXT_CAPTION),
		 "%", NEXUS_TXT_CAPTION, t->caption, GFX_OPAQUE);
	nexus_draw_meter(x + 8, METER_Y + 44, STAT_W - 16, 6, pct,
			 hot ? t->warning : accent);
}

/*
 * Copy @p src into @p dst, cut to what fits in @p room pixels at @p scale,
 * with ".." where it was cut. Never wider than asked: text on this panel is
 * not clipped to its card, so a long title would otherwise run out of the
 * card, over the level bars and off the screen.
 */
static void fit_text(char *dst, const char *src, int scale, int room)
{
	int adv = gfx_text_w("0", scale) + scale;
	size_t n = (size_t)((room + scale) / adv);
	size_t len = strlen(src);

	if (len <= n) {
		memcpy(dst, src, len + 1U);
		return;
	}
	n = n > 2U ? n - 2U : 0U;
	memcpy(dst, src, n);
	strcpy(&dst[n], "..");
}

/*
 * The level bars, as frames. The host sends no levels, so this is a shape
 * that reads as music rather than a measurement of it: eight frames, four
 * bars, heights in pixels. Frame 0 is the still pose.
 */
#define EQ_FRAMES 8
static const uint8_t k_eq[EQ_FRAMES][4] = {
	{ 10, 20, 14, 24 }, { 16, 12, 22, 18 }, { 22, 8, 18, 12 },
	{ 14, 18, 10, 20 }, { 8, 24, 16, 14 },  { 18, 16, 24, 8 },
	{ 24, 10, 12, 16 }, { 12, 22, 20, 10 },
};

/*
 * The frame the bars are on. The tick advances it; draw() only reads it. It
 * cannot be derived from the uptime inside draw(), because draw() runs once
 * per band - the bars span two bands, and two readings of the clock a few
 * milliseconds apart would tear them across the boundary.
 */
static uint8_t g_eq_frame;

/* Moving bars: a track is showing, and the host has not said it is paused. */
static bool np_animating(const struct nexus_host *h)
{
	return h->link && h->now_playing[0] != '\0' && !h->paused;
}

static void draw_now_playing(const struct nexus_host *h)
{
	if (!gfx_hits(NP_Y, NP_H)) {
		return;
	}

	const struct nexus_theme *t = nexus_theme();
	bool shown = h->link && h->now_playing[0] != '\0';
	int tx = NEXUS_PAD + 8 + ART + 10;
	int room = NEXUS_CONTENT_W - (8 + ART + 10) - 8 - (shown ? EQ_ROOM : 0);
	char buf[NEXUS_HOST_TEXT];

	nexus_draw_card(NEXUS_PAD, NP_Y, NEXUS_CONTENT_W, NP_H);
	gfx_round_rect(NEXUS_PAD + 8, ART_Y, ART, ART, 8,
		       shown ? t->accent_alt : t->muted, 50);
	if (shown) {
		gfx_glyph_grad(NEXUS_PAD + 14, ART_Y + 6, ic_note, NOTE_W, ICON_H,
			       3, t->accent_alt, t->accent, GFX_OPAQUE);
	} else {
		gfx_glyph(NEXUS_PAD + 14, ART_Y + 6, ic_note, NOTE_W, ICON_H, 3,
			  t->muted, GFX_OPAQUE);
	}
	nexus_draw_caption(tx, NP_Y + 7, "NOW PLAYING");

	if (!h->link) {
		gfx_text(tx, NP_Y + 22, "--", NEXUS_TXT_BODY, t->muted, GFX_OPAQUE);
		return;
	}
	if (!shown) {
		gfx_text(tx, NP_Y + 22, "NOTHING", NEXUS_TXT_BODY, t->muted,
			 GFX_OPAQUE);
		return;
	}

	/*
	 * Body size, always. It used to drop to caption size for a long title,
	 * which is the right trade at arm's length and unreadable from across a
	 * desk. A title that does not fit wraps to a second line instead, broken
	 * at a space, and the artist gives way to it - the title is what you
	 * look up to read.
	 */
	const char *title = h->now_playing;
	int n = (room + NEXUS_TXT_BODY) /
		(gfx_text_w("0", NEXUS_TXT_BODY) + NEXUS_TXT_BODY);
	int len = (int)strlen(title);

	if (len <= n) {
		gfx_text(tx, NP_Y + 19, title, NEXUS_TXT_BODY, t->value,
			 GFX_OPAQUE);
		if (h->artist[0] != '\0') {
			fit_text(buf, h->artist, NEXUS_TXT_BODY, room);
			gfx_text(tx, NP_Y + 38, buf, NEXUS_TXT_BODY, t->caption,
				 GFX_OPAQUE);
		}
	} else {
		int cut = n;

		while (cut > 0 && title[cut] != ' ') {
			cut--;
		}

		/* One long word: break it where the line ends. */
		int next = cut > 0 ? cut + 1 : n;

		cut = cut > 0 ? cut : n;
		memcpy(buf, title, (size_t)cut);
		buf[cut] = '\0';
		gfx_text(tx, NP_Y + 19, buf, NEXUS_TXT_BODY, t->value,
			 GFX_OPAQUE);
		fit_text(buf, &title[next], NEXUS_TXT_BODY, room);
		gfx_text(tx, NP_Y + 37, buf, NEXUS_TXT_BODY, t->value,
			 GFX_OPAQUE);
	}

	/*
	 * Level bars: moving while the track plays, a flat muted line while it
	 * is paused. Only while this screen is up and something plays does the
	 * tick move them - see host_tick().
	 */
	bool live = !h->paused;
	int bx = NEXUS_PAD + NEXUS_CONTENT_W - 8 - 18;

	for (int k = 0; k < 4; k++) {
		int bh = live ? k_eq[g_eq_frame % EQ_FRAMES][k] : 4;

		gfx_round_rect(bx + k * 5, EQ_BASE - bh, 3, bh, 1,
			       live ? t->accent_alt : t->muted, GFX_OPAQUE);
	}
}

static void host_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	const struct nexus_host *h = nexus_host();
	/* Values outlive the link in the model; they must not on screen. */
	uint8_t cpu = h->link ? h->cpu : NEXUS_HOST_UNKNOWN;
	uint8_t mem = h->link ? h->mem : NEXUS_HOST_UNKNOWN;

	nexus_draw_ground();
	draw_header(h);
	draw_clock();
	draw_stat(NEXUS_PAD, "CPU", ic_chip, CHIP_W, cpu, t->accent);
	draw_stat(NEXUS_PAD + STAT_W + 6, "RAM", ic_ram, RAM_W, mem,
		  t->accent_alt);
	draw_now_playing(h);
}

/*
 * What the clock card last showed. The clock moves on its own - the kernel
 * counts between resyncs, and the connected-for time needs no host at all -
 * so nothing arrives to say the minute has turned. Without this the card
 * repainted only when a resync happened to disagree with it, which in
 * practice was never: the clock on screen simply stopped.
 */
static uint32_t g_clock_shown;

static void host_enter(void)
{
	/* The first paint is the whole screen; no need for the tick to
	 * repaint the card straight after it. */
	g_clock_shown = clock_key();
}

static void host_tick(void)
{
	uint32_t key = clock_key();

	if (key != g_clock_shown) {
		g_clock_shown = key;
		nexus_screen_invalidate_rows(CLOCK_Y, CLOCK_Y + CLOCK_H);
	}

	/*
	 * The level bars, one frame per tick - five a second at NORMAL - and
	 * only the rows they stand in: two bands. Nothing moves, and nothing is
	 * sent to the panel, while the track is paused, while nothing plays,
	 * on any other screen (no tick runs for this one), or while the display
	 * is blanked (nothing renders).
	 */
	if (np_animating(nexus_host())) {
		g_eq_frame++;
		nexus_screen_invalidate_rows(EQ_BASE - EQ_MAX, EQ_BASE);
	}
}

void nexus_host_screen_dirty(enum nexus_host_field field)
{
	/*
	 * Not the screen in front of you: nothing to repaint. This is the
	 * whole of the cost. A companion sends two or three lines a second,
	 * and before this every one of them repainted all 240 rows - while
	 * you were on the dashboard, or mid-game, where none of it is even
	 * drawn. The panel is the slowest thing on this device; the cheapest
	 * frame is the one never sent.
	 */
	if (nexus_screen_current() != &nexus_screen_host_def) {
		return;
	}

	switch (field) {
	case NEXUS_HOST_F_CLOCK:
		nexus_screen_invalidate_rows(CLOCK_Y, CLOCK_Y + CLOCK_H);
		break;
	case NEXUS_HOST_F_LOAD:
		nexus_screen_invalidate_rows(METER_Y, METER_Y + METER_H);
		break;
	case NEXUS_HOST_F_NP:
		nexus_screen_invalidate_rows(NP_Y, NP_Y + NP_H);
		break;
	case NEXUS_HOST_F_LINK:
	default:
		/* The header says LINKED or not, and every value below it
		 * turns to dashes with it. That is the whole screen. */
		nexus_screen_invalidate();
		break;
	}
}

const struct nexus_screen nexus_screen_host_def = {
	.name = "HOST",
	.enter = host_enter,
	.draw = host_draw,
	.tick = host_tick,
	/*
	 * NORMAL, not IDLE. The companion pushes about once a second and each
	 * line invalidates; IDLE's coalesce window is a full second, which
	 * would make a clock that ticks look like a clock that sticks. It is
	 * also the rate the tick above checks the minute at.
	 */
	.refresh = NEXUS_REFRESH_NORMAL,
	.btn_short = NEXUS_ACTION_BACK,
	.btn_long = NEXUS_ACTION_HOME,
};
