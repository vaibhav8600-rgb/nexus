/*
 * Screen stack, refresh scheduler and ZMK display entry point (Sec. 54-56, 60).
 *
 * NEXUS renders as ZMK's custom status screen. That is deliberate: ZMK already
 * owns the display device, a tick and a work queue, so the whole UI runs with
 * no NEXUS thread and no second framebuffer (Sections 89, 122, 141-F).
 *
 * LVGL is still nominally the host - ZMK's API hands back an lv_obj_t - but it
 * is given one empty object and draws nothing. Pixels come from the strip
 * compositor, which is the only way a 240x240 RGB565 UI fits next to BLE and
 * Studio on this part (see include/nexus/gfx.h).
 */

#include <nexus/display.h>
#include <nexus/gfx.h>
#include <nexus/screen.h>
#include <nexus/sound.h>
#include <nexus/theme.h>
#include <nexus/widgets.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>
#include <zmk/display/status_screen.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

/* HOME -> GAME_CENTER -> GAME, or HOME -> SETTINGS -> DIAGNOSTICS. Four is the
 * deepest real path; a fifth push means a screen forgot to pop. */
#define SCREEN_STACK_DEPTH 4

/*
 * LVGL paints its (empty) screen once when ZMK loads it. Starting the
 * compositor after that avoids a race for the SPI bus at boot, and 120 ms is
 * invisible next to the splash.
 */
#define FIRST_FRAME_DELAY_MS 120

static const struct nexus_screen *g_stack[SCREEN_STACK_DEPTH];
static uint8_t g_depth;
static enum nexus_refresh g_rate_override;
static int64_t g_last_input;

/* Dirty region as a row range; empty when hi <= lo. */
static int g_dirty_lo = GFX_H;
static int g_dirty_hi;

static void tick_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_tick, tick_work_fn);

static void paint_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_paint, paint_work_fn);

const struct nexus_screen *nexus_screen_current(void)
{
	return g_depth ? g_stack[g_depth - 1] : NULL;
}

static uint32_t rate_ms(enum nexus_refresh r)
{
	switch (r) {
	case NEXUS_REFRESH_FAST:
		return CONFIG_NEXUS_UI_REFRESH_FAST_MS;
	case NEXUS_REFRESH_NORMAL:
		return CONFIG_NEXUS_UI_REFRESH_NORMAL_MS;
	default:
		return CONFIG_NEXUS_UI_REFRESH_IDLE_MS;
	}
}

static uint32_t current_rate_ms(void)
{
	const struct nexus_screen *cur = nexus_screen_current();
	enum nexus_refresh r = cur ? cur->refresh : NEXUS_REFRESH_IDLE;

	if (g_rate_override > r) {
		r = g_rate_override;
	}
	return rate_ms(r);
}

/* ---- painting ---------------------------------------------------------- */

static void draw_scene(void *ctx)
{
	ARG_UNUSED(ctx);

	const struct nexus_screen *cur = nexus_screen_current();

	nexus_draw_ground();

	if (cur && cur->draw) {
		cur->draw();
	}
}

#if IS_ENABLED(CONFIG_NEXUS_DEBUG)
static uint16_t g_frames;
static uint8_t g_fps;
static int64_t g_fps_since;

uint8_t nexus_screen_fps(void)
{
	return g_fps;
}

static void count_frame(void)
{
	int64_t now = k_uptime_get();

	g_frames++;
	if (now - g_fps_since >= MSEC_PER_SEC) {
		g_fps = (uint8_t)MIN(g_frames, 255);
		g_frames = 0;
		g_fps_since = now;
	}
}
#else
#define count_frame() ((void)0)
#endif

static void render_dirty(void)
{
	if (g_dirty_hi <= g_dirty_lo || !gfx_ready()) {
		return;
	}

	int lo = g_dirty_lo;
	int hi = g_dirty_hi;

	/* Cleared before drawing, so an invalidate raised from inside draw()
	 * survives instead of being wiped by this frame. */
	g_dirty_lo = GFX_H;
	g_dirty_hi = 0;

	gfx_render_range(draw_scene, NULL, lo, hi);
	count_frame();
}

