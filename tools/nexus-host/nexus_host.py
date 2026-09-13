#!/usr/bin/env python3
"""NEXUS companion: push what only this machine knows to the dongle.

One way, one line at a time, over the USB serial the dongle exposes when
CONFIG_NEXUS_HOST_LINK is on. The dongle never answers, so this can only ever
tell it things - it cannot type, press keys, or read anything back.

    python3 nexus_host.py --list          # which ports look like the dongle?
    python3 nexus_host.py                 # find them and run
    python3 nexus_host.py --port /dev/ttyACM1

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
import os
import shutil
import subprocess
import sys
import time

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
CLOCK_EVERY = 60       # seconds between clock resyncs
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
    """(title, artist) from whatever this OS will say, or ('', '').

    Never raises, never hangs. The "is running" checks matter on macOS:
    asking an app that is not running for its track launches it.
    """
    try:
        if sys.platform.startswith('linux') and shutil.which('playerctl'):
            cmd = ['playerctl', 'metadata', '--format',
                   '{{title}}\n{{artist}}']
        elif sys.platform == 'darwin':
            cmd = ['osascript'] + OSASCRIPT
        else:
            return '', ''
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=2)
        if out.returncode != 0:
            return '', ''
        parts = out.stdout.strip('\n').split('\n') + ['', '']
        return parts[0].strip(), parts[1].strip()
    except Exception:
        return '', ''


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
    if cpu is not None:
        out.append('C %d' % cpu)
    if mem is not None:
        out.append('M %d' % mem)

    title, artist = now_playing()
    np = (fold(title), fold(artist))
    if np != state['np']:
        out += ['N %s' % np[0], 'A %s' % np[1]]
        state['np'] = np

    # Never nothing: the dongle drops the link after a few silent seconds,
    # and NO LINK while this is plainly running sends you looking for a
    # fault that is not there. The clock is the cheapest thing to say.
    if not out:
        out = clock_lines(now)[:1]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('--port', action='append',
                    help='serial port; found when omitted (repeatable)')
    ap.add_argument('--list', action='store_true', help='list candidate ports')
    ap.add_argument('--interval', type=float, default=1.0,
                    help='seconds between updates (default 1)')
    ap.add_argument('--verbose', action='store_true', help='echo every line')
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

    if not serial and os.name != 'posix':
        sys.exit('On Windows use nexus_host.ps1, or pip install pyserial.')

    load = Load()
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

            data = ''.join(line + '\n' for line in batch(state, load))
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
                s.write(b'X\n')    # "I am going away", not stale data
                s.close()
            except OSError:
                pass
        print('\nstopped')
        return 0


if __name__ == '__main__':
    sys.exit(main())
