#!/usr/bin/env python3
"""NEXUS companion: push what only this machine knows to the dongle.

One way, one line at a time, over the USB serial the dongle exposes when
CONFIG_NEXUS_HOST_LINK is on. The dongle never answers, so this can only ever
tell it things - it cannot type, press keys, or read anything back.

    python3 nexus_host.py --list          # which ports look like the dongle?
    python3 nexus_host.py                 # find them and run
    python3 nexus_host.py --port /dev/ttyACM1
    python3 nexus_host.py --buddy         # and be Buddy's brain (docs/buddy.md)

--buddy talks to Gemini only with GEMINI_API_KEY set in the environment, and
sends it the time of day, how busy this machine is (low / medium / high), the
track playing and why it is asking - never keystrokes, windows or files.
Without the key it says canned lines from persona/persona.json.

Needs nothing but Python on Linux and macOS: the serial port is opened as
the tty it is, CPU and memory come from /proc on Linux, and now playing from
playerctl on Linux or the Music and Spotify apps on macOS. pyserial and
psutil are used when they happen to be installed, and never required.

On Windows use nexus_host.ps1 instead - it needs nothing at all there, and
Python cannot open a COM port without pyserial.
"""
import argparse
import datetime
import glob
import json
import os
import random
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None

try:
    import psutil
except ImportError:
    psutil = None

TEXT_MAX = 39          # NEXUS_HOST_TEXT - 1, the dongle truncates anyway
# Seconds between clock resends. Ten, not sixty: a dongle that has lost power
# has no clock, and nothing can ask for one over a one-way link. The dongle
# draws minutes, so a resend inside the same one costs no repaint.
CLOCK_EVERY = 10
NP_EVERY = 10          # seconds between resends of the track, changed or not
ZMK_VID = 0x1D50
EPOCH = datetime.date(1970, 1, 1)


# ---- ports ------------------------------------------------------------------

# A ZMK Studio RPC request: SOF 0xAB, Request { request_id: 1, core {
# get_lock_state: true } }, EOF 0xAD. Read-only. The newline makes the bytes
# one ignored line if this lands on the host link instead.
STUDIO_PING = bytes([0xAB, 0x08, 0x01, 0x1A, 0x02, 0x10, 0x01, 0xAD, 0x0A])


def dongle_ports():
    """Every serial port the dongle has - Studio's and the host link."""
    if serial:
        return sorted(p.device for p in list_ports.comports()
                      if p.vid == ZMK_VID)
    if sys.platform.startswith('linux'):
        # udev names these after the USB manufacturer string, which ZMK
        # sets. Stable across replugs, unlike ttyACM numbers.
        return sorted(glob.glob('/dev/serial/by-id/usb-ZMK_Project_*'))
    # macOS without pyserial: cu.usbmodem* is every CDC device on the
    # machine, and writing to an Arduino is not a guess worth making.
    return []


def is_studio(path):
    """True if the port answers a Studio request, False if it stays silent,
    None if it cannot be opened."""
    try:
        if serial:
            with serial.Serial(path, 115200, timeout=0.1,
                               write_timeout=1) as s:
                time.sleep(0.2)
                s.reset_input_buffer()
                s.write(STUDIO_PING)
                end = time.time() + 1.0
                while time.time() < end:
                    if s.in_waiting:
                        return True
                    time.sleep(0.05)
                return False
        import select
        import termios
        import tty
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            tty.setraw(fd)
            time.sleep(0.2)
            termios.tcflush(fd, termios.TCIFLUSH)
            os.write(fd, STUDIO_PING)
            ready, _, _ = select.select([fd], [], [], 1.0)
            return bool(ready)
        finally:
            os.close(fd)
    except Exception:            # OSError, or pyserial's SerialException
        return None


def candidates():
    """The host link port, and only that.

    With ZMK Studio built in the dongle has two serial ports, and writing
    these lines into Studio's is not harmless. Which is which cannot be
    guessed from the order - a guess that the host link was the higher
    interface was wrong on the first real dongle it met - so ask: Studio's
    port answers a Studio request, and the host link, being one way, never
    answers anything. The silent one is ours. With no Studio there is one
    port, it is silent, and it is picked.
    """
    for path in dongle_ports():
        if is_studio(path) is False:
            return [path]
    return []


def open_port(path):
    if serial:
        return serial.Serial(path, 115200, timeout=1, write_timeout=2)
    import tty
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)             # no echo, no newline translation
    return os.fdopen(fd, 'wb', buffering=0)


# ---- readings ----------------------------------------------------------------

