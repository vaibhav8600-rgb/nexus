#!/usr/bin/env python3
"""The host link: its protocol, its staleness rule, and its screen.

None of this is compiled here, so the parser is ported and exercised rather
than read. The cases that matter are the ones a real companion will produce
by accident: a number out of range, a line longer than the buffer, a field
nobody has implemented yet, a burst of lines in one packet, and the companion
dying without saying so.
"""
import datetime
import importlib.util
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


def body(src, signature):
    """One C function's body, by its signature line. [-1]: the name may also
    appear in a forward declaration or a work-item macro before it."""
    return src.split(signature)[-1].split('\n}\n')[0]


UNKNOWN = 0xFF
U32_MAX = 0xFFFFFFFF


class Host:
    """Mirrors host_link.c: the ring, the line assembler, parse_line(), the
    clock and the date. @c now stands in for k_uptime_get(), in seconds."""

    RING = 256
    LINE_MAX = 72

    def __init__(self, text_max):
        self.text_max = text_max
        self.link = False
        self.cpu = UNKNOWN
        self.mem = UNKNOWN
        self.now_playing = ''
        self.artist = ''
        self.clock_sec = U32_MAX
        self.clock_at = 0
        self.day = U32_MAX
        self.now = 0
        self.ring = bytearray()
        self.line_buf = ''

    # -- the wire
    def isr(self, data):
        """uart_cb(): into the ring, dropping what does not fit."""
        room = self.RING - len(self.ring)
        self.ring += data[:room]

    def work(self):
        """parse_work_fn(): drain the ring, assemble, parse."""
        data, self.ring = bytes(self.ring), bytearray()
        for b in data:
            c = chr(b)
            if c == '\r':
                continue
            if c != '\n':
                if len(self.line_buf) < self.LINE_MAX - 1:
                    self.line_buf += c
                continue
            if self.line_buf:
                line, self.line_buf = self.line_buf, ''
                self.line(line)

    # -- the model
    def num(self, s, cap):
        """to_u32(): the whole value or nothing."""
        if not s or not '0' <= s[0] <= '9':
            return None
        v = 0
        for ch in s:
            if not '0' <= ch <= '9':
                return None
            v = v * 10 + int(ch)
            if v > cap:
                return None
        return v

    def fold(self, text):
        """fold(): ASCII 32..90, upper case, drop anything else."""
        out = ''
        for ch in text.upper():
            if 32 <= ord(ch) <= 90 and len(out) < self.text_max - 1:
                out += ch
        return out

    def clock(self):
        if self.clock_sec == U32_MAX:
            return U32_MAX
        return (self.clock_sec + self.now - self.clock_at) % 86400

    def days_since_clock(self):
        if self.clock_sec == U32_MAX:
            return 0
        return (self.clock_sec + self.now - self.clock_at) // 86400

    def today(self):
        if self.day == U32_MAX:
            return U32_MAX
        return self.day + self.days_since_clock()

    def line(self, raw):
        raw = raw[:self.LINE_MAX - 1]
        if not raw:
            return
        key, val = raw[0], raw[2:] if raw[1:2] == ' ' else raw[1:]
        if key in 'CM':
            n = self.num(val, 100)
            setattr(self, 'cpu' if key == 'C' else 'mem',
                    UNKNOWN if n is None else n)
        elif key == 'T':
            n = self.num(val, 86399)
            if n is None:
                return self._up()
            was, today = self.clock(), self.today()
            self.clock_sec, self.clock_at = n, self.now
            if today != U32_MAX:
                if was != U32_MAX and was > n + 43200:
                    today += 1
                elif was != U32_MAX and n > was + 43200 and today:
                    today -= 1
                self.day = today
        elif key == 'N':
            self.now_playing = self.fold(val)
        elif key == 'A':
            self.artist = self.fold(val)
        elif key == 'D':
            n = self.num(val, 200000)
            if n is None:
                return self._up()
            past = self.days_since_clock()
            self.day = n - past if n > past else 0
        elif key == 'X':
            self.cpu = self.mem = UNKNOWN
            self.now_playing = self.artist = ''
            self.link = False
            return
        else:
            return
        self._up()

    def _up(self):
        self.link = True


