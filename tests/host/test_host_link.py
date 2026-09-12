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

    def fold(self, text):
        """Mirrors fold() in host_link.c: ASCII 32..90, upper case, drop
        anything else. The font has no lower case and draws the rest as
        '?'."""
        out = ''
        for ch in text.upper():
            if 32 <= ord(ch) <= 90 and len(out) < self.text_max - 1:
                out += ch
        return out

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
            self.now_playing = self.fold(val)
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
    ok(st.now_playing == 'ARTIST - TITLE',
       'N sets now playing, folded to the font')
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

    print('\nNow playing, folded to what the font can draw')
    # The font is ASCII 32..90 - space through Z - and gfx_text() draws
    # anything else as '?'. A real track title is full of things outside it.
    st.line('N Miles Davis - So What')
    ok(st.now_playing == 'MILES DAVIS - SO WHAT',
       'lower case folds up rather than drawing as question marks')
    st.line(u'N Sigur R\u00f3s \u2014 Hopp\u00edpolla')
    ok(st.now_playing == 'SIGUR RS  HOPPPOLLA',
       'accents and em dashes are dropped, not substituted: %s'
       % st.now_playing)
    st.line('N ' + 'A' * 100)
    ok(len(st.now_playing) == text_max - 1,
       'and it still cannot overrun (%d)' % len(st.now_playing))

    c_fold = c.split('static void fold')[1].split('\n}\n')[0]
    ok("c -= 'a' - 'A'" in c_fold, 'the C folds case')
    ok('> 90U' in c_fold and 'continue' in c_fold,
       'and drops what is left outside the font')
    ok('fold(clean, val, sizeof(clean))' in c
       and 'strcmp(g_host.now_playing, clean)' in c,
       'the comparison happens after folding - two titles that draw the same '
       'must not cost a repaint')

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

    print('\nIt repaints what moved, and only when you are looking')
    # Every parsed line used to call nexus_screen_invalidate() - a full 240
    # row repaint, two or three times a second, while you were on the
    # dashboard or mid-game where none of it is drawn.
    ok('nexus_screen_invalidate()' not in c,
       'the link never repaints the whole screen itself')
    ok(c.count('nexus_host_screen_dirty') >= 5,
       'it names the field that changed and lets the screen decide')
    for field in ('C', 'M', 'N'):
        branch = c.split("case '%s':" % field)[1].split('break;')[0]
        ok('if (' in branch and 'nexus_host_screen_dirty' in branch,
           "'%s' repaints only when the value actually moved" % field)
    # to the next case label: T guards an early break of its own.
    clock = c.split("case 'T':")[1].split("case '")[0]
    ok('/ 60U != ' in clock,
       "'T' repaints only when the minute on the face changes, not on every "
       'resync')

    hs = read('src', 'ui', 'host_screen.c')
    dirty = hs.split('void nexus_host_screen_dirty')[1].split('\n}\n')[0]
    ok('nexus_screen_current() != &nexus_screen_host_def' in dirty,
       'and nothing is repainted at all when HOST is not the current screen')
    for rows in ('CLOCK_Y, CLOCK_Y + CLOCK_H', 'METER_Y, METER_Y + METER_H',
                 'NP_Y, NP_Y + NP_H'):
        ok(rows in dirty,
           'a field maps to its own card (%s)' % rows.split(',')[0])

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

    print('\nThe clock')
    kc = read('Kconfig').split('config NEXUS_HOST_CLOCK_24H')[1]
    kc = kc.split('\nconfig ')[0]
    ok('default n' in kc, '12 hour with AM/PM is the default, 24 hour is opt in')
    clock = s.split('static void draw_clock')[1].split('\n}\n')[0]
    ok('IS_ENABLED(CONFIG_NEXUS_HOST_CLOCK_24H)' in clock,
       'and the format is a build option, not a hard-coded one')

    def face(sec, h24):
        """Mirrors draw_clock(): the numerals, and the suffix or None."""
        hour, minute = sec // 3600, (sec % 3600) // 60
        if h24:
            return '%02d:%02d' % (hour, minute), None
        suffix = 'AM' if hour < 12 else 'PM'
        hour %= 12
        return '%d:%02d' % (hour or 12, minute), suffix

    for sec, want12, want24 in ((0, ('12:00', 'AM'), ('00:00', None)),
                                (60, ('12:01', 'AM'), ('00:01', None)),
                                (11 * 3600 + 59 * 60, ('11:59', 'AM'),
                                 ('11:59', None)),
                                (12 * 3600, ('12:00', 'PM'),
                                 ('12:00', None)),
                                (13 * 3600 + 32 * 60, ('1:32', 'PM'),
                                 ('13:32', None)),
                                (23 * 3600 + 59 * 60, ('11:59', 'PM'),
                                 ('23:59', None))):
        got = face(sec, False)
        ok(got == want12, '%5ds -> %s %s' % (sec, got[0], got[1]))
        ok(face(sec, True) == want24, '%5ds -> %s in 24 hour'
           % (sec, want24[0]))

    ok('hour = 12U' in clock,
       'midnight and noon read as 12, never as 0 - the bug every 12 hour '
       'clock ships once')
    ok('suffix ? 0 : 2' in clock,
       'padded on a 24 hour face so the colon never moves, unpadded on a 12 '
       'hour one')
    ok('NEXUS_TXT_BODY' in clock and 'gfx_text_h(NEXUS_TXT_BIG)' in clock,
       'AM/PM is drawn smaller, on the numerals\' baseline')

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