def parse_proc_stat(text):
    """(busy, total) jiffies from the aggregate cpu line of /proc/stat."""
    f = [int(x) for x in text.split('\n', 1)[0].split()[1:]]
    idle = f[3] + (f[4] if len(f) > 4 else 0)      # idle + iowait
    return sum(f) - idle, sum(f)


def parse_meminfo(text):
    """Percent used from /proc/meminfo, the way free(1) counts it."""
    kv = {}
    for line in text.splitlines():
        k, _, v = line.partition(':')
        if v.strip():
            kv[k] = int(v.split()[0])
    total = kv.get('MemTotal', 0)
    if not total or 'MemAvailable' not in kv:
        return None
    return round(100 * (total - kv['MemAvailable']) / total)


class Load:
    """CPU and memory percent, or None for either this OS will not give."""

    def __init__(self):
        self.last = None
        if psutil:
            psutil.cpu_percent()   # the first reading is always 0.0

    def read(self):
        if psutil:
            return (round(psutil.cpu_percent()),
                    round(psutil.virtual_memory().percent))
        if not os.path.exists('/proc/stat'):
            return None, None
        try:
            with open('/proc/stat') as f:
                busy, total = parse_proc_stat(f.read())
            with open('/proc/meminfo') as f:
                mem = parse_meminfo(f.read())
        except (OSError, ValueError, IndexError):
            return None, None
        cpu = None
        if self.last and total > self.last[1]:
            cpu = round(100 * (busy - self.last[0]) / (total - self.last[1]))
        self.last = (busy, total)
        return cpu, mem


OSASCRIPT = []
for app in ('Spotify', 'Music'):
    OSASCRIPT += ['-e', 'if application "%s" is running then' % app,
                  '-e', 'tell application "%s"' % app,
                  '-e', 'if player state is playing then return '
                        '(name of current track) & linefeed & '
                        '(artist of current track)',
                  '-e', 'end tell', '-e', 'end if']
OSASCRIPT += ['-e', 'return ""']


def now_playing():
    """(title, artist, playing) from whatever this OS will say, or
    ('', '', False).

    Never raises, never hangs. The "is running" checks matter on macOS:
    asking an app that is not running for its track launches it.
    """
    try:
        if sys.platform.startswith('linux') and shutil.which('playerctl'):
            cmd = ['playerctl', 'metadata', '--format',
                   '{{title}}\n{{artist}}\n{{status}}']
        elif sys.platform == 'darwin':
            cmd = ['osascript'] + OSASCRIPT
        else:
            return '', '', False
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=2)
        if out.returncode != 0:
            return '', '', False
        return parse_now_playing(out.stdout, cmd[0] == 'playerctl')
    except Exception:
        return '', '', False


def parse_now_playing(text, has_status):
    """Title, artist and playing from 'title\\nartist[\\nstatus]'. Only the
    trailing newline is stripped: an empty title is a leading one. playerctl
    says Playing or Paused; the macOS script only answers while playing."""
    parts = text.rstrip('\n').split('\n') + ['', '', '']
    title, artist = parts[0].strip(), parts[1].strip()
    playing = parts[2].strip() == 'Playing' if has_status else bool(title)
    return title, artist, playing


def fold(text):
    """What the panel's font can draw: ASCII 32..90, upper case. The dongle
    folds too; doing it here keeps the wire to what can be drawn."""
    return ''.join(c for c in text.upper() if 32 <= ord(c) <= 90)[:TEXT_MAX]


# ---- the protocol ------------------------------------------------------------

def clock_lines(now=None):
    """T then D, from the same instant - the dongle files the date against
    the day that clock counts from."""
    t = time.localtime(now)
    days = (datetime.date(t.tm_year, t.tm_mon, t.tm_mday) - EPOCH).days
    return ['T %d' % (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec),
            'D %d' % days]


def batch(state, load):
    """The next lines to send. @p state carries what was sent last."""
    out = []
    now = time.time()

    if now - state['clock'] >= CLOCK_EVERY:
        out += clock_lines(now)
        state['clock'] = now

    cpu, mem = load.read()
    state['cpu'], state['mem'] = cpu, mem      # for Buddy's brain
    if cpu is not None:
        out.append('C %d' % cpu)
    if mem is not None:
        out.append('M %d' % mem)

    title, artist, playing = now_playing()
    np = (fold(title), fold(artist), playing)
    state['np_now'] = np
    # When it changes, and every NP_EVERY seconds whether it has or not. The
    # link is one way: when the keyboard is switched to another host and
    # back the dongle forgets the track, and nothing can ask for it again.
    # The dongle ignores a value it already has, so a resend costs no repaint.
    if np != state['np'] or now - state.get('np_at', now) >= NP_EVERY:
        # P: playing or paused. The dongle moves its level bars only while
        # a track plays; firmware from before P ignores the line.
        out += ['N %s' % np[0], 'A %s' % np[1], 'P %d' % playing]
        state['np'] = np
        state['np_at'] = now

    # Never nothing: the dongle drops the link after a few silent seconds,
    # and NO LINK while this is plainly running sends you looking for a
    # fault that is not there. The clock is the cheapest thing to say.
    if not out:
        out = clock_lines(now)[:1]
    return out


