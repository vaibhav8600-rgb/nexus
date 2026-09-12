/*
 * HOST - what the machine on the other end of the cable knows.
 *
 * The clock is the reason this screen exists. The dongle has no RTC, so the
 * only real time it will ever see is the time a companion tells it, and a
 * clock is the one thing a screen on a desk is expected to have.
 *
 * Everything here has to survive the companion not being there, which is the
 * normal state: no cable, a phone on the other end, or simply nothing
 * running. Unknown draws as dashes in the muted colour, never as a stale
 * number - a CPU meter frozen at 3% is worse than an empty one, because it
 * looks like it is working.
 */

#include <nexus/gfx.h>
#include <nexus/host.h>
#include <nexus/nexus.h>
#include <nexus/screen.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>

#include "../nexus_priv.h"

/* ---- layout ------------------------------------------------------------- */

#define CLOCK_Y 22
#define CLOCK_H 74
#define METER_Y 106
#define METER_H 54
#define NP_Y 170
#define NP_H 52

static void draw_clock(void)
{
	const struct nexus_theme *t = nexus_theme();
	uint32_t sec = nexus_host_clock();
	char buf[8];

	nexus_draw_card(NEXUS_PAD, CLOCK_Y, NEXUS_CONTENT_W, CLOCK_H);

	if (sec == UINT32_MAX) {
		nexus_draw_caption_c(GFX_W / 2, CLOCK_Y + 20, "NO HOST CLOCK");
		gfx_text_c(GFX_W / 2, CLOCK_Y + 36, "--:--", NEXUS_TXT_BIG,
			   t->muted, GFX_OPAQUE);
		return;
	}

	/*
	 * HH:MM as one string, padded, so the colon never moves. Seconds are
	 * deliberately absent: they would repaint this card once a second
	 * forever, which is the one thing a screen that sits idle must not do.
	 */
	int n = 0;

	gfx_utoa(sec / 3600U, buf, sizeof(buf), 2);
	while (buf[n]) {
		n++;
	}
	buf[n++] = ':';
	gfx_utoa((sec % 3600U) / 60U, &buf[n], (int)sizeof(buf) - n, 2);

	nexus_draw_caption_c(GFX_W / 2, CLOCK_Y + 12, "HOST TIME");
	gfx_text_c(GFX_W / 2, CLOCK_Y + 28, buf, NEXUS_TXT_BIG, t->value,
		   GFX_OPAQUE);
}

static void draw_meter(int x, int w, const char *label, uint8_t pct)
{
	const struct nexus_theme *t = nexus_theme();
	char buf[8];

	nexus_draw_card(x, METER_Y, w, METER_H);
	nexus_draw_caption(x + 8, METER_Y + 7, label);

	if (pct == NEXUS_HOST_UNKNOWN) {
		gfx_text(x + 8, METER_Y + 21, "--", NEXUS_TXT_VALUE, t->muted,
			 GFX_OPAQUE);
		return;
	}

	gfx_text(x + 8, METER_Y + 19, gfx_utoa(pct, buf, sizeof(buf), 0),
		 NEXUS_TXT_VALUE, t->value, GFX_OPAQUE);
	/* Over 80% in the warning colour: the number is the reading, the
	 * colour is the judgement, same as the battery meters. */
	nexus_draw_meter(x + 8, METER_Y + METER_H - 12, w - 16, 6, pct,
			 pct >= 80 ? t->warning : t->accent);
}

static void draw_now_playing(void)
{
	const struct nexus_theme *t = nexus_theme();
	const struct nexus_host *h = nexus_host();

	nexus_draw_card(NEXUS_PAD, NP_Y, NEXUS_CONTENT_W, NP_H);
	nexus_draw_caption(NEXUS_PAD + 8, NP_Y + 7, "NOW PLAYING");

	if (!h->link || h->now_playing[0] == '\0') {
		gfx_text(NEXUS_PAD + 8, NP_Y + 24, "--", NEXUS_TXT_BODY,
			 t->muted, GFX_OPAQUE);
		return;
	}

	/*
	 * Drop a size rather than clip, the same rule layer names follow: the
	 * string is the user's, it can be any length, and half a track title
	 * at body size reads worse than all of it at caption size.
	 */
	int scale = gfx_text_w(h->now_playing, NEXUS_TXT_BODY) <=
			    NEXUS_CONTENT_W - 16
			    ? NEXUS_TXT_BODY
			    : NEXUS_TXT_CAPTION;

	gfx_text(NEXUS_PAD + 8, NP_Y + 24, h->now_playing, scale, t->value,
		 GFX_OPAQUE);
}

static void host_draw(void)
{
	const struct nexus_theme *t = nexus_theme();
	const struct nexus_host *h = nexus_host();

	nexus_draw_ground();

	if (gfx_hits(6, gfx_text_h(NEXUS_TXT_LABEL))) {
		nexus_draw_label(NEXUS_PAD, 6, "HOST");
		/* The link light, in the words rather than a dot: this screen
		 * is mostly empty without it, and "why is it empty" should be
		 * answered on the screen asking the question. */
		const char *state = h->link ? "LINKED" : "NO LINK";

		gfx_text(GFX_W - NEXUS_PAD - gfx_text_w(state, NEXUS_TXT_LABEL),
			 6, state, NEXUS_TXT_LABEL,
			 h->link ? t->accent : t->muted, GFX_OPAQUE);
	}

	draw_clock();
	draw_meter(NEXUS_PAD, 106, "CPU", h->cpu);
	draw_meter(NEXUS_PAD + 112, 110, "MEM", h->mem);
	draw_now_playing();
}

const struct nexus_screen nexus_screen_host_def = {
	.name = "HOST",
	.draw = host_draw,
	/*
	 * NORMAL, not IDLE. The companion pushes about once a second and each
	 * line invalidates; IDLE's coalesce window is a full second, which
	 * would make a clock that ticks look like a clock that sticks.
	 */
	.refresh = NEXUS_REFRESH_NORMAL,
	.btn_short = NEXUS_ACTION_BACK,
	.btn_long = NEXUS_ACTION_HOME,
};