def civil(days):
    """format_date() in host_screen.c, ported line for line."""
    z = days + 719468
    doe = z % 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    mday = doy - (153 * mp + 2) // 5 + 1
    mon = mp + 2 if mp < 10 else mp - 10
    wday = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][(days + 4) % 7]
    return '%s %d %s' % (wday, mday, ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN',
                                      'JUL', 'AUG', 'SEP', 'OCT', 'NOV',
                                      'DEC'][mon])


def text_w(s, scale):
    return len(s) * 6 * scale - scale if s else 0


def fit_text(s, scale, room):
    """fit_text() in host_screen.c."""
    n = (room + scale) // (text_w('0', scale) + scale)
    return s if len(s) <= n else s[:max(n - 2, 0)] + '..'


def protocol(text_max):
    print('Protocol')
    st = Host(text_max)
    st.line('C 37')
    ok(st.cpu == 37 and st.link, 'C sets the CPU and brings the link up')
    st.line('M 62')
    ok(st.mem == 62, 'M sets memory')
    st.line('T 48720')
    ok(st.clock_sec == 48720, 'T is seconds since local midnight (13:32)')
    st.line('D 20709')
    ok(st.today() == 20709, 'D is the local date, days since 1970')
    st.line('N So What')
    st.line('A Miles Davis')
    ok(st.now_playing == 'SO WHAT' and st.artist == 'MILES DAVIS',
       'N is the title and A the artist, each folded to the font')
    st.line('N ')
    ok(st.now_playing == '', 'and an empty N clears it')

    print('\nThings a companion will get wrong')
    st.line('C 900')
    ok(st.cpu == UNKNOWN,
       'an out-of-range number reads as unknown, not clamped to 100 - '
       'clamping would hide the bug')
    st.line('C abc')
    ok(st.cpu == UNKNOWN, 'and so does a non-number')
    st.line('C 37abc')
    ok(st.cpu == UNKNOWN,
       'and so does a number with junk after it - "37abc" is a mangled '
       'line, and a mangled line must not become a plausible 37')
    st.line('Q whatever')
    ok(True, 'an unknown field is ignored, so a newer companion still works')
    st.line('N ' + 'x' * 200)
    ok(len(st.now_playing) == text_max - 1,
       'an over-long string is truncated to %d, never overrunning'
       % (text_max - 1))
    st.line(u'N Sigur Rós — Hoppípolla')
    ok(st.now_playing == 'SIGUR RS  HOPPPOLLA',
       'accents and em dashes are dropped, not substituted: %s'
       % st.now_playing)

    print('\nA burst arrives whole')
    # The handoff this replaced kept one line per interrupt and dropped the
    # rest. A companion writes its lines back to back, so they land in one
    # packet and one interrupt - and every line after the first was lost:
    # the date, the title, the clock that is only resent once a minute.
    st = Host(text_max)
    st.isr(b'T 48720\nD 20709\nC 37\nM 62\nN NEON NIGHTS\nA RETROSYNTH\n')
    st.work()
    ok((st.clock_sec, st.today(), st.cpu, st.mem, st.now_playing, st.artist)
       == (48720, 20709, 37, 62, 'NEON NIGHTS', 'RETROSYNTH'),
       'six lines in one interrupt, six fields set')
    st = Host(text_max)
    for part in (b'C 4', b'1\r\nM', b' 5', b'0\n'):
        st.isr(part)
    st.work()
    ok(st.cpu == 41 and st.mem == 50,
       'a line split across interrupts, with CRLF, still parses')
    st = Host(text_max)
    # 258 bytes into a 256 byte ring: "C 37\n" loses its "7\n".
    st.isr(b'N ' + b'Z' * 250 + b'\nC 37\n')
    st.work()
    ok(st.now_playing.startswith('ZZ') and st.cpu == UNKNOWN,
       'a flood past the ring drops bytes rather than overrunning it')
    st.isr(b'M 62\n')
    st.work()
    ok(st.cpu == UNKNOWN,
       'the torn "C 3" joins the next line as "C 3M 62" and is rejected '
       'whole - a lenient parser would have shown CPU 3%')
    st.isr(b'C 37\nM 62\n')
    st.work()
    ok(st.cpu == 37 and st.mem == 62, 'and the lines after that are fine')

    print('\nThe date turns over with the clock')
    st = Host(text_max)
    st.line('T %d' % (23 * 3600 + 59 * 60))
    st.line('D 20709')
    st.now += 120
    ok(st.clock() == 60 and st.today() == 20710,
       'two minutes past 23:59 is 00:01 on the next day, with no resync')
    st.now += 86400 * 3
    ok(st.today() == 20713, 'and three days later, three days later')
    st = Host(text_max)
    st.line('D 20709')
    st.line('T 48720')
    ok(st.today() == 20709, 'a D before any T is still that date')
    st = Host(text_max)
    st.line('T 86399')
    st.now += 2                      # the dongle is past midnight...
    st.line('D 20709')               # ...the host is not, yet
    ok(st.today() == 20709,
       'a D that lands just after the kernel clock\'s midnight keeps the '
       'host\'s date rather than the day after it')

    print('\nA T on its own keeps the date')
    # The heartbeat sends T alone, and so does `echo "T 48720"` by hand.
    # day is filed against the clock's own start, so a T that moved the
    # start without carrying the date put it back a day after midnight.
    st = Host(text_max)
    st.line('T %d' % (23 * 3600 + 59 * 60))
    st.line('D 20709')
    st.now += 120                    # 00:01 on the 14th, by the dongle
    st.line('T 60')                  # the host agrees, and sends no D
    ok(st.today() == 20710 and st.clock() == 60,
       'a resync after midnight, with no D, keeps the new date')
    st = Host(text_max)
    st.line('T 86390')
    st.line('D 20709')
    st.now += 5                      # the dongle says 23:59:55 on the 13th
    st.line('T 3')                   # the host is already at 00:00:03
    ok(st.today() == 20710,
       'the host past midnight and the dongle not: the date moves on')
    st = Host(text_max)
    st.line('T 86395')
    st.line('D 20709')
    st.now += 10                     # the dongle ran fast: 00:00:05, 14th
    st.line('T 86399')               # the host is still at 23:59:59
    ok(st.today() == 20709,
       'the dongle past midnight and the host not: the date goes back')
    st = Host(text_max)
    st.line('D 20709')
    st.line('T 100')
    st.line('T 200')
    ok(st.today() == 20709, 'and plain resyncs in the day change nothing')

    t_src = read('src', 'host', 'host_link.c').split("case 'T':")[1]
    t_src = t_src.split("case '")[0]
    ok('uint32_t today = nexus_host_day();' in t_src
       and t_src.index('nexus_host_day()') < t_src.index('g_host.clock_at =')
       and 'g_host.day = today;' in t_src and '43200U' in t_src,
       'the C reads the date before moving the clock, and carries it over')

    print('\nThe link going away')
    st.line('C 40')
    st.line('N X')
    ok(st.link, 'link is up while lines arrive')
    st.line('X')
    ok(not st.link and st.cpu == UNKNOWN and st.now_playing == ''
       and st.artist == '',
       'X drops the link and forgets load and track on the spot')
    ok(st.clock() != U32_MAX and st.today() != U32_MAX,
       'but not the clock or the date: those stay true while the dongle has '
       'power, so a companion that ran once this morning is enough for them')