# ---- Buddy's brain -----------------------------------------------------------

GEMINI_MODEL = 'gemini-3.1-flash-lite'   # the one place it is named
GEMINI_URL = ('https://generativelanguage.googleapis.com/v1beta/models/'
              '%s:generateContent')
LINE_MAX = 24          # the persona's limit, and what the bubble fits
ASK_GAP = 20           # seconds between Gemini requests, at the least
LEASE_EVERY = 10       # B 1 resend; the dongle hands the face back after 60
SONG_GAP = 300         # at most one remark per five minutes about the music
BUSY_CPU, BUSY_FOR = 90, 60
BACKOFF_MAX = 1800

# Why it is asking, as Gemini is told.
REASONS = {
    'hello': 'the owner just sat down at the computer; greet them',
    'tick': 'a few quiet minutes have passed; say something small',
    'music': 'a new song just started',
    'busy': 'the computer has been very busy for a minute',
    'late': 'it is late at night',
}


def persona_path():
    """Beside the script once installed, else the repo's own copy."""
    here = os.path.dirname(os.path.abspath(__file__))
    beside = os.path.join(here, 'persona.json')
    if os.path.exists(beside):
        return beside
    return os.path.join(here, '..', '..', 'persona', 'persona.json')


def load_persona(path=None):
    with open(path or persona_path(), encoding='utf-8') as f:
        return json.load(f)


def buddy_fold(text):
    return ''.join(c for c in text.upper() if 32 <= ord(c) <= 90)[:LINE_MAX]


def system_prompt(p):
    """The persona as Gemini reads it. persona.json is the one source."""
    v = p['voice']
    return '\n'.join(
        ['You are %s, a tiny animated face on %s\'s keyboard dongle. '
         'Pronoun: %s.' % (p['name'], p['owner'] or 'the owner', p['pronoun']),
         'Traits:'] + ['- ' + t for t in p['traits']] +
        ['Voice: %s. A line is at most %d characters, %s case, %s.'
         % (v['style'], v['max_chars'], v['case'], v['charset']),
         'Never:'] + ['- ' + n for n in p['never']] +
        ['Reply only as JSON matching the schema: "emotion" is how the face '
         'looks, "line" goes on the dongle, "reply" is a slightly longer '
         'version for a screen.'])


def gemini_body(p, context):
    return {
        'systemInstruction': {'parts': [{'text': system_prompt(p)}]},
        'contents': [{'role': 'user', 'parts': [{'text': context}]}],
        'generationConfig': {
            'responseMimeType': 'application/json',
            'responseSchema': {
                'type': 'OBJECT',
                'properties': {
                    'emotion': {'type': 'STRING',
                                'enum': [e.upper() for e in p['emotions']]},
                    'line': {'type': 'STRING'},
                    'reply': {'type': 'STRING'},
                },
                'required': ['emotion', 'line', 'reply'],
            },
            'temperature': 1.0,
            'maxOutputTokens': 200,
        },
    }


def parse_reply(raw, p):
    """(emotion, line) from a generateContent response, or None for anything
    the dongle should not be sent: bad JSON, an emotion it does not have, a
    line too long or full of characters the font cannot draw."""
    try:
        text = json.loads(raw)['candidates'][0]['content']['parts'][0]['text']
        obj = json.loads(text)
        emotion = str(obj['emotion']).lower()
        line = obj['line']
    except (ValueError, KeyError, IndexError, TypeError):
        return None
    if emotion not in p['emotions'] or not isinstance(line, str):
        return None
    line = line.strip()
    if not line or len(line) > LINE_MAX or buddy_fold(line) != line.upper():
        return None
    return emotion, line.upper()


def bucket(pct):
    if pct is None:
        return 'unknown'
    return 'low' if pct < 40 else 'medium' if pct < 80 else 'high'