static void paint_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	render_dirty();
}

static void arm_paint(void)
{
	/*
	 * k_work_SCHEDULE, not RESCHEDULE. Reschedule restarts the delay on
	 * every call, so a stream of events closer together than the coalesce
	 * window pushes the deadline out forever and the repaint never lands -
	 * which is exactly how a dashboard ends up frozen while you type.
	 * Schedule is a throttle: the first event arms the timer, later ones
	 * are no-ops, and the accumulated dirty range paints on time.
	 */
	k_work_schedule_for_queue(nexus_workq(), &g_paint,
				  K_MSEC(current_rate_ms()));
}

void nexus_screen_invalidate_rows(int y0, int y1)
{
	if (y0 < g_dirty_lo) {
		g_dirty_lo = y0;
	}
	if (y1 > g_dirty_hi) {
		g_dirty_hi = y1;
	}
	arm_paint();
}

void nexus_screen_invalidate(void)
{
	nexus_screen_invalidate_rows(0, GFX_H);
}

void nexus_screen_render_now(void)
{
	/*
	 * The coalesce window in arm_paint() is there to throttle *status*
	 * events, which arrive on their own schedule and do not need to be on
	 * screen this instant. User input is the opposite: on an IDLE screen
	 * the window is a full second, so a cursor move sat there waiting for
	 * a timer that exists to rate-limit battery reports. That is the whole
	 * of the "settings feels laggy" report.
	 *
	 * Safe to render straight from the caller because every caller is
	 * already on the NEXUS work queue - the same context paint_work_fn
	 * runs in, so this cannot race it. The armed timer still fires and
	 * finds a clean dirty range, which is a no-op.
	 */
	render_dirty();
}

/* ---- scheduler --------------------------------------------------------- */

static void idle_backlight(void)
{
	if (CONFIG_NEXUS_BACKLIGHT_TIMEOUT_S == 0 ||
	    nexus_display_backlight_mode() == NEXUS_BACKLIGHT_FIXED) {
		return;
	}

	/* Never blank mid-game: the player is looking at it even when they are
	 * not pressing anything (Section 66). */
	const struct nexus_screen *cur = nexus_screen_current();

	if (cur && cur->refresh == NEXUS_REFRESH_FAST) {
		return;
	}

	bool idle = (k_uptime_get() - g_last_input) >
		    (int64_t)CONFIG_NEXUS_BACKLIGHT_TIMEOUT_S * MSEC_PER_SEC;

	nexus_display_backlight_set(idle ? 0 : 100);
}

static void tick_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	const struct nexus_screen *cur = nexus_screen_current();

	if (cur && cur->tick) {
		cur->tick();
	}

	idle_backlight();
	k_work_reschedule_for_queue(nexus_workq(), &g_tick,
				    K_MSEC(current_rate_ms()));
}

void nexus_screen_request_refresh(enum nexus_refresh rate)
{
	g_rate_override = rate;
	g_last_input = k_uptime_get();
	k_work_reschedule_for_queue(nexus_workq(), &g_tick,
				    K_MSEC(current_rate_ms()));
}

/* ---- stack ------------------------------------------------------------- */

static void enter(const struct nexus_screen *screen)
{
	g_rate_override = NEXUS_REFRESH_IDLE;
	g_last_input = k_uptime_get();

	if (screen->enter) {
		screen->enter();
	}

	/*
	 * Paint the new screen immediately rather than waiting out the coalesce
	 * window. We are already on the display queue, and a navigation press
	 * that takes 200 ms to show anything feels broken.
	 */
	g_dirty_lo = 0;
	g_dirty_hi = GFX_H;
	render_dirty();

	k_work_reschedule_for_queue(nexus_workq(), &g_tick,
				    K_MSEC(current_rate_ms()));
}

static void leave(void)
{
	const struct nexus_screen *cur = nexus_screen_current();

	if (cur && cur->exit) {
		cur->exit();
	}
}

void nexus_screen_push(const struct nexus_screen *screen)
{
	if (!screen) {
		return;
	}
	if (g_depth >= SCREEN_STACK_DEPTH) {
		LOG_WRN("screen stack full, replacing top instead of pushing");
		nexus_screen_replace(screen);
		return;
	}

	leave();
	g_stack[g_depth++] = screen;
	enter(screen);
}

