/*
 * Sound engine (Sections 16-17, 48, 108).
 *
 * Effects are short note sequences synthesised on the buzzer - no samples, no
 * flash cost beyond a few hundred bytes of tables. Playback advances on a
 * delayed work item on the SYSTEM queue, deliberately not the display queue:
 * a slow LVGL frame must never stretch a note, and a note must never delay a
 * frame. Sound is the lowest priority subsystem in the build (Section 67).
 */

#include <nexus/sound.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

struct note {
	uint16_t freq;  /* Hz, 0 = rest */
	uint16_t ms;
};

#define N_END { 0, 0 }

/* Equal-tempered pitches, only the ones actually used. */
#define C4 262
#define E4 330
#define G4 392
#define A4 440
#define C5 523
#define D5 587
#define E5 659
#define F5 698
#define G5 784
#define A5 880
#define B5 988
#define C6 1047
#define D6 1175
#define E6 1319
#define G6 1568
#define REST 0

/*
 * Startup: a real phrase rather than four rising notes.
 *
 * C major arpeggio up to the octave, a beat of air, then a bright
 * G-B-D-G turnaround that lands back on the tonic - about 1.6 s, which fits
 * comfortably inside the default 3.5 s splash. Sequenced on a work item like
 * every other effect, so it never holds up the boot it plays over
 * (Sections 18, 108).
 */
static const struct note s_startup[]   = {
	{C5,110},{E5,110},{G5,110},{C6,190},
	{REST,60},
	{G5,90},{B5,90},{D6,90},{G6,260},
	{REST,50},
	{C6,300},
	N_END
};
static const struct note s_select[]    = { {A5,25}, N_END };
static const struct note s_back[]      = { {E5,25}, N_END };
static const struct note s_connect[]   = { {G5,60},{C6,90}, N_END };
static const struct note s_disconnect[]= { {C6,60},{G5,90}, N_END };
static const struct note s_menu_open[] = { {E5,30},{A5,45}, N_END };
static const struct note s_menu_sel[]  = { {C6,45}, N_END };
static const struct note s_game_start[]= { {C5,60},{G5,60},{C6,110}, N_END };
static const struct note s_pause[]     = { {A5,50},{E5,80}, N_END };
static const struct note s_resume[]    = { {E5,50},{A5,80}, N_END };
static const struct note s_game_over[] = { {G5,110},{E5,110},{C5,110},{G4,220}, N_END };
static const struct note s_move[]      = { {E4,14}, N_END };
static const struct note s_rotate[]    = { {A4,18}, N_END };
static const struct note s_drop[]      = { {C4,40}, N_END };
static const struct note s_line[]      = { {C5,45},{G5,45},{C6,70}, N_END };
static const struct note s_tetris[]    = { {C5,40},{E5,40},{G5,40},{C6,40},{E6,120}, N_END };
static const struct note s_level[]     = { {G5,50},{C6,50},{E6,110}, N_END };

/* Index order must match enum nexus_sound. */
static const struct note *const s_table[NEXUS_SOUND_COUNT] = {
	[NEXUS_SOUND_STARTUP]       = s_startup,
	[NEXUS_SOUND_SELECT]        = s_select,
	[NEXUS_SOUND_BACK]          = s_back,
	[NEXUS_SOUND_CONNECT]       = s_connect,
	[NEXUS_SOUND_DISCONNECT]    = s_disconnect,
	[NEXUS_SOUND_MENU_OPEN]     = s_menu_open,
	[NEXUS_SOUND_MENU_SELECT]   = s_menu_sel,
	[NEXUS_SOUND_GAME_START]    = s_game_start,
	[NEXUS_SOUND_GAME_PAUSE]    = s_pause,
	[NEXUS_SOUND_GAME_RESUME]   = s_resume,
	[NEXUS_SOUND_GAME_OVER]     = s_game_over,
	[NEXUS_SOUND_TETRIS_MOVE]   = s_move,
	[NEXUS_SOUND_TETRIS_ROTATE] = s_rotate,
	[NEXUS_SOUND_TETRIS_DROP]   = s_drop,
	[NEXUS_SOUND_TETRIS_LINE]   = s_line,
	[NEXUS_SOUND_TETRIS_TETRIS] = s_tetris,
	[NEXUS_SOUND_TETRIS_LEVEL]  = s_level,
};

/* Which category each effect belongs to, so UI and game sound toggle apart. */
static bool effect_enabled(enum nexus_sound id)
{
	bool game = id >= NEXUS_SOUND_GAME_START;

	if (game) {
		return IS_ENABLED(CONFIG_NEXUS_GAME_SOUND);
	}
	return IS_ENABLED(CONFIG_NEXUS_UI_SOUND);
}

static const struct note *g_seq;
static uint8_t g_pos;
static bool g_muted;

static void step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_step, step);

static void step(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_seq == NULL || g_seq[g_pos].ms == 0) {
		g_seq = NULL;
		nexus_buzzer_tone(0);
		return;
	}

	const struct note *n = &g_seq[g_pos++];

	nexus_buzzer_tone(n->freq);
	k_work_schedule(&g_step, K_MSEC(n->ms));
}

void nexus_sound_play(enum nexus_sound id)
{
	if (g_muted || id >= NEXUS_SOUND_COUNT || !nexus_buzzer_ready()) {
		return;
	}
	if (!effect_enabled(id)) {
		return;
	}

	/* Last effect wins. Queueing them would lag behind fast gameplay and
	 * sound worse than simply cutting to the newest event. */
	g_seq = s_table[id];
	g_pos = 0;
	k_work_reschedule(&g_step, K_NO_WAIT);
}

void nexus_sound_stop(void)
{
	g_seq = NULL;
	k_work_cancel_delayable(&g_step);
	nexus_buzzer_tone(0);
}

void nexus_sound_set_enabled(bool enabled)
{
	g_muted = !enabled;
	if (g_muted) {
		nexus_sound_stop();
	}
}

bool nexus_sound_enabled(void)
{
	return !g_muted && nexus_buzzer_ready();
}
