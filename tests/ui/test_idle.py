#!/usr/bin/env python3
"""Blanking the display when nobody is there.

The rule has three parts and each one was a bug before it was a rule:

  - Idle is ZMK's activity state, not a timer since the last button press.
    The dongle has no switches; typing happens on the halves, and the only
    thing that sees it is ZMK's own activity module. A timer counting from
    the last local input blanks the screen while you type.

  - A game is never idle. The player is looking at it precisely when they
    are not pressing anything.

  - Blanking has to mean something on hardware with the backlight strapped
    to VCC, which is what the supplied wiring does. There is no light to
    switch there, so the panel is blanked instead.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')

bad = []


def ok(cond, why):
    print(('  ok    ' if cond else '  FAIL  ') + why)
    if not cond:
        bad.append(why)


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding='utf-8') as f:
        return f.read()


def main():
    s = read('src', 'ui', 'screen.c')
    status_h = read('include', 'nexus', 'status.h')
    status_c = read('src', 'status', 'status.c')
    events = read('src', 'status', 'zmk_events.c')

    print('Idle is ZMK saying the user went away')
    ok('user_active' in status_h and 'NEXUS_STATUS_ACTIVE' in status_h,
       'the model carries whether ZMK thinks anyone is there')
    ok('.user_active = true,' in status_c,
       'and it starts true - a build with no activity event must behave as '
       'though the user is present, not as though they have gone')
    sub = events.split('ZMK_LISTENER')[-1]
    ok('ZMK_SUBSCRIPTION(nexus_status, zmk_activity_state_changed);' in sub,
       'the subscription exists')
    m = re.search(r'#if[^\n]*\n\s*ZMK_SUBSCRIPTION\(nexus_status, '
                  r'zmk_activity_state_changed\)', events)
    ok(m is None,
       'and is unconditional - it used to be behind the sound option, which '
       'is not a thing the display timeout should depend on')

    idle = s.split('static void idle_display')[1].split('\n}\n')[0]
    ok('nexus_status_get()->user_active' in idle,
       'the rule reads that state rather than timing local input')
    ok('g_idle_since' in idle and 'MAX(g_last_input, g_idle_since)' in idle,
       'and counts from whichever came last: ZMK going idle, or the button')

    print('\nA game is never idle')
    ok('NEXUS_REFRESH_FAST' in idle and 'set_dimmed(false)' in idle,
       'a FAST screen is woken, not blanked')

    print('\nOff means off')
    ok('CONFIG_NEXUS_BACKLIGHT_TIMEOUT_S == 0' in idle,
       'a timeout of 0 disables the whole rule')
    dim = s.split('static void set_dimmed')[-1].split('\n}\n')[0]
    ok('CONFIG_NEXUS_BACKLIGHT_TIMEOUT_S == 0' in dim,
       'and cannot be blanked by any other path either')

    print('\nWhat blanking does on each wiring')
    ok('NEXUS_BACKLIGHT_FIXED' in dim and 'nexus_display_sleep(dim)' in dim,
       'strapped backlight: the panel is blanked')
    ok('nexus_display_backlight_set(dim ? 0 : 100)' in dim,
       'switchable backlight: the light goes off instead')
    ok('g_dimmed == dim' in dim,
       'and the transition runs once, not once per tick down the SPI bus')
    ok('nexus_screen_invalidate();' in dim,
       'waking repaints, so nothing a game drew in the dark survives')

    print('\nNothing is pushed at a dark panel')
    render = s.split('static void render_dirty')[1].split('\n}\n')[0]
    ok('if (g_dimmed) {' in render,
       'render_dirty() returns early while blanked')

    print('\nAny input wakes it')
    now = s.split('void nexus_screen_render_now')[1].split('\n}\n')[0]
    ok('set_dimmed(false)' in now and 'g_last_input = k_uptime_get()' in now,
       'through the one function that exists for input, so the press that '
       'wakes the screen still does its job')

    print('\nThe option says what it now does')
    kc = read('Kconfig').split('config NEXUS_BACKLIGHT_TIMEOUT_S')[1]
    kc = kc.split('\nconfig ')[0]
    ok('Studio' not in kc,
       'no claim about ZMK Studio - nothing has ever implemented that')
    ok('activity' in kc.lower(), 'it explains what idle means')
    ok('strapped to VCC' in kc,
       'and what blanking can and cannot do on the supplied wiring')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
