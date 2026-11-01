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

/*
 * Equal-tempered pitches, only the ones actually used.
 *
 * Everything sits in octaves 5-7 on purpose, and this is the one change that
 * makes the most audible difference to the whole build.
 *
 * A passive piezo is not a speaker with a flat response - it is a mechanical
 * resonator, and its output peaks somewhere around 2-4 kHz. Drive it at 500 Hz
 * and it is quiet and dull no matter what duty cycle you use; drive it near
 * resonance and the same 50% square wave is dramatically louder and brighter.
 * That is why snake-module's effects live on B6 and E7 rather than in the
 * middle of a piano, and why NEXUS's old C5-G6 tables sounded weak on the
 * exact same hardware.
 *
 * Octave 4 is gone entirely. Nothing down there was audible enough to be worth
 * the flash.
 */
#define G4 392
#define C6 1047
#define E6 1319
#define G6 1568
#define A6 1760
#define C7 2093
#define D7 2349
#define E7 2637
#define F7 2794
#define G7 3136
#define REST 0

/*
 * Startup: an original chiptune fanfare.
 *
 * Written for this project rather than borrowed - the obvious references are
 * somebody's trademark or somebody's copyright, and a keyboard dongle is not
 * worth either. The idiom is the 8-bit one it is meant to evoke: a springy
 * dotted pickup, a rising triad answered a fourth higher, then a two-note
 * cadence that lands hard on the tonic.
 *
 * Short notes with 20-30 ms of silence between them, because a passive buzzer
 * has no envelope of its own - the gaps ARE the articulation, and without
 * them a run of notes smears into one tone.
 *
 * Pitched an octave above where it started, into the buzzer's loud band. Same
 * tune, considerably more of it actually reaching the room.
 *
 * About 1.7 s, comfortably inside the 3.5 s splash it plays under.
 */
static const struct note s_startup[]   = {
	{E6,90},{REST,25},{E6,90},{REST,25},{G6,150},{REST,30},
	{C7,110},{REST,20},{E7,110},{REST,20},{G7,220},{REST,60},
	{F7,110},{REST,20},{D7,110},{REST,20},
	{G6,90},{REST,25},{C7,340},
	N_END
};

static const struct note s_select[]    = { {A6,25}, N_END };
static const struct note s_back[]      = { {E6,25}, N_END };
static const struct note s_connect[]   = { {G6,60},{C7,90}, N_END };
static const struct note s_disconnect[]= { {C7,60},{G6,90}, N_END };
static const struct note s_menu_open[] = { {E6,30},{A6,45}, N_END };
static const struct note s_menu_sel[]  = { {C7,45}, N_END };
static const struct note s_game_start[]= { {C6,60},{G6,60},{C7,110}, N_END };
static const struct note s_pause[]     = { {A6,50},{E6,80}, N_END };
static const struct note s_resume[]    = { {E6,50},{A6,80}, N_END };

/* Game over is the one thing allowed to end low - a fall to G4 reads as
 * "that's over" in a way no bright pitch does, and it is the last thing you
 * hear rather than something that has to cut through play. */
static const struct note s_game_over[] = { {G6,110},{E6,110},{C6,110},{G4,260}, N_END };

/* Gameplay blips. Short, and now high enough to be heard over a keyboard.
 * They still descend move > rotate > drop so the three stay distinct. */
static const struct note s_move[]      = { {E6,14}, N_END };
static const struct note s_rotate[]    = { {A6,18}, N_END };
static const struct note s_drop[]      = { {C6,40}, N_END };
static const struct note s_line[]      = { {C6,45},{G6,45},{C7,70}, N_END };
static const struct note s_tetris[]    = { {C6,40},{E6,40},{G6,40},{C7,40},{E7,120}, N_END };
static const struct note s_level[]     = { {G6,50},{C7,50},{E7,110}, N_END };

/*
 * Split halves. Four cues that have to be told apart in a second, so they are
 * systematic rather than four unrelated jingles: rising = arrived, falling =
 * gone, and the left half sits a fifth below the right one. Learn one and you
 * know all four.
 */
static const struct note s_half_l_conn[] = { {C6,55},{REST,15},{G6,90}, N_END };
static const struct note s_half_l_gone[] = { {G6,55},{REST,15},{C6,110}, N_END };
static const struct note s_half_r_conn[] = { {G6,55},{REST,15},{D7,90}, N_END };
static const struct note s_half_r_gone[] = { {D7,55},{REST,15},{G6,110}, N_END };

/*
 * Sleep and wake. Deliberately slower and softer than the half cues - this
 * fires when nothing is happening, so it should read as a sigh rather than an
 * alert.
 */
static const struct note s_sleep[] = {
	{A6,90},{REST,25},{E6,90},{REST,25},{C6,200}, N_END };
static const struct note s_wake[] = {
	{C6,70},{REST,20},{E6,70},{REST,20},{A6,160}, N_END };

#if IS_ENABLED(CONFIG_NEXUS_BUDDY)
/*
 * Buddy's cues, one per emotion, all inside the buzzer's loud band.
 *
 * A glide is written out as steps rather than given an engine of its own:
 * at 20-35 ms a step the ear hears a slide, and the player that already
 * exists plays it, so not one existing table had to change shape.
 */