def link_source(c, kconfig):
    print('\nWhat the C actually does')
    isr = body(c, 'static void uart_cb(const struct device *dev, '
                  'void *user_data)')
    ok('ring_buf_put(&g_rx_ring' in isr and 'k_spin_lock(&g_rx_lock)' in isr
       and 'k_work_submit_to_queue' in isr,
       'the ISR moves bytes into the ring, under its lock, and posts the work')
    for forbidden in ('nexus_screen_invalidate', 'parse_line', 'LOG_',
                      'g_line'):
        ok(forbidden not in isr, 'the ISR does not touch %s' % forbidden)
    work = body(c, 'static void parse_work_fn(struct k_work *work)\n{')
    ok('ring_buf_get(&g_rx_ring' in work and 'k_spin_lock(&g_rx_lock)' in work,
       'the work item drains the ring under the same lock')
    ok('if (g_line_len < LINE_MAX - 1)' in work,
       'the line buffer cannot overrun - longer lines truncate')
    ok('g_line_ready' not in c and 'atomic' not in c,
       'and the drop-if-busy handoff is gone entirely')
    kc = kconfig.split('config NEXUS_HOST_LINK\n')[1].split('\nconfig ')[0]
    ok('select RING_BUFFER' in kc, 'Kconfig selects the ring buffer it uses')
    to_u32 = body(c, 'static uint32_t to_u32(const char *s, uint32_t max)')
    ok("*s == '\\0' ? v : UINT32_MAX" in to_u32,
       'to_u32() rejects trailing junk rather than stopping at it')

    ok('k_work_reschedule_for_queue(nexus_workq(), &g_stale' in work,
       'every line restarts the staleness timer')
    ok('g_host.link = false;' in body(c, 'static void stale_work_fn('
                                         'struct k_work *work)\n{'),
       'and running out drops the link')
    ok('uart_irq_rx_enable' in c and 'uart_tx' not in c and
       'uart_poll_out' not in c,
       'receive only - the dongle has no way to talk back')

    c_fold = c.split('static void fold')[1].split('\n}\n')[0]
    ok("c -= 'a' - 'A'" in c_fold and '> 90U' in c_fold,
       'the C folds case and drops what is outside the font')
    xs = c.split("case 'X':")[1].split('return;')[0]
    ok('clock_sec' not in xs and 'g_host.day' not in xs,
       'X leaves the clock and date alone')

    print('\nIt repaints what moved, and only when you are looking')
    ok('nexus_screen_invalidate()' not in c,
       'the link never repaints the whole screen itself')
    for field, what in (('C', 'LOAD'), ('M', 'LOAD'), ('N', 'NP'),
                        ('A', 'NP'), ('D', 'CLOCK')):
        branch = c.split("case '%s':" % field)[1].split("case '")[0]
        ok('if (' in branch and 'NEXUS_HOST_F_%s' % what in branch,
           "'%s' repaints its card only when the value actually moved"
           % field)
    ok('strcmp(g_host.artist, clean)' in c,
       'A compares after folding, like N')
    clock = c.split("case 'T':")[1].split("case '")[0]
    ok('/ 60U != ' in clock,
       "'T' repaints only when the minute on the face changes")


