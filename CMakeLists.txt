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
zephyr_library_sources_ifdef(CONFIG_NEXUS_TETRIS
  src/games/tetris/tetris_core.c
  src/games/tetris/tetris.c
)

zephyr_library_sources_ifdef(CONFIG_NEXUS_BEHAVIOR_ACTION src/behaviors/behavior_nexus_action.c)

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
  set(NEXUS_SPLASH_SRC "${CONFIG_NEXUS_SPLASH_IMAGE}")

  if(NEXUS_SPLASH_SRC STREQUAL "")
    zephyr_library_sources(assets/splash_default.c)
  else()
    if(NOT IS_ABSOLUTE "${NEXUS_SPLASH_SRC}")
      # Resolve against the user's config directory. ZMK sets ZMK_CONFIG for
      # config-repo builds; APPLICATION_CONFIG_DIR covers a plain west build.
      if(DEFINED ENV{ZMK_CONFIG} AND EXISTS "$ENV{ZMK_CONFIG}/${NEXUS_SPLASH_SRC}")
        set(NEXUS_SPLASH_SRC "$ENV{ZMK_CONFIG}/${NEXUS_SPLASH_SRC}")
      else()
        set(NEXUS_SPLASH_SRC "${APPLICATION_CONFIG_DIR}/${NEXUS_SPLASH_SRC}")
      endif()
    endif()

    if(NOT EXISTS "${NEXUS_SPLASH_SRC}")
      message(FATAL_ERROR
        "CONFIG_NEXUS_SPLASH_IMAGE points at a missing file:\n"
        "  ${NEXUS_SPLASH_SRC}\n"
        "Paths are relative to your zmk-config's config/ directory.")
    endif()

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
