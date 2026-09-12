# NEXUS Smart ZMK Dongle - reusable ZMK/Zephyr module.
# Nothing here is compiled unless CONFIG_NEXUS=y, so peripheral (keyboard half)
# builds pay zero flash and zero RAM for the dongle UI (Section 81). See
# docs/architecture.md.

if(NOT CONFIG_NEXUS)
  return()
endif()

zephyr_include_directories(include)
zephyr_library_named(nexus)

# ---------------------------------------------------------------------------
# ZMK's own headers.
#
# zmk/display.h, zmk/event_manager.h, zmk/keymap.h and drivers/behavior.h all
# live in <zmk>/app/include. Zephyr puts that on the *application* target's
# include path, but a module library like this one does not inherit it, so
# without this every NEXUS file that talks to ZMK fails with:
#
#     fatal error: zmk/display.h: No such file or directory
#
# APPLICATION_SOURCE_DIR is <workspace>/zmk/app in a ZMK build. Scoped to this
# library rather than added globally with zephyr_include_directories(), since
# nothing outside NEXUS needs it.
# ---------------------------------------------------------------------------
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

# src/ui/screen.c includes <lvgl.h> for the one lv_obj_t ZMK's status-screen
# API hands back. Zephyr's LVGL module normally exports this globally; adding
# it explicitly costs nothing and matches what the Snake shield had to do.
if(DEFINED ZEPHYR_LVGL_MODULE_DIR)
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
endif()

zephyr_library_sources(
  src/nexus.c
  src/action.c
  src/settings.c
  src/status/status.c
  src/status/split_conn.c
  src/status/zmk_events.c
)

zephyr_library_sources_ifdef(CONFIG_NEXUS_BUTTON   src/hal/nexus_button.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_DISPLAY  src/hal/nexus_backlight.c)

zephyr_library_sources_ifdef(CONFIG_NEXUS_SOUND
  src/hal/nexus_buzzer.c
  src/sound/sound.c
)

zephyr_library_sources_ifdef(CONFIG_NEXUS_DISPLAY
  src/ui/gfx.c
  src/ui/theme.c
  src/ui/widgets.c
  src/ui/screen.c
  src/ui/home.c
  src/ui/menus.c
)
zephyr_library_sources_ifdef(CONFIG_NEXUS_SPLASH      src/ui/splash.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_GAME_CENTER src/ui/game_center.c)

zephyr_library_sources_ifdef(CONFIG_NEXUS_GAMES  src/games/game_manager.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_SNAKE  src/games/snake/snake.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_PACMAN src/games/pacman/pacman.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_JUMPER src/games/jumper/jumper.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_INVADERS src/games/invaders/invaders.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_PONG   src/games/pong/pong.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_BREAKOUT
  src/games/breakout/breakout.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_TETRIS
  src/games/tetris/tetris_core.c
  src/games/tetris/tetris.c
)

zephyr_library_sources_ifdef(CONFIG_NEXUS_BEHAVIOR_ACTION src/behaviors/behavior_nexus_action.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_HOST_LINK src/host/host_link.c)
zephyr_library_sources_ifdef(CONFIG_NEXUS_HOST_LINK src/ui/host_screen.c)

