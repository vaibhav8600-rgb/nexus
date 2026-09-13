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
        self.paused = False
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
        elif key == 'P':
            n = self.num(val, 1)
            if n is not None:
                self.paused = n == 0
        elif key == 'D':
            n = self.num(val, 200000)
            if n is None:
                return self._up()
            past = self.days_since_clock()
            self.day = n - past if n > past else 0
        elif key == 'X':
            self.cpu = self.mem = UNKNOWN
            self.now_playing = self.artist = ''
            self.paused = False
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
    era, doe = z // 146097, z % 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    mday = doy - (153 * mp + 2) // 5 + 1
    mon = mp + 2 if mp < 10 else mp - 10
    year = yoe + era * 400 + (1 if mon <= 1 else 0)
    wday = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'][(days + 4) % 7]
    return '%s %d %s %d' % (wday, mday,
                            ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL',
                             'AUG', 'SEP', 'OCT', 'NOV', 'DEC'][mon], year)


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
    st.line('P 0')
    ok(st.paused and st.now_playing == 'SO WHAT',
       'P 0 marks it paused - the title stays')
    st.line('P 7')
    ok(st.paused, 'P with anything but 0 or 1 changes nothing')
    st.line('P 1')
    ok(not st.paused, 'P 1 plays again')
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
       'the artist and a wrapped title\'s second line are cut to their room '
       '- text is not clipped to its card on this panel')
    ok('NEXUS_TXT_CAPTION' not in np.split('"NOW PLAYING"')[1],
       'title and artist are body size, always - caption size was unreadable '
       'from across a desk')

    print('\nThe level bars move only while something plays')
    ok('return h->link && h->now_playing[0] != \'\\0\' && !h->paused;'
       in body(s, 'static bool np_animating(const struct nexus_host *h)\n{'),
       'moving means: linked, a track showing, and not paused')
    tick = body(s, 'static void host_tick(void)\n{')
    anim = tick.split('np_animating(nexus_host())')[-1]
    ok('if (np_animating(nexus_host())) {' in tick
       and 'g_eq_frame++' in anim
       and 'nexus_screen_invalidate_rows(EQ_BASE - EQ_MAX, EQ_BASE)' in anim,
       'the tick advances the frame and repaints only the rows the bars '
       'stand in')
    ok('k_eq[g_eq_frame % EQ_FRAMES][k]' in np and 'k_uptime_get' not in np,
       'draw() reads the frame the tick set, never the clock - draw runs once '
       'per band, and two clock readings would tear the bars across a band')
    ok('int bh = live ? k_eq[g_eq_frame % EQ_FRAMES][k] : 4;' in np
       and 'live ? t->accent_alt : t->muted' in np,
       'paused: a flat muted line')
    frames = [[int(v) for v in re.findall(r'\d+', row)] for row in
              re.findall(r'\{ ([\d, ]+) \}',
                         s.split('k_eq[EQ_FRAMES][4] = {')[1].split('};')[0])]
    K = {k: int(v) for k, v in re.findall(r'^#define (\w+) (\d+)', s, re.M)}
    art_y = K['NP_Y'] + (K['NP_H'] - K['ART']) // 2
    eq_base = art_y + K['ART'] - 4
    ok(len(frames) == 8 and all(len(f) == 4 for f in frames)
       and frames[0] == [10, 20, 14, 24],
       'eight frames of four bars; frame 0 is the still pose')
    ok(max(max(f) for f in frames) <= K['EQ_MAX'],
       'no bar is taller than the rows the tick repaints (%d)' % K['EQ_MAX'])
    ok(eq_base - K['EQ_MAX'] >= K['NP_Y'] and eq_base <= K['NP_Y'] + K['NP_H'],
       'and those rows, %d-%d, are inside the card' % (eq_base - K['EQ_MAX'],
                                                       eq_base))
    bands = {y // 12 for y in range(eq_base - K['EQ_MAX'], eq_base)}
    ok(len(bands) == 2,
       'a frame costs %d bands of the panel\'s 20' % len(bands))
    link_c = read('src', 'host', 'host_link.c')
    p_case = link_c.split("case 'P':")[1].split("case '")[0]
    ok('to_u32(val, 1)' in p_case and 'NEXUS_HOST_F_NP' in p_case
       and 'g_host.paused = false;' in link_c.split("case 'X':")[1],
       'P 1 plays, P 0 pauses, anything else is ignored; X clears it')
    st = Host(40)
    st.line('N SO WHAT')
    ok(not getattr(st, 'paused', False),
       'a companion that never sends P reads as playing, so older ones '
       'still get moving bars')

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
       and 'draw_line("SINCE CONNECTED")' in clock,
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
    tt = body(s, 'bool nexus_host_time_text(char *buf, int len, '
                 'const char **suffix)\n{')
    ok('nexus_host_time_text(a, sizeof(a), &suffix)' in clock,
       'the HOST clock is written by the shared nexus_host_time_text()')
    ok('IS_ENABLED(CONFIG_NEXUS_HOST_CLOCK_24H)' in tt,
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
    ok('hour = 12U' in tt and '*suffix ? 0 : 2' in tt,
       'midnight and noon read as 12; 24 hour is padded so the colon holds')
    ok('gfx_face_h(BIG) - gfx_face_h(1)' in s,
       'units sit on the numerals\' baseline')

    print('\nThe date')
    fmt = body(s, 'void nexus_host_date_text(uint32_t days, char *wday, '
                  'char *dmon, char *yyyy)\n{')
    for const in ('719468U', '146097U', '1460U', '36524U', '146096U',
                  '153U', '(days + 4U) % 7U', 'era * 400U',
                  '(mon <= 1U ? 1U : 0U)'):
        ok(const in fmt, 'nexus_host_date_text() is civil_from_days (%s)'
           % const)
    epoch = datetime.date(1970, 1, 1)
    wrong = []
    # Every day from 1970 to 2517 - the whole range D accepts - weekday, day,
    # month and year, against Python's own calendar.
    for d in range(0, 200001):
        dt = epoch + datetime.timedelta(days=d)
        want = '%s %d %s %d' % (dt.strftime('%a').upper(), dt.day,
                                dt.strftime('%b').upper(), dt.year)
        if civil(d) != want:
            wrong.append((d, civil(d), want))
    ok(not wrong, 'it agrees with the calendar on every day from 1970 to '
       '2517, leap years and 2000 and 2100 included: %s'
       % (wrong[:3] or '200,001 days checked'))
    ok(civil(20709) == 'SUN 13 SEP 2026', 'day 20709 is SUN 13 SEP 2026')
    ok(civil(0) == 'THU 1 JAN 1970', 'day 0 is THU 1 JAN 1970')
    ok(civil(11016) == 'TUE 29 FEB 2000' and civil(47541) == 'MON 1 MAR 2100'
       and civil(47540) == 'SUN 28 FEB 2100',
       '2000 has a 29 February; 2100 does not')
    longest = max((civil(d) for d in range(0, 200001, 7)), key=len)
    ok(len(longest) + 1 <= 16 and 'char line[16]' in s,
       'the longest date ("%s") fits its 16 byte buffer' % longest)


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
                rw = sum(4 * K['BIG'] if ch == ':' else big_adv
                         for ch in txt) - K['BIG']
            else:
                rw = len(txt) * 11 - 1
            total += rw + ((8 if big else 4) if i else 0)
        return total

    ok(K['BIG'] == 2, 'the clock is the display face at 2x: 28px')
    for runs in ([('12:59', True), ('PM', False)], [('23:59', True)],
                 [('999', True), ('H', False), ('59', True), ('M', False)]):
        ok(runs_w(runs) <= content - 16,
           'the widest clock line fits its card: %s is %dpx'
           % (''.join(r[0] for r in runs), runs_w(runs)))
    line_y = K['CLOCK_Y'] + int(re.search(r'#define LINE_Y \(CLOCK_Y \+ (\d+)\)',
                                          s).group(1))
    for line in ('SINCE CONNECTED', 'WED 30 SEP 2026', 'HOST TIME'):
        lw = len(line) * 11 - 1
        ok(lw <= content - 16,
           '"%s" in the face at 1x is %dpx, inside the card' % (line, lw))
    numerals_end = K['CLOCK_Y'] + 9 + 14 * K['BIG']
    ok(line_y - numerals_end >= 8
       and line_y + 14 <= K['CLOCK_Y'] + K['CLOCK_H'] - 6,
       'the date sits %dpx under the numerals and inside the card'
       % (line_y - numerals_end))
    sc = K['BIG']
    ok(4 * sc + 2 * sc <= 9 * sc and 9 * sc + 2 * sc <= 14 * sc,
       'the drawn colon sits inside the numerals\' height at any scale')

    print('\nNow playing titles')
    room_playing = content - (8 + K['ART'] + 10) - 8 - K['EQ_ROOM']

    def wrap(title):
        """draw_now_playing(): one line at body size, or two broken at a
        space, the second cut to fit."""
        n = (room_playing + 2) // (text_w('0', 2) + 2)
        if len(title) <= n:
            return [title]
        cut = n
        while cut > 0 and title[cut] != ' ':
            cut -= 1
        nxt = cut + 1 if cut > 0 else n
        cut = cut if cut > 0 else n
        return [title[:cut], fit_text(title[nxt:], 2, room_playing)]

    for title, want in (('SO WHAT', ['SO WHAT']),
                        ('NEON NIGHTS', ['NEON NIGHTS']),
                        ('PAHADO KE SHEHAR MEIN', ['PAHADO KE', 'SHEHAR MEIN']),
                        ('LOKI DEATH IN DOOMSDAY? 4 MORE SPIDER-M',
                         ['LOKI DEATH', 'IN DOOMSD..']),
                        ('SUPERCALIFRAGILISTIC', ['SUPERCALIFR', 'AGILISTIC'])):
        got = wrap(title)
        ok(got == want and all(text_w(x, 2) <= room_playing for x in got),
           '"%s" -> %s' % (title, got))
    np_src = body(s, 'static void draw_now_playing(const struct nexus_host '
                     '*h)\n{')
    ok("while (cut > 0 && title[cut] != ' ')" in np_src
       and 'int next = cut > 0 ? cut + 1 : n;' in np_src,
       'the C breaks at the last space that fits, or mid-word for one long '
       'word')

    print('\nEvery character on the clock line is drawn')
    # The display face has digits and letters only; its punctuation is
    # blank. "--:--" once rendered as a lone colon, because the colon was
    # drawn by hand and the dashes were left to a glyph that is empty.
    face = [[int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', b)]
            for _, b in re.findall(r'/\*\s*(\S+)\s*\*/\s*\{([^}]*)\}',
                                   read('src', 'ui', 'font10x14.h'))]
    runs = body(s, 'int nexus_host_numerals(int x, int y, const char *s, '
                   'int scale, gfx_color c)\n{')
    drawn = set(re.findall(r"\*s == '(.)'", runs))
    ok('nexus_host_numerals(x, y, r->s, BIG, big_c)'
       in body(s, 'static void draw_runs(int y, const struct run *runs, '
                  'int n, gfx_color big_c,'),
       'HOST draws its numerals with the shared helper')
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
    ok('def dongle_ports' in py and 'if p.vid == ZMK_VID)' in py,
       'it looks for the dongle by ZMK\'s USB vendor id')

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

    print('\nOnly the host link port - never ZMK Studio\'s')
    # Opening every NEXUS port held Studio's too - Windows gives a port to one
    # program at a time - so with the companion installed Studio could not
    # connect. The first fix guessed the host link was the higher interface,
    # and on the real dongle it was the lower: Studio is MI_03 (COM15), the
    # host link MI_00 (COM14). So the companions ask instead. Studio's port
    # answered this exact request with AB 0A 06 08 01 1A 02 10 00 AD
    # (request 1, lock state LOCKED); the host link stayed silent.
    ok(nh.STUDIO_PING == bytes([0xAB, 0x08, 0x01, 0x1A, 0x02, 0x10, 0x01,
                                0xAD, 0x0A]),
       'the probe is a framed Studio get_lock_state request, and a newline')

    saved = nh.dongle_ports, nh.is_studio
    for ports, answers, want, why in (
            (['COM14', 'COM15'], {'COM14': False, 'COM15': True}, ['COM14'],
             'the real dongle: Studio answers on COM15, COM14 is ours'),
            (['COM14', 'COM15'], {'COM14': True, 'COM15': False}, ['COM15'],
             'the other order: still the silent one, whichever it is'),
            (['COM14', 'COM15'], {'COM14': False, 'COM15': None}, ['COM14'],
             'Studio connected, its port busy: skipped, ours still found'),
            (['COM7'], {'COM7': False}, ['COM7'],
             'a build without Studio: its one port'),
            (['COM14', 'COM15'], {'COM14': None, 'COM15': True}, [],
             'ours busy (another copy has it): nothing, rather than Studio\'s')):
        nh.dongle_ports = lambda ports=ports: ports
        nh.is_studio = lambda p, answers=answers: answers[p]
        ok(nh.candidates() == want, '%s - %s' % (why, nh.candidates()))
    nh.dongle_ports, nh.is_studio = saved

    st = Host(40)
    st.isr(nh.STUDIO_PING + b'C 41\n')
    st.work()
    ok(st.cpu == 41,
       'the probe landing on the host link is one ignored line; the next '
       'line still parses')

    ps1 = read('tools', 'nexus-host', 'nexus_host.ps1')
    ok('[byte[]](0xAB, 0x08, 0x01, 0x1A, 0x02, 0x10, 0x01, 0xAD, 0x0A)' in ps1,
       'PowerShell sends the same request')
    ps_pick = ps1.split('function Get-HostLinkPort')[1].split('\n}\n')[0]
    ok('Test-StudioPort $c.Port' in ps_pick
       and "if ($studio -eq $false) { return $c.Port }" in ps_pick
       and 'Select-Object -Last 1' not in ps_pick,
       'and takes the port that stays silent, not a guess from the order')
    ok('else { @(Get-HostLinkPort) }' in ps1
       and '(Get-NexusPorts).Port }' not in ps1,
       'and only that port is opened, unless -Port says otherwise')

    class Quiet:
        def read(self):
            return None, None

    nh.now_playing = lambda: ('', '', False)
    state = {'clock': 1e18, 'np': ('', '', False)}
    ok(nh.batch(state, Quiet())[0].startswith('T '),
       'a round with nothing to say still says the clock, so the link '
       'does not go stale while it runs')
    nh.now_playing = lambda: ('So What', 'Miles Davis', True)
    got = nh.batch(state, Quiet())
    ok(got == ['N SO WHAT', 'A MILES DAVIS', 'P 1'],
       'title, artist and playing go as N, A and P: %s' % got)
    nh.now_playing = lambda: ('So What', 'Miles Davis', False)
    got = nh.batch(state, Quiet())
    ok(got[-1] == 'P 0', 'and pausing sends P 0: %s' % got)
    nl = chr(10)
    for text, has, want in (
            ('Song' + nl + 'Band' + nl + 'Playing' + nl, True,
             ('Song', 'Band', True)),
            ('Song' + nl + 'Band' + nl + 'Paused' + nl, True,
             ('Song', 'Band', False)),
            (nl + nl + 'Stopped' + nl, True, ('', '', False)),
            ('Song' + nl + 'Band' + nl, False, ('Song', 'Band', True))):
        ok(nh.parse_now_playing(text, has) == want,
           'playerctl/osascript output %r -> %r' % (text, want))

    print('\nThe Windows companion')
    ps = read('tools', 'nexus-host', 'nexus_host.ps1')
    clock = ps.split("$lines.Add('T '")[1][:300]
    ok("$lines.Add('D '" in clock, 'sends D right after T')
    ok('$lines.Add("N $title")' in ps and '$lines.Add("A $artist")' in ps,
       'and the title and artist separately')
    ok('$lines.Add("P $playing")' in ps
       and "PlaybackStatus -eq 'Playing') { 1 } else { 0 }" in ps
       and '$np = "$title|$artist|$playing"' in ps,
       'and P, from the session\'s own playback status, resent when it '
       'changes')
    ok("'X'" in ps, 'says X on the way out')
    ok(not re.search(r'^\s*\$args\s*\+?=', ps, re.M),
       'never assigns $args - that is PowerShell\'s automatic variable')
    inst = ps.split('if ($Install) {')[-1].split('\n}\n')[0]
    ok("'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run'" in ps
       and 'Set-ItemProperty $RUN_KEY' in inst,
       '-Install: the per-user Run key, no admin, listed in Startup apps')
    ok('CreateShortcut' not in ps,
       'not a Startup-folder shortcut - that folder is wherever the registry '
       'says, and on a real machine it said a deleted temp directory')
    ok('Copy-Item $PSCommandPath $script' in inst
       and '-File `"$script`"' in inst,
       'pointing at an installed copy, so deleting the repo does not break it')
    ok(inst.index('Stop-Companions') < inst.index('Start-Process'),
       'an older copy is stopped before the new one starts')
    ok(inst.index('try {') < inst.index('Set-ItemProperty')
       and 'exit 1' in inst
       and inst.index('exit 1') < inst.index("'running."),
       'a failed step stops the install and says so, rather than printing '
       '"installed" under the errors')
    un = ps.split('if ($Uninstall) {')[-1].split('\n}\n')[0]
    ok('Remove-ItemProperty $RUN_KEY' in un and 'Get-OldStartupLink' in un,
       '-Uninstall removes the Run entry, and an old shortcut if one exists')
    for name, flag in (('install.cmd', '-Install'),
                       ('uninstall.cmd', '-Uninstall')):
        ok('"%~dp0nexus_host.ps1" ' + flag in read('tools', 'nexus-host', name),
           '%s is the double-click for %s' % (name, flag))


def home_plate():
    print('\nThe home screen\'s plate clock')
    home = read('src', 'ui', 'home.c')
    ok(re.search(r'^#define BRAND_Y NEXUS_PAD\b', home, re.M) is not None,
       'the plate starts at NEXUS_PAD (9)')
    brand_y, brand_h = 9, int(re.search(r'#define BRAND_H (\d+)', home).group(1))
    row1 = brand_y + int(re.search(r'#define PLATE_ROW1 \(BRAND_Y \+ (\d+)\)',
                                   home).group(1))
    row2 = brand_y + int(re.search(r'#define PLATE_ROW2 \(BRAND_Y \+ (\d+)\)',
                                   home).group(1))

    hd = body(home, 'static void home_draw(void)\n{')
    brand = hd.split('gfx_hits(BRAND_Y, BRAND_H)')[1].split('if (gfx_hits')[0]
    ok('\t\tdraw_brand();\n#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK)\n'
       '\t\tdraw_host_clock();\n#endif' in brand,
       'the NEXUS name is always drawn; the clock only joins it')
    ok(brand.index('draw_host_clock()') < brand.index('draw_flags(st)'),
       'and the jiggler dot still draws last')
    draw = body(home, 'static void draw_host_clock(void)\n{')
    ok('if (!plate_clock_shown() ||' in draw
       and draw.index('return;') < draw.index('nexus_host_numerals('),
       'nothing is drawn before a host has sent a time, or if the name '
       'leaves no room')
    ok('nexus_host_numerals(centred(l0, l1, nexus_host_numerals_w(hm, 1))'
       in draw and 'nexus_host_date_text(day, wd, dm, yy)' in draw,
       'with the helpers HOST uses, so the two screens cannot disagree')
    ok('.tick = home_tick' in home and
       'nexus_screen_invalidate_rows(BRAND_Y, BRAND_Y + BRAND_H)'
       in body(home, 'static void home_tick(void)\n{'),
       'and it repaints the plate when the minute or day turns')

    print('\nReadable from further away')
    # About 23.4mm of panel over 240px. Normal acuity recognises a letter
    # that subtends 5 arcminutes.
    mm_per_px = 23.4 / 240

    def arcmin(px, mm):
        return px * mm_per_px / mm * (180 / 3.141592653589793) * 60

    flat = re.sub(r'\s+', ' ', draw)
    ok('PLATE_ROW1, hm, 1, t->value)' in flat
       and 'PLATE_ROW1, wd, 1,' in flat,
       'the time and the weekday are the display face at 1x: 14px, bold')
    ok('PLATE_ROW2, suffix, NEXUS_TXT_CAPTION' in flat
       and 'PLATE_ROW2, dm, NEXUS_TXT_CAPTION' in flat,
       'AM/PM and the date are caption type under them - four bold rows '
       'crowded the name')
    ok(arcmin(14, 914) >= 5,
       '14px is %.1fmm: %.1f arcmin at three feet, readable (was %.1f)'
       % (14 * mm_per_px, arcmin(14, 914), arcmin(7, 914)))
    print('        at four feet it is %.1f arcmin - borderline, and the most '
          'that fits beside the name' % arcmin(14, 1219))

    print('\nIt fits beside the name')
    # NEXUS at display face 2x: 108px, centred, glow two letter-pixels out.
    half = (5 * 22 - 2) // 2 + 4
    l0, l1, r0, r1 = 9, 120 - half, 120 + half, 231

    def centred(a, b, w):
        return (a + b) // 2 - w // 2

    def num_w(s):
        return sum(4 if ch == ':' else 11 for ch in s) - 1

    for s in ('12:59', '23:59', '0:00'):
        w = num_w(s)
        x = centred(l0, l1, w)
        ok(x >= l0 + 3 and x + w <= l1 - 3,
           '"%s" spans %d-%d, between the plate edge (%d) and the glow (%d)'
           % (s, x, x + w, l0, l1))
    for w, what in ((num_w('WED'), 'WED'), (text_w('30 SEP', 1), '30 SEP')):
        x = centred(r0, r1, w)
        ok(x >= r0 + 3 and x + w <= r1 - 3,
           '"%s" spans %d-%d, between the glow (%d) and the plate edge (%d)'
           % (what, x, x + w, r0, r1))
    ok(row1 >= brand_y + 8 and row2 + 7 <= brand_y + brand_h - 6
       and row2 >= row1 + 14 + 3,
       'rows at y %d and %d, inside the plate and clear of each other'
       % (row1, row2))
    ok(abs((row1 + row2 + 7) / 2 - (brand_y + brand_h / 2)) <= 1,
       'and centred on it, as the name is')
    shown = body(home, 'static bool plate_clock_shown(void)\n{')
    ok('gfx_face_w("WED", 1) + 6 <= right' in shown
       and 'gfx_text_w("00 MMM", NEXUS_TXT_CAPTION) + 6 <= right' in shown,
       'the room check covers both right-hand rows, not just the weekday')

    print('\nThe jiggler dot')
    flags = body(home, 'static void draw_flags(const struct nexus_status *st)'
                       '\n{')
    ok('if (plate_clock_shown()) {' in flags
       and flags.index('#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK)')
       < flags.index('plate_clock_shown()'),
       'moves only while the clock is on the plate, and only in host builds')
    x, y, r = 231 - 7, brand_y + 5, 3 + 1
    wd_right = centred(r0, r1, num_w('WED')) + num_w('WED')
    ok(y + r < row1,
       'tucked above the weekday: the dot ends at y %d, the weekday starts '
       'at %d' % (y + r, row1))
    ok(x - r >= wd_right,
       'and right of it: the dot starts at x %d, the weekday ends at %d'
       % (x - r, wd_right))
    for radius in (7, 9):
        ccx, ccy = 231 - radius, brand_y + radius
        far = ((x + r * 0.7 - ccx) ** 2 + (y - r * 0.7 - ccy) ** 2) ** 0.5
        ok(far <= radius,
           'inside the card\'s rounded corner at radius %d' % radius)


def without_host_link():
    print('\nA build without CONFIG_NEXUS_HOST_LINK')
    # Off is the default, and most people who use this module never turn it
    # on. host_link.c and host_screen.c are not compiled then, so anything
    # that calls into them from elsewhere has to be compiled out with them -
    # or the build fails at link time, for everyone, on a feature they never
    # asked for.
    kc = read('Kconfig').split('config NEXUS_HOST_LINK\n')[1].split('\nconfig ')[0]
    ok('default n' in kc, 'the host link is off unless a config turns it on')
    cm = read('CMakeLists.txt')
    for f in ('src/host/host_link.c', 'src/ui/host_screen.c'):
        ok('zephyr_library_sources_ifdef(CONFIG_NEXUS_HOST_LINK %s)' % f in cm,
           '%s is only compiled with it' % f)

    only_there = re.compile(
        r'\b(nexus_host|nexus_host_clock|nexus_host_day|nexus_host_time_text|'
        r'nexus_host_date_text|nexus_host_link_init|nexus_host_screen_dirty)'
        r'\s*\(|\bnexus_screen_host_def\b')
    unguarded = []
    checked = 0
    for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
        for name in files:
            rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
            rel = rel.replace(os.sep, '/')
            host_only = ('src/host/host_link.c', 'src/ui/host_screen.c')
            if not name.endswith('.c') or rel in host_only:
                continue
            stack = []           # per open #if: does it select the host link?
            for n, line in enumerate(read(rel).splitlines(), 1):
                d = line.strip()
                if re.match(r'#\s*if', d):
                    stack.append('CONFIG_NEXUS_HOST_LINK' in d
                                 and '!' not in d)
                elif re.match(r'#\s*(else|elif)', d) and stack:
                    stack[-1] = False
                elif re.match(r'#\s*endif', d) and stack:
                    stack.pop()
                elif only_there.search(line):
                    checked += 1
                    if not any(stack):
                        unguarded.append('%s:%d' % (rel, n))
    ok(checked > 0 and not unguarded,
       'every one of the %d calls into them elsewhere is inside '
       '#if IS_ENABLED(CONFIG_NEXUS_HOST_LINK): %s'
       % (checked, unguarded or 'none outside'))


def main():
    c = read('src', 'host', 'host_link.c')
    h = read('include', 'nexus', 'host.h')
    s = read('src', 'ui', 'host_screen.c')
    text_max = int(re.search(r'#define NEXUS_HOST_TEXT (\d+)', h).group(1))

    protocol(text_max)
    link_source(c, read('Kconfig'))
    screen(s, read('src', 'status', 'zmk_events.c'))
    layout(s)
    home_plate()
    without_host_link()
    companions()

    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
