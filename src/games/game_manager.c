/*
 * Game manager (Section 38).
 *
 * Owns the registry, the active game, and high-score persistence. The UI asks
 * it for names and scores; it never learns what a tetromino is (Section 38).
 * Adding a game is one entry in games[] plus its own file.
 */

#include <nexus/game.h>
#include <nexus/sound.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include "../nexus_priv.h"

LOG_MODULE_DECLARE(nexus, CONFIG_NEXUS_LOG_LEVEL);

#if IS_ENABLED(CONFIG_NEXUS_TETRIS)
extern const struct nexus_game nexus_game_tetris;
#endif
#if IS_ENABLED(CONFIG_NEXUS_SNAKE)
extern const struct nexus_game nexus_game_snake;
#endif
#if IS_ENABLED(CONFIG_NEXUS_BREAKOUT)
extern const struct nexus_game nexus_game_breakout;
#endif

static const struct nexus_game *const games[] = {
#if IS_ENABLED(CONFIG_NEXUS_TETRIS)
	&nexus_game_tetris,
#endif
#if IS_ENABLED(CONFIG_NEXUS_SNAKE)
	&nexus_game_snake,
#endif
#if IS_ENABLED(CONFIG_NEXUS_BREAKOUT)
	&nexus_game_breakout,
#endif
	/* Terminator. A zero-length array is not valid C, and the Game Center
	 * can legitimately be built with every game turned off. */
	NULL,
};

#define GAME_COUNT (ARRAY_SIZE(games) - 1)

/* Parallel to games[]: one high score each, loaded from settings at boot. */
static uint32_t highscores[ARRAY_SIZE(games)];

static const struct nexus_game *g_active;

static uint8_t g_speed = NEXUS_GAME_SPEED_DEFAULT;

uint8_t nexus_game_speed(void)
{
	return g_speed;
}

void nexus_game_speed_set(uint8_t speed)
{
	if (speed < NEXUS_GAME_SPEED_MIN) {
		speed = NEXUS_GAME_SPEED_MIN;
	}
	if (speed > NEXUS_GAME_SPEED_MAX) {
		speed = NEXUS_GAME_SPEED_MAX;
	}
	g_speed = speed;
}

const char *nexus_game_speed_name(void)
{
	static const char *const names[] = { "SLOW", "EASY", "NORMAL", "FAST",
					     "INSANE" };

	return names[g_speed - NEXUS_GAME_SPEED_MIN];
}

/* IS_ENABLED, not #ifdef: CONFIG_NEXUS_SNAKE_WRAP does not exist at all
 * when Snake is not built, and this still has to compile and hold a value. */
static bool g_snake_wrap = IS_ENABLED(CONFIG_NEXUS_SNAKE_WRAP);

bool nexus_snake_wrap(void)
{
	return g_snake_wrap;
}

void nexus_snake_wrap_set(bool wrap)
{
	g_snake_wrap = wrap;
}

uint8_t nexus_game_count(void)
{
	return (uint8_t)GAME_COUNT;
}

const struct nexus_game *nexus_game_at(uint8_t index)
{
	return (index < GAME_COUNT) ? games[index] : NULL;
}

static int index_of(const struct nexus_game *game)
{
	for (size_t i = 0; i < GAME_COUNT; i++) {
		if (games[i] == game) {
			return (int)i;
		}
	}
	return -1;
}

const struct nexus_game *nexus_game_active(void)
{
	return g_active;
}

int nexus_game_launch(uint8_t index)
{
	const struct nexus_game *game = nexus_game_at(index);

	if (game == NULL) {
		return -EINVAL;
	}

	if (g_active) {
		nexus_game_stop();
	}

	g_active = game;
	game->start();
	nexus_sound_play(NEXUS_SOUND_GAME_START);
	return 0;
}

void nexus_game_draw(void)
{
	if (g_active && g_active->draw) {
		g_active->draw();
	}
}

void nexus_game_stop(void)
{
	if (g_active == NULL) {
		return;
	}

	/* Banking the score on exit means quitting mid-round still counts, and
	 * it is the only write path besides game over. */
	nexus_game_submit_score(g_active, g_active->score());
	g_active->stop();
	g_active = NULL;
}

void nexus_game_tick(void)
{
	if (g_active && g_active->update) {
		g_active->update();
	}
}

bool nexus_game_input(enum nexus_action action)
{
	return g_active ? g_active->input(action) : false;
}

void nexus_game_pause(void)
{
	if (g_active && g_active->state() == NEXUS_GAME_RUNNING) {
		g_active->pause();
		nexus_sound_play(NEXUS_SOUND_GAME_PAUSE);
	}
}

void nexus_game_resume(void)
{
	if (g_active && g_active->state() == NEXUS_GAME_PAUSED) {
		g_active->resume();
		nexus_sound_play(NEXUS_SOUND_GAME_RESUME);
	}
}

enum nexus_game_state nexus_game_state(void)
{
	return g_active ? g_active->state() : NEXUS_GAME_IDLE;
}

uint32_t nexus_game_highscore(const struct nexus_game *game)
{
	int i = index_of(game);

	return (i < 0) ? 0 : highscores[i];
}

void nexus_game_submit_score(const struct nexus_game *game, uint32_t score)
{
	int i = index_of(game);

	if (i < 0 || score <= highscores[i]) {
		return;
	}

	highscores[i] = score;

#if IS_ENABLED(CONFIG_NEXUS_GAME_HIGHSCORE_PERSIST)
	/* Only on an actual record, never per frame - flash wear is real and
	 * settings writes block (Section 107). Concatenated by hand rather than
	 * with snprintf: this runs on the display thread, whose stack is sized
	 * for the UI and not for picolibc's double-capable formatter. */
	static const char prefix[] = "nexus/games/";
	char key[40];
	size_t n = sizeof(prefix) - 1;

	memcpy(key, prefix, n);
	strncpy(&key[n], game->id, sizeof(key) - n - 1);
	key[sizeof(key) - 1] = '\0';

	int ret = settings_save_one(key, &score, sizeof(score));

	if (ret) {
		LOG_WRN("high score save failed (%d); runtime value kept", ret);
	}
#endif
}

#if IS_ENABLED(CONFIG_NEXUS_GAME_HIGHSCORE_PERSIST)
static int highscore_set(const char *name, size_t len, settings_read_cb read_cb,
			 void *cb_arg)
{
	for (size_t i = 0; i < GAME_COUNT; i++) {
		if (settings_name_steq(name, games[i]->id, NULL)) {
			uint32_t v = 0;

			if (len != sizeof(v)) {
				return -EINVAL;
			}
			if (read_cb(cb_arg, &v, sizeof(v)) < 0) {
				return -EIO;
			}
			highscores[i] = v;
			return 0;
		}
	}

	/* A leftover key from a game that is no longer built - ignore it
	 * rather than failing the whole settings load. */
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nexus_games, "nexus/games", NULL, highscore_set,
			       NULL, NULL);
#endif

static int games_init(void)
{
	nexus_health_set_games(GAME_COUNT > 0);
	return 0;
}

SYS_INIT(games_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