#define D6 1175
/* "Mm-hm": two even beats, the face at rest. */
static const struct note s_b_neutral[] = { {G6,70},{REST,40},{G6,70}, N_END };
static const struct note s_b_happy[] = { {G6,60},{C7,60},{E7,110}, N_END };
/* C7 down to C6 over 420 ms, a semitone a step. */
static const struct note s_b_sad[] = {
	{2093,35},{1976,35},{1865,35},{1760,35},{1661,35},{1568,35},
	{1480,35},{1397,35},{1319,35},{1245,35},{1175,35},{1047,35}, N_END };
static const struct note s_b_angry[] = {
	{C6,45},{D6,45},{C6,45},{D6,45},{C6,45},{D6,45}, N_END };
/* E6 up to G7 in 160 ms. */
static const struct note s_b_surprised[] = {
	{1319,20},{1493,20},{1690,20},{1913,20},{2166,20},{2452,20},
	{2776,20},{3136,20}, N_END };
static const struct note s_b_sleepy[] = { {A6,120},{G6,140},{E6,240}, N_END };
static const struct note s_b_love[] = {
	{C7,70},{E7,70},{C7,70},{E7,70}, N_END };
static const struct note s_b_thinking[] = {
	{D7,40},{REST,200},{D7,40}, N_END };
/* G6 up to D7 in 120 ms. */
static const struct note s_b_listening[] = {
	{1568,20},{1700,20},{1843,20},{1998,20},{2166,20},{2349,20}, N_END };
#endif

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
	[NEXUS_SOUND_HALF_L_CONNECT]    = s_half_l_conn,
	[NEXUS_SOUND_HALF_L_DISCONNECT] = s_half_l_gone,
	[NEXUS_SOUND_HALF_R_CONNECT]    = s_half_r_conn,
	[NEXUS_SOUND_HALF_R_DISCONNECT] = s_half_r_gone,
	[NEXUS_SOUND_SLEEP]             = s_sleep,
	[NEXUS_SOUND_WAKE]              = s_wake,
#if IS_ENABLED(CONFIG_NEXUS_BUDDY)
	[NEXUS_SOUND_BUDDY_NEUTRAL]     = s_b_neutral,
	[NEXUS_SOUND_BUDDY_HAPPY]       = s_b_happy,
	[NEXUS_SOUND_BUDDY_SAD]         = s_b_sad,
	[NEXUS_SOUND_BUDDY_ANGRY]       = s_b_angry,
	[NEXUS_SOUND_BUDDY_SURPRISED]   = s_b_surprised,
	[NEXUS_SOUND_BUDDY_SLEEPY]      = s_b_sleepy,
	[NEXUS_SOUND_BUDDY_LOVE]        = s_b_love,
	[NEXUS_SOUND_BUDDY_THINKING]    = s_b_thinking,
	[NEXUS_SOUND_BUDDY_LISTENING]   = s_b_listening,
#endif
};

/*
 * Which category each effect belongs to, so UI and game sound toggle apart.
 *
 * Bounded at both ends on purpose. An open-ended `>= GAME_START` silently
 * swept up everything added to the end of the enum later, which is how the
 * split-half and sleep chirps - status sounds, nothing to do with games -
 * ended up gated behind CONFIG_NEXUS_GAME_SOUND.
 */
static bool effect_enabled(enum nexus_sound id)
{
	bool game = (id >= NEXUS_SOUND_GAME_START &&
		     id <= NEXUS_SOUND_TETRIS_LEVEL);

	if (game) {
		return IS_ENABLED(CONFIG_NEXUS_GAME_SOUND);
	}
	return IS_ENABLED(CONFIG_NEXUS_UI_SOUND);
}

static const struct note *g_seq;
static uint8_t g_pos;
static bool g_muted;

#if IS_ENABLED(CONFIG_NEXUS_BUDDY)
/* What is playing is one of Buddy's, and anything may cut it. */
static bool g_soft;
#endif

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

#if IS_ENABLED(CONFIG_NEXUS_BUDDY)
	bool soft = id >= NEXUS_SOUND_BUDDY_NEUTRAL;

	if (soft && g_seq != NULL && !g_soft) {
		return; /* never over a real effect */
	}
	g_soft = soft;
#endif

	/* Last effect wins. Queueing them would lag behind fast gameplay and
	 * sound worse than simply cutting to the newest event. */
	/*
	 * Position first, then the sequence. step() runs on the system work
	 * queue and can preempt this: with the old order it could observe the
	 * new (short) sequence still carrying the old sequence's index and
	 * read past the end of it. Zeroing first means every interleaving
	 * sees a valid index, because index 0 is valid for every sequence.
	 */
	g_pos = 0;
	g_seq = s_table[id];
	k_work_reschedule(&g_step, K_NO_WAIT);
}

#if IS_ENABLED(CONFIG_NEXUS_BUDDY)
/* The babble's one note, rewritten per blip. Position first, as above. */
static struct note g_blip[3];

void nexus_sound_blip(uint16_t hz, uint16_t ms, uint16_t hz_end)
{
	if (g_muted || !nexus_buzzer_ready() || !IS_ENABLED(CONFIG_NEXUS_UI_SOUND)) {
		return;
	}
	if (g_seq != NULL && !g_soft) {
		return;
	}

	uint16_t first = hz_end ? ms / 2U : ms;

	g_pos = 0;
	g_blip[0] = (struct note){ hz, first };
	g_blip[1] = (struct note){ hz_end, hz_end ? (uint16_t)(ms - first) : 0U };
	g_blip[2] = (struct note)N_END;
	g_soft = true;
	g_seq = g_blip;
	k_work_reschedule(&g_step, K_NO_WAIT);
}
#endif

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