def part_of_day(hour):
    return ('night' if hour < 5 or hour >= 23 else 'morning' if hour < 12
            else 'afternoon' if hour < 18 else 'evening')


class Brain:
    """Decides when Buddy says something and gets the words: from Gemini with
    GEMINI_API_KEY, from persona.json without. Returns lines for the batch.

    The request runs on a thread. Waiting on it here would hold back C and M,
    and a HOST screen gone stale for a slow answer is a regression.
    """

    def __init__(self, persona, key, model=GEMINI_MODEL, minutes=10,
                 call=None):
        self.p, self.key, self.model = persona, key, model
        self.every = max(5, min(60, minutes)) * 60
        self.call = call or self.http
        self.lease_at = -LEASE_EVERY
        self.next_ask = 0.0
        self.backoff = 0
        self.inflight = False
        self.result = None
        self.said = ''
        self.said_at = None
        self.song, self.song_at = None, -SONG_GAP
        self.busy_since, self.busy_at = None, -600
        self.complained = set()

    def http(self, body):
        req = urllib.request.Request(
            GEMINI_URL % self.model, data=json.dumps(body).encode('utf-8'),
            headers={'Content-Type': 'application/json',
                     'x-goog-api-key': self.key}, method='POST')
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read().decode('utf-8')

    def canned(self, reason):
        pool = self.p['canned'].get(reason) or self.p['canned']['tick']
        pick = [c for c in pool if c[1] != self.said] or pool
        emotion, line = random.choice(pick)
        return emotion, buddy_fold(line)

    def say(self, reason, now, np, cpu, mem):
        if not self.key:
            self.result = self.canned(reason)
            return
        hour = time.localtime(now).tm_hour
        title, artist, playing = np
        ctx = ['Why: %s.' % REASONS[reason],
               'Time of day: %s.' % part_of_day(hour),
               'Computer load: CPU %s, memory %s.' % (bucket(cpu), bucket(mem)),
               'Now playing: %s.' % ('%s by %s' % (title, artist or 'unknown')
                                     if playing and title else 'nothing')]
        if self.said_at is not None:
            ctx.append('Minutes since your last line: %d. It was "%s"; do not '
                       'repeat it.' % ((now - self.said_at) // 60, self.said))
        body = gemini_body(self.p, '\n'.join(ctx))
        self.inflight = True
        threading.Thread(target=self.ask, args=(body, reason),
                         daemon=True).start()

    def ask(self, body, reason):
        try:
            got = parse_reply(self.call(body), self.p)
            self.backoff = 0
            self.result = got or self.canned(reason)
        except urllib.error.HTTPError as e:
            if e.code == 429:
                # Out of quota: rest, longer each time, and say so once.
                self.backoff = min(max(60, self.backoff * 2), BACKOFF_MAX)
                self.next_ask = time.time() + self.backoff
                self.complain('quota', 'Gemini quota reached - %s is '
                              'resting for %d s' % (self.p['name'],
                                                    self.backoff))
            else:
                self.next_ask = time.time() + 120
                self.complain(e.code, 'Gemini said HTTP %d' % e.code)
            self.result = self.canned(reason)
        except Exception as e:     # offline, DNS, timeout
            self.next_ask = time.time() + 120
            self.complain(type(e).__name__, 'Gemini unreachable: %s' % e)
            self.result = self.canned(reason)
        finally:
            self.inflight = False

    def complain(self, kind, text):
        if kind not in self.complained:      # once per kind, never the key
            self.complained.add(kind)
            print(text, file=sys.stderr)

    def trigger(self, now, cpu, np):
        title, artist, playing = np
        if self.said_at is None:
            return 'hello'
        if playing and title and (title, artist) != self.song:
            self.song = (title, artist)
            if now - self.song_at >= SONG_GAP:
                self.song_at = now
                return 'music'
        if cpu is not None and cpu >= BUSY_CPU:
            self.busy_since = self.busy_since or now
            if now - self.busy_since >= BUSY_FOR and now - self.busy_at >= 600:
                self.busy_at = now
                return 'busy'
        else:
            self.busy_since = None
        if now - self.said_at >= self.every:
            hour = time.localtime(now).tm_hour
            return 'late' if hour >= 23 or hour < 5 else 'tick'
        return None

    def lines(self, now, cpu=None, mem=None, np=('', '', False)):
        out = []
        if now - self.lease_at >= LEASE_EVERY:
            out.append('B 1')
            self.lease_at = now
        if self.result:
            emotion, line = self.result
            self.result = None
            self.said, self.said_at = line, now
            out += ['E ' + emotion.upper(), 'S ' + line]
        if self.inflight or now < self.next_ask:
            return out
        reason = self.trigger(now, cpu, np)
        if reason:
            self.said_at = self.said_at if self.said_at is not None else now
            self.next_ask = now + ASK_GAP
            self.say(reason, now, np, cpu, mem)
            if self.inflight:
                out.append('E THINKING')
            elif self.result:
                return out + self.lines(now, cpu, mem, np)
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('--port', action='append',
                    help='serial port; found when omitted (repeatable)')
    ap.add_argument('--list', action='store_true', help='list candidate ports')
    ap.add_argument('--transport', choices=('auto', 'usb', 'ble'),
                    default='auto',
                    help='how to reach the dongle (default auto); only the '
                         'Windows companion, nexus_host.ps1, does ble so far')
    ap.add_argument('--interval', type=float, default=1.0,
                    help='seconds between updates (default 1)')
    ap.add_argument('--verbose', action='store_true', help='echo every line')
    ap.add_argument('--buddy', action='store_true',
                    help="be Buddy's brain (Gemini with GEMINI_API_KEY set, "
                         'canned lines without)')
    ap.add_argument('--buddy-minutes', type=int, default=10,
                    help='minutes between unprompted lines, 5-60 (default 10)')
    ap.add_argument('--gemini-model', default=GEMINI_MODEL,
                    help='Gemini model (default %s)' % GEMINI_MODEL)
    args = ap.parse_args()

    if args.list:
        for p in dongle_ports():
            studio = is_studio(p)
            print('%-40s %s' % (p, 'ZMK Studio (answered a Studio request)'
                                if studio else
                                'host link (silent) <- used' if studio is False
                                else 'busy - Studio connected, or a companion'))
        if sys.platform == 'darwin' and not serial:
            print('macOS: pass --port /dev/cu.usbmodem... (see ls /dev/cu.*)')
        return 0

    if args.transport == 'ble':
        sys.exit('The host link over Bluetooth is not supported by this '
                 'companion yet; USB works. On Windows, nexus_host.ps1 does '
                 'both.')

    if not serial and os.name != 'posix':
        sys.exit('On Windows use nexus_host.ps1, or pip install pyserial.')

    load = Load()
    brain = None
    if args.buddy:
        # The key from the environment and nowhere else: an argument would
        # sit in the process list and the shell's history.
        key = os.environ.get('GEMINI_API_KEY', '').strip()
        brain = Brain(load_persona(), key, args.gemini_model,
                      args.buddy_minutes)
        print('buddy: %s' % ('Gemini (%s)' % args.gemini_model if key else
                             'canned lines - set GEMINI_API_KEY for Gemini'))
    print('NEXUS companion running (ctrl-c to stop)')
    open_ = {}
    waiting = False

    # Reconnect rather than exit: the dongle goes away on every reflash, and
    # a companion that dies on the first unplug is a companion you stop using.
    try:
        while True:
            if not open_:
                for path in args.port or candidates():
                    try:
                        open_[path] = open_port(path)
                        print('open', path)
                    except OSError as e:
                        print('%s: %s' % (path, e), file=sys.stderr)
                if not open_:
                    if not waiting:
                        print('dongle found, but no free host link port - '
                              'another copy running? (--list shows them)'
                              if not args.port and dongle_ports()
                              else 'waiting for the dongle')
                        waiting = True
                    time.sleep(3)
                    continue
                waiting = False
                state = {'clock': 0.0, 'np': None}   # a fresh link: resend
                if brain:
                    brain.lease_at = -LEASE_EVERY

            lines = batch(state, load)
            if brain:
                # In the same write as the rest, so they share one packet.
                lines += brain.lines(time.time(), state.get('cpu'),
                                     state.get('mem'),
                                     state.get('np_now', ('', '', False)))
            data = ''.join(line + '\n' for line in lines)
            if args.verbose:
                print(data, end='')
            for path in list(open_):
                try:
                    open_[path].write(data.encode('ascii', 'replace'))
                except OSError as e:
                    print('%s lost: %s' % (path, e), file=sys.stderr)
                    try:
                        open_.pop(path).close()
                    except OSError:
                        pass
            time.sleep(args.interval)
    except KeyboardInterrupt:
        for s in open_.values():
            try:
                # "I am going away", not stale data - and B 0 first, so the
                # face is Buddy's own again now rather than in a minute.
                s.write((b'B 0\n' if brain else b'') + b'X\n')
                s.close()
            except OSError:
                pass
        print('\nstopped')
        return 0


if __name__ == '__main__':
    sys.exit(main())