void nexus_screen_pop(void)
{
	if (g_depth <= 1) {
		return;
	}

	leave();
	g_depth--;
	enter(g_stack[g_depth - 1]);
}

void nexus_screen_replace(const struct nexus_screen *screen)
{
	if (!screen) {
		return;
	}

	leave();
	if (g_depth == 0) {
		g_depth = 1;
	}
	g_stack[g_depth - 1] = screen;
	enter(screen);
}

void nexus_screen_home(void)
{
	/* Unwind properly: every screen above home still gets its exit(), so
	 * status subscriptions and game timers are released. */
	while (g_depth > 1) {
		leave();
		g_depth--;
	}
	nexus_screen_replace(&nexus_screen_home_def);
}

/* ---- boot -------------------------------------------------------------- */

static const struct nexus_screen *default_screen(void)
{
#if IS_ENABLED(CONFIG_NEXUS_DEFAULT_SCREEN_GAME_CENTER) &&                     \
	IS_ENABLED(CONFIG_NEXUS_GAME_CENTER)
	return &nexus_screen_game_center_def;
#elif IS_ENABLED(CONFIG_NEXUS_DEFAULT_SCREEN_DIAGNOSTICS)
	return &nexus_screen_diagnostics_def;
#else
	return &nexus_screen_home_def;
#endif
}

#if IS_ENABLED(CONFIG_NEXUS_SPLASH)
static void splash_done(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Only if the splash is still up: pressing the button during it already
	 * moved on, and this timer must not yank the user back. */
	if (nexus_screen_current() == &nexus_screen_splash_def) {
		nexus_screen_replace(default_screen());
	}
}
static K_WORK_DELAYABLE_DEFINE(g_splash_done, splash_done);
#endif

static void first_frame(void)
{
	if (gfx_init() != 0) {
		/* Requirement A: the keyboard does not care that the panel is
		 * missing, and neither does anything else in this file. */
		nexus_health_set_display(false);
		return;
	}

	nexus_health_set_display(true);
	nexus_health_set_backlight(nexus_backlight_init() == 0);

	g_depth = 1;

#if IS_ENABLED(CONFIG_NEXUS_SPLASH)
	if (CONFIG_NEXUS_SPLASH_DURATION_MS > 0) {
		g_stack[0] = &nexus_screen_splash_def;
		enter(g_stack[0]);
		k_work_schedule_for_queue(nexus_workq(), &g_splash_done,
					  K_MSEC(CONFIG_NEXUS_SPLASH_DURATION_MS));
		return;
	}
#endif

	g_stack[0] = default_screen();
	enter(g_stack[0]);
}

/*
 * Deliberately an LVGL timer and not a k_work.
 *
 * This function is called from inside zmk_display_status_screen(), before ZMK
 * has necessarily started its display work queue - submitting to a queue that
 * is not running yet is not something to find out about on hardware. An LVGL
 * timer runs from lv_task_handler(), which is by definition the thread that
 * owns the panel, so the first frame lands on the right thread at a time when
 * the queue is definitely up. Everything after this point uses nexus_workq().
 *
 * set_repeat_count(1) is the portable one-shot: lv_timer_del was renamed in
 * LVGL 9, this spelling works in both.
 */
static void first_frame_timer(lv_timer_t *timer)
{
	ARG_UNUSED(timer);
	first_frame();
}

/*
 * ZMK calls this exactly once during display init and loads the result as the
 * active LVGL screen. Returning a bare object is the point: LVGL owns nothing
 * on this panel beyond one initial clear, and every pixel after that comes
 * from the compositor.
 */
lv_obj_t *zmk_display_status_screen(void)
{
	lv_timer_t *t = lv_timer_create(first_frame_timer,
					FIRST_FRAME_DELAY_MS, NULL);

	if (t) {
		lv_timer_set_repeat_count(t, 1);
	} else {
		LOG_ERR("no LVGL timer for first frame; UI stays dark");
	}

	return lv_obj_create(NULL);
}
