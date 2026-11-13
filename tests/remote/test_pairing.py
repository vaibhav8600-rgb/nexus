#!/usr/bin/env python3
"""
The pairing window's rules, held in place by source checks.

The window is driven by Bluetooth callbacks and cannot run on a host, so this
checks that the three rules a state-machine audit found missing stay written
down: a passkey extends the window, a closed window leaves no pairing in
flight, and PAIR while a new pairing settles does not reopen it.

Run: python tests/remote/test_pairing.py
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
bad = []


def ok(cond, why):
    print('  %s  %s' % ('ok  ' if cond else 'FAIL', why))
    if not cond:
        bad.append(why)


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding='utf-8') as f:
        return f.read()


def body(src, signature):
    """One function's body. Not its forward declaration: that ends in ';'."""
    m = re.search(re.escape(signature) + r'[^;{]*\{(.*?)\n\}\n', src, re.S)
    return m.group(1) if m else ''


def main():
    print('The pairing window')
    c = re.sub(r'/\*.*?\*/', '', read('src', 'remote', 'remote.c'), flags=re.S)

    shown = body(c, 'static void passkey_display(')
    ok('conn == g_pair_conn' in shown and
       'k_work_reschedule(&g_pair_close, PAIR_WINDOW)' in shown,
       'a passkey on screen gives its own candidate a full window to type it')

    closing = body(c, 'static void pair_close_fn(')
    ok('stale == g_phone' in closing and 'bt_conn_disconnect(stale' in closing,
       'closing hangs up a candidate that is not a served phone, so no '
       'pairing can finish after the window has gone')
    ok('nexus_remote_screen_hide(NEXUS_REMOTE_VIEW_PASSKEY)' in closing,
       'and takes a passkey nobody can use off the screen')

    opening = body(c, 'static void pair_open_fn(')
    ok(re.search(r'if \(!g_just_paired\) \{\s*k_work_reschedule\(&g_pair_close, '
                 r'PAIR_WINDOW\)', opening),
       'PAIR while a new pairing settles does not reopen the window')

    hid = re.sub(r'/\*.*?\*/', '', read('src', 'remote', 'remote_hid.c'),
                 flags=re.S)
    press = hid.split('case REMOTE_KEY_PRESS:', 1)[1].split('case ', 1)[0]
    ok('remote_hid_keepalive();' in press,
       'a held key starts with a fresh watchdog, however long it queued')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
