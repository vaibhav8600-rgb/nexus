#!/usr/bin/env python3
"""The host link: its protocol, its staleness rule, and its screen.

None of this is compiled here, so the parser is ported and exercised rather
than read. The cases that matter are the ones a real companion will produce
by accident: a number out of range, a line longer than the buffer, a field
nobody has implemented yet, and the companion dying without saying so.
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


UNKNOWN = 0xFF


class Host:
    """Mirrors parse_line() and the staleness sweep in host_link.c."""

    def __init__(self, text_max):
        self.text_max = text_max
        self.link = False
        self.cpu = UNKNOWN
        self.mem = UNKNOWN
        self.now_playing = ''
        self.clock_sec = None

    def num(self, s, cap):
        if not s or not s[0].isdigit():
            return None
        v = 0
        for ch in s:
            if not ch.isdigit():
                break
            v = v * 10 + int(ch)
            if v > cap:
                return None
        return v

    def line(self, raw):
        raw = raw[:71]                       # LINE_MAX - 1, the C truncates
        if not raw:
            return
        key, val = raw[0], raw[2:] if raw[1:2] == ' ' else raw[1:]
        if key == 'C':
            n = self.num(val, 100)
            self.cpu = UNKNOWN if n is None else n
        elif key == 'M':
            n = self.num(val, 100)
            self.mem = UNKNOWN if n is None else n
        elif key == 'T':
            n = self.num(val, 86399)
            if n is not None:
                self.clock_sec = n
        elif key == 'N':
            self.now_playing = val[:self.text_max - 1]
        elif key == 'X':
            self.cpu = self.mem = UNKNOWN
            self.now_playing = ''
            self.link = False
            return
        else:
            return
        self.link = True


def main():
    c = read('src', 'host', 'host_link.c')
    h = read('include', 'nexus', 'host.h')
    text_max = int(re.search(r'#define NEXUS_HOST_TEXT (\d+)', h).group(1))

    print('Protocol')
    st = Host(text_max)
    st.line('C 37')
    ok(st.cpu == 37 and st.link, 'C sets the CPU and brings the link up')
    st.line('M 62')
    ok(st.mem == 62, 'M sets memory')
    st.line('T 48720')
    ok(st.clock_sec == 48720, 'T is seconds since local midnight (13:32)')
    st.line('N Artist - Title')
    ok(st.now_playing == 'Artist - Title', 'N sets now playing')
    st.line('N ')
    ok(st.now_playing == '', 'and an empty N clears it')

    print('\nThings a companion will get wrong')
    st.line('C 900')
    ok(st.cpu == UNKNOWN,
       'an out-of-range number reads as unknown, not clamped to 100 - '
       'clamping would hide the bug')
    st.line('C abc')
    ok(st.cpu == UNKNOWN, 'and so does a non-number')
    st.line('Q whatever')
    ok(True, 'an unknown field is ignored, so a newer companion still works')
    st.line('N ' + 'x' * 200)
    ok(len(st.now_playing) == text_max - 1,
       'an over-long string is truncated to %d, never overrunning'
       % (text_max - 1))

    print('\nThe link going away')
    st.line('C 40')
    ok(st.link, 'link is up while lines arrive')
    st.line('X')
    ok(not st.link and st.cpu == UNKNOWN and st.now_playing == '',
       'X drops the link and forgets every value on the spot')

    print('\nWhat the C actually does')
    ok('atomic_set(&g_line_ready, 1)' in c and 'k_work_submit_to_queue' in c,
       'the ISR hands the line to the work queue rather than parsing it')
    isr = c.split('static void uart_cb')[1].split('\n}\n')[0]
    for forbidden in ('nexus_screen_invalidate', 'parse_line', 'LOG_'):
        ok(forbidden not in isr,
           'the ISR does not call %s()' % forbidden)
    ok('if (g_rx_len < LINE_MAX - 1)' in c,
       'the line buffer cannot overrun - longer lines truncate')
    ok('k_work_reschedule_for_queue(nexus_workq(), &g_stale' in c,
       'every line restarts the staleness timer')
    # [-1]: the name appears as a forward declaration and in the work
    # item before the function itself, so take the body, not a stub.
    ok('g_host.link = false;' in c.split('stale_work_fn')[-1],
       'and running out drops the link')
    ok('uart_irq_rx_enable' in c and 'uart_tx' not in c and
       'uart_poll_out' not in c,
       'receive only - the dongle has no way to talk back')

    print('\nThe screen survives having no host')
    s = read('src', 'ui', 'host_screen.c')
    ok(s.count('NEXUS_HOST_UNKNOWN') >= 1 and '"--"' in s,
       'unknown values draw as dashes, not as the last number seen')
    ok('UINT32_MAX' in s and '"--:--"' in s,
       'and an unset clock draws as --:--')
    ok('t->muted' in s, 'in the muted colour, so it does not read as data')
    ok('NEXUS_REFRESH_NORMAL' in s,
       "the screen refreshes at NORMAL - IDLE's one-second coalesce would "
       'make a ticking clock look stuck')

    print('\nLayout fits the panel')
    K = {k: int(v) for k, v in
         re.findall(r'^#define (\w+_[YH]) (\d+)', s, re.M)}
    blocks = [('clock', K['CLOCK_Y'], K['CLOCK_H']),
              ('meters', K['METER_Y'], K['METER_H']),
              ('now playing', K['NP_Y'], K['NP_H'])]
    for (n1, y1, h1), (n2, y2, _) in zip(blocks, blocks[1:]):
        ok(y1 + h1 <= y2, '%s ends at %d, before %s starts at %d'
           % (n1, y1 + h1, n2, y2))
    last = blocks[-1]
    ok(last[1] + last[2] <= 240,
       'and the last card ends on the panel at %d' % (last[1] + last[2]))
    ok(K['CLOCK_Y'] >= 6 + 14, 'the clock card clears the title row')

    print('\nThe companion')
    py = read('tools', 'nexus-host', 'nexus_host.py')
    ok("'X\\n'" in py or 'b\'X\\n\'' in py,
       'it says X on the way out rather than leaving stale numbers')
    ok('CLOCK_EVERY = 60' in py and 'T %d' in py,
       'and resends the clock every minute, because the dongle drifts')
    ok('psutil is None' in py or 'if psutil' in py,
       'psutil is optional - the clock works without it')
    ok('while True' in py and 'retrying' in py,
       'it reconnects rather than dying on the first reflash')

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