def screen(s, events):
    print('\nThe screen')
    dirty = body(s, 'void nexus_host_screen_dirty(enum nexus_host_field '
                    'field)\n{')
    ok('nexus_screen_current() != &nexus_screen_host_def' in dirty,
       'nothing is repainted at all when HOST is not the current screen')
    for rows in ('CLOCK_Y, CLOCK_Y + CLOCK_H', 'METER_Y, METER_Y + METER_H',
                 'NP_Y, NP_Y + NP_H'):
        ok(rows in dirty,
           'a field maps to its own card (%s)' % rows.split(',')[0])

    draw = body(s, 'static void host_draw(void)\n{')
    ok('h->link ? h->cpu : NEXUS_HOST_UNKNOWN' in draw
       and 'h->link ? h->mem : NEXUS_HOST_UNKNOWN' in draw,
       'CPU and RAM draw as dashes once the link goes stale - they used to '
       'keep the last number, the exact thing the staleness rule forbids')
    np = body(s, 'static void draw_now_playing(const struct nexus_host *h)'
                 '\n{')
    ok("h->link && h->now_playing[0]" in np, 'and so does the track')
    ok(np.count('fit_text(buf') == 2,
       'title and artist are both cut to their room - text is not clipped '
       'to its card on this panel')

    print('\nThe clock moves on its own')
    ok('.tick = host_tick' in s and '.enter = host_enter' in s,
       'HOST has a tick')
    tick = body(s, 'static void host_tick(void)\n{')
    ok('clock_key()' in tick and 'CLOCK_Y, CLOCK_Y + CLOCK_H' in tick,
       'which repaints the clock card when the minute turns - before it, '
       'the card only repainted when a resync disagreed with it, so the '
       'clock on screen simply stopped')
    key = body(s, 'static uint32_t clock_key(void)\n{')
    ok('sec / 60U' in key and 'host_up_at' in key,
       'the key covers the host clock and the connected-for time')

    print('\nWith nothing installed on the host')
    clock = body(s, 'static void draw_clock(void)\n{')
    ok('st->host_up' in clock and 'host_up_at' in clock
       and '"SINCE HOST CONNECTED"' in clock,
       'a host with no companion still gets a clock card: how long it has '
       'been connected, which the dongle knows by itself')
    ok('"H"' in clock and '"M"' in clock,
       'as hours and minutes, never H:MM - it must not read as a time')
    ok('"--:--"' in clock and '"NO HOST"' in clock,
       'and with no host at all, dashes and the reason')
    ev = events.split('st->host_up = up;')[1][:200]
    ok('if (became)' in ev and 'st->host_up_at = k_uptime_get()' in ev,
       'the timestamp is taken where host_up rises')

    print('\nThe clock face')
    kc = read('Kconfig').split('config NEXUS_HOST_CLOCK_24H')[1]
    ok('default n' in kc.split('\nconfig ')[0],
       '12 hour with AM/PM is the default, 24 hour is opt in')
    ok('IS_ENABLED(CONFIG_NEXUS_HOST_CLOCK_24H)' in clock,
       'and the format is a build option')

    def face(sec, h24):
        """Mirrors draw_clock(): the numerals, and the suffix or None."""
        hour, minute = sec // 3600, (sec % 3600) // 60
        if h24:
            return '%02d:%02d' % (hour, minute), None
        suffix = 'AM' if hour < 12 else 'PM'
        hour %= 12
        return '%d:%02d' % (hour or 12, minute), suffix

    for sec, want12, want24 in ((0, ('12:00', 'AM'), '00:00'),
                                (11 * 3600 + 59 * 60, ('11:59', 'AM'),
                                 '11:59'),
                                (12 * 3600, ('12:00', 'PM'), '12:00'),
                                (13 * 3600 + 32 * 60, ('1:32', 'PM'),
                                 '13:32'),
                                (23 * 3600 + 59 * 60, ('11:59', 'PM'),
                                 '23:59')):
        ok(face(sec, False) == want12 and face(sec, True)[0] == want24,
           '%5ds -> %s %s, or %s' % (sec, want12[0], want12[1], want24))
    ok('hour = 12U' in clock and 'suffix ? 0 : 2' in clock,
       'midnight and noon read as 12; 24 hour is padded so the colon holds')
    ok('gfx_face_h(BIG) - gfx_face_h(1)' in s,
       'units sit on the numerals\' baseline')

    print('\nThe date')
    fmt = body(s, 'static void format_date(uint32_t days, char *buf)\n{')
    for const in ('719468U', '146097U', '1460U', '36524U', '146096U',
                  '153U', '(days + 4U) % 7U'):
        ok(const in fmt, 'format_date() is civil_from_days (%s)' % const)
    epoch = datetime.date(1970, 1, 1)
    wrong = []
    for d in list(range(0, 800)) + list(range(10900, 11400)) + \
            list(range(20000, 21000)) + list(range(47400, 47600)):
        want = (epoch + datetime.timedelta(days=d)).strftime('%a %d %b')
        want = want.upper().replace(' 0', ' ')
        if civil(d) != want:
            wrong.append((d, civil(d), want))
    ok(not wrong, 'and it agrees with the calendar across leap years, 2000 '
       'and 2100: %s' % (wrong[:3] or '2,500 days checked'))
    ok(civil(20709) == 'SUN 13 SEP', 'day 20709 is SUN 13 SEP (2026)')
    ok(len('WED 30 SEP') + 1 <= 16, 'the longest date fits its 16 byte buffer')