# ---------------------------------------------------------------------------
# Splash asset pipeline (Section 20): user PNG -> RGB565 C array at build time.
#
# The integrator drops a PNG into their own zmk-config; the build turns it into
# firmware data. Nobody hand-generates C arrays (Section 20) and nobody edits
# NEXUS source to change artwork (Requirement C). Exactly one of the two
# branches below is compiled, so Section 21's priority - user asset first,
# built-in default second - is settled at link time.
# ---------------------------------------------------------------------------
if(CONFIG_NEXUS_SPLASH)
  # Priority: the integrator's image, then one bundled with the module, then
  # the drawn badge. Exactly one is compiled, and all three provide the same
  # nexus_splash_art symbol, so src/ui/splash.c never learns which it got.
  set(NEXUS_SPLASH_SRC "${CONFIG_NEXUS_SPLASH_IMAGE}")
  if(NEXUS_SPLASH_SRC STREQUAL "")
    set(NEXUS_SPLASH_SRC "${CONFIG_NEXUS_SPLASH_DEFAULT_IMAGE}")
    if(NOT NEXUS_SPLASH_SRC STREQUAL "" AND NOT IS_ABSOLUTE "${NEXUS_SPLASH_SRC}")
      # A module default is relative to the module, not to anyone's config.
      set(NEXUS_SPLASH_SRC "${CMAKE_CURRENT_LIST_DIR}/${NEXUS_SPLASH_SRC}")
    endif()
  endif()

  if(NEXUS_SPLASH_SRC STREQUAL "")
    zephyr_library_sources(assets/splash_default.c)
  else()
    if(NOT IS_ABSOLUTE "${NEXUS_SPLASH_SRC}")
      # Resolve against the user's config directory.
      #
      # ${ZMK_CONFIG}, not $ENV{ZMK_CONFIG}. ZMK's build passes it as a CMake
      # cache variable - `-DZMK_CONFIG=<path>` on the west build line, visible
      # in any build log - and never puts it in the environment. So the ENV
      # form was always empty, every relative path fell through to
      # APPLICATION_CONFIG_DIR (the app directory on a ZMK build, not the
      # config repo), and the first person to set CONFIG_NEXUS_SPLASH_IMAGE
      # would have hit the FATAL_ERROR below pointing into zmk/app.
      #
      # All three roots are tried, cache variable first, so a plain west build
      # that does export ZMK_CONFIG still works.
      set(_nexus_splash_roots "")
      if(DEFINED ZMK_CONFIG)
        list(APPEND _nexus_splash_roots "${ZMK_CONFIG}")
      endif()
      if(DEFINED ENV{ZMK_CONFIG})
        list(APPEND _nexus_splash_roots "$ENV{ZMK_CONFIG}")
      endif()
      if(DEFINED APPLICATION_CONFIG_DIR)
        list(APPEND _nexus_splash_roots "${APPLICATION_CONFIG_DIR}")
      endif()

      set(_nexus_splash_found "")
      foreach(_root ${_nexus_splash_roots})
        if(NOT _nexus_splash_found AND EXISTS "${_root}/${NEXUS_SPLASH_SRC}")
          set(_nexus_splash_found "${_root}/${NEXUS_SPLASH_SRC}")
        endif()
      endforeach()

      if(_nexus_splash_found)
        set(NEXUS_SPLASH_SRC "${_nexus_splash_found}")
      elseif(_nexus_splash_roots)
        # Resolve against the first root anyway, so the error below names a
        # real path rather than the bare relative one.
        list(GET _nexus_splash_roots 0 _nexus_splash_first)
        set(NEXUS_SPLASH_SRC "${_nexus_splash_first}/${NEXUS_SPLASH_SRC}")
      endif()
    endif()

    if(NOT EXISTS "${NEXUS_SPLASH_SRC}")
      message(FATAL_ERROR
        "CONFIG_NEXUS_SPLASH_IMAGE points at a missing file:\n"
        "  ${NEXUS_SPLASH_SRC}\n"
        "Searched under: ${_nexus_splash_roots}\n"
        "Paths are relative to your zmk-config's config/ directory.")
    endif()

    # Printed so the build log says which splash it actually took. Without it
    # the only symptom of a splash that did not get picked up is the default
    # one appearing, which looks like nothing happened at all.
    message(STATUS "NEXUS splash image: ${NEXUS_SPLASH_SRC}")

    set(NEXUS_SPLASH_GEN "${CMAKE_CURRENT_BINARY_DIR}/nexus_splash_user.c")
    add_custom_command(
      OUTPUT "${NEXUS_SPLASH_GEN}"
      COMMAND ${PYTHON_EXECUTABLE}
              "${CMAKE_CURRENT_LIST_DIR}/scripts/png2c.py"
              --symbol nexus_splash_pixels
              --max-size ${CONFIG_NEXUS_SPLASH_MAX_DIM}
              "${NEXUS_SPLASH_SRC}" "${NEXUS_SPLASH_GEN}"
      DEPENDS "${NEXUS_SPLASH_SRC}" "${CMAKE_CURRENT_LIST_DIR}/scripts/png2c.py"
      COMMENT "NEXUS: converting splash ${NEXUS_SPLASH_SRC} -> RGB565"
      VERBATIM)
    zephyr_library_sources("${NEXUS_SPLASH_GEN}")
  endif()
endif()