def layout(s):
    print('\nLayout fits the panel')
    K = {k: int(v) for k, v in
         re.findall(r'^#define (\w+) (\d+)', s, re.M)}
    pad, w = 9, 240
    content = w - 2 * pad
    ok(K['HDR_Y'] + 2 + K['PILL_H'] < K['CLOCK_Y'],
       'the header pill ends before the clock card')
    blocks = [('clock', K['CLOCK_Y'], K['CLOCK_H']),
              ('stats', K['METER_Y'], K['METER_H']),
              ('now playing', K['NP_Y'], K['NP_H'])]
    for (n1, y1, h1), (n2, y2, _) in zip(blocks, blocks[1:]):
        ok(y2 - (y1 + h1) == 7, '%s ends at %d, a 7px gap before %s'
           % (n1, y1 + h1, n2))
    ok(K['NP_Y'] + K['NP_H'] == w - pad,
       'and the last card ends on the bottom padding (%d)'
       % (K['NP_Y'] + K['NP_H']))
    ok(pad + 2 * K['STAT_W'] + 6 == w - pad,
       'two stat cards and their gutter are exactly the content width')

    big_adv = (10 + 1) * K['BIG']

    def runs_w(runs):
        total = 0
        for i, (txt, big) in enumerate(runs):
            if big:
                rw = sum(K['COLON_W'] if ch == ':' else big_adv
                         for ch in txt) - K['BIG']
            else:
                rw = len(txt) * 11 - 1
            total += rw + ((10 if big else 4) if i else 0)
        return total

    for runs in ([('12:59', True), ('PM', False)], [('23:59', True)],
                 [('999', True), ('H', False), ('59', True), ('M', False)]):
        ok(runs_w(runs) <= content - 16,
           'the widest clock line fits its card: %s is %dpx'
           % (''.join(r[0] for r in runs), runs_w(runs)))
    for line in ('SINCE HOST CONNECTED', 'WED 30 SEP'):
        tw = text_w(line, 1) + (len(line) - 1) * 2
        ok(tw <= content - 16, '"%s" tracked is %dpx' % (line, tw))
    # 9 face pixels of digit, 3 to the colon square, colon at y+12 and y+27
    ok(12 + 6 <= 21 <= 27 and 27 + 6 <= 14 * K['BIG'],
       'the drawn colon sits inside the numerals\' height')

    room_playing = content - (8 + K['ART'] + 10) - 8 - K['EQ_ROOM']
    for title in ('SO WHAT', 'PAHADO KE SHEHAR MEIN',
                  'A' * 39, 'SUPERCALIFRAGILISTICEXPIALIDOCIOUS'):
        sc = 2 if text_w(title, 2) <= room_playing else 1
        got = fit_text(title, sc, room_playing)
        ok(text_w(got, sc) <= room_playing,
           'a %d character title fits at scale %d as "%s"'
           % (len(title), sc, got))

    print('\nEvery character on the clock line is drawn')
    # The display face has digits and letters only; its punctuation is
    # blank. "--:--" once rendered as a lone colon, because the colon was
    # drawn by hand and the dashes were left to a glyph that is empty.
    face = [[int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', b)]
            for _, b in re.findall(r'/\*\s*(\S+)\s*\*/\s*\{([^}]*)\}',
                                   read('src', 'ui', 'font10x14.h'))]
    runs = body(s, 'static void draw_runs(int y, const struct run *runs, '
                   'int n, gfx_color big_c,')
    drawn = set(re.findall(r"\*p == '(.)'", runs))
    for ch in '0123456789:-AMPH':
        ok(ch in drawn or any(face[ord(ch) - 32]),
           "'%s' is %s" % (ch, 'drawn by hand' if ch in drawn
                           else 'a real glyph in the face'))

    print('\nIcons')
    for name, wdef, hdef in (('ic_monitor', 'MON_W', 'MON_H'),
                             ('ic_chip', 'CHIP_W', 'ICON_H'),
                             ('ic_ram', 'RAM_W', 'ICON_H'),
                             ('ic_note', 'NOTE_W', 'ICON_H')):
        m = re.search(r'%s\[(\w+)\] = \{([^}]*)\}' % name, s)
        rows = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', m.group(2))]
        ok(m.group(1) == hdef and len(rows) == K[hdef]
           and all(r < (1 << K[wdef]) for r in rows) and K[wdef] <= 16,
           '%s is %dx%d and no row sets a bit past its width'
           % (name, K[wdef], K[hdef]))


def companions():
    print('\nThe Python companion needs nothing but Python')
    py = read('tools', 'nexus-host', 'nexus_host.py')
    ok("sys.exit('pyserial is required" not in py and 'serial = None' in py,
       'pyserial is optional')
    ok('psutil = None' in py and '/proc/stat' in py,
       'so is psutil: /proc on Linux')
    ok('tty.setraw(fd)' in py, 'a POSIX serial port is opened as the tty it is')
    ok('osascript' in py and 'is running then' in py,
       'now playing on macOS from the Music and Spotify apps, without '
       'launching them')
    ok('def candidates' in py and 'p.vid == ZMK_VID)' in py,
       'it writes to every ZMK serial port, not the first - the first is '
       'often Studio\'s, which is why it used to find the dongle and show '
       'nothing')

    spec = importlib.util.spec_from_file_location(
        'nexus_host', os.path.join(ROOT, 'tools', 'nexus-host',
                                   'nexus_host.py'))
    nh = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(nh)
    ok(nh.parse_proc_stat('cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 1 2 3 4\n')
       == (150, 1000), '/proc/stat: busy excludes idle and iowait')
    ok(nh.parse_meminfo('MemTotal: 16000000 kB\nMemFree: 1000 kB\n'
                        'MemAvailable: 4000000 kB\n') == 75,
       '/proc/meminfo: used is total minus available, as free(1) counts it')
    ok(nh.parse_meminfo('MemTotal: 16000000 kB\n') is None,
       'and a kernel without MemAvailable reads as unknown, not a guess')
    noon = datetime.datetime(2026, 9, 13, 13, 32, 5).timestamp()
    ok(nh.clock_lines(noon) == ['T 48725', 'D 20709'],
       'T then D, from the same instant: %s' % nh.clock_lines(noon))
    ok(nh.fold(u'Sigur Rós') == 'SIGUR RS', 'and it folds like the dongle')

    class Quiet:
        def read(self):
            return None, None

    nh.now_playing = lambda: ('', '')
    state = {'clock': 1e18, 'np': ('', '')}
    ok(nh.batch(state, Quiet())[0].startswith('T '),
       'a round with nothing to say still says the clock, so the link '
       'does not go stale while it runs')
    nh.now_playing = lambda: ('So What', 'Miles Davis')
    got = nh.batch(state, Quiet())
    ok(got == ['N SO WHAT', 'A MILES DAVIS'],
       'title and artist go as N and A: %s' % got)

    print('\nThe Windows companion')
    ps = read('tools', 'nexus-host', 'nexus_host.ps1')
    clock = ps.split("$lines.Add('T '")[1][:300]
    ok("$lines.Add('D '" in clock, 'sends D right after T')
    ok('$lines.Add("N $title")' in ps and '$lines.Add("A $artist")' in ps,
       'and the title and artist separately')
    ok("'X'" in ps, 'says X on the way out')
    ok(not re.search(r'^\s*\$args\s*\+?=', ps, re.M),
       'never assigns $args - that is PowerShell\'s automatic variable')
    inst = ps.split('if ($Install) {')[-1]
    ok("GetFolderPath('Startup')" in ps and 'CreateShortcut' in inst,
       '-Install: a per-user Startup shortcut, no admin, no service')
    ok('Copy-Item $PSCommandPath $script' in inst
       and '-File `"$script`"' in inst,
       'pointing at an installed copy, so deleting the repo does not break it')
    ok(inst.index('Stop-Companions') < inst.index('Start-Process'),
       'an older copy is stopped before the new one starts')
    ok('Remove-Item $lnk' in ps.split('if ($Uninstall) {')[-1],
       '-Uninstall takes it away again')
    for name, flag in (('install.cmd', '-Install'),
                       ('uninstall.cmd', '-Uninstall')):
        ok('"%~dp0nexus_host.ps1" ' + flag in read('tools', 'nexus-host', name),
           '%s is the double-click for %s' % (name, flag))


def main():
    c = read('src', 'host', 'host_link.c')
    h = read('include', 'nexus', 'host.h')
    s = read('src', 'ui', 'host_screen.c')
    text_max = int(re.search(r'#define NEXUS_HOST_TEXT (\d+)', h).group(1))

    protocol(text_max)
    link_source(c, read('Kconfig'))
    screen(s, read('src', 'status', 'zmk_events.c'))
    layout(s)
    companions()

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
