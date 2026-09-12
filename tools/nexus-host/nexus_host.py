#!/usr/bin/env python3
"""NEXUS companion: push what only this machine knows to the dongle.

One way, one line at a time, over the USB serial the dongle exposes when
CONFIG_NEXUS_HOST_LINK is on. The dongle never answers, so this can only ever
tell it things - it cannot type, press keys, or read anything back.

    python nexus_host.py --list          # which port is the dongle?
    python nexus_host.py                 # guess the port and run
    python nexus_host.py --port COM7

Needs pyserial. psutil is optional and only buys CPU and memory; without it
the clock still works, which is the part that cannot come from anywhere else.
Now-playing is best effort and per-OS - see docs/host-link.md.
"""
import argparse
import shutil
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit('pyserial is required:  pip install pyserial')

try:
    import psutil
except ImportError:
    psutil = None

TEXT_MAX = 39          # NEXUS_HOST_TEXT - 1, the dongle truncates anyway
CLOCK_EVERY = 60       # seconds between clock resyncs


def find_port():
    """The dongle, or None. ZMK's USB VID is the first thing to look for."""
    best = None
    for p in list_ports.comports():
        blob = ' '.join(str(x) for x in
                        (p.description, p.manufacturer, p.product) if x)
        if p.vid == 0x1D50:                      # ZMK / OpenMoko range
            return p.device
        if 'nexus' in blob.lower() or 'zmk' in blob.lower():
            best = best or p.device
    return best


def now_playing():
    """Whatever this OS will tell us, or ''. Never raises, never hangs."""
    try:
        if sys.platform.startswith('linux') and shutil.which('playerctl'):
            out = subprocess.run(
                ['playerctl', 'metadata', '--format',
                 '{{artist}} - {{title}}'],
                capture_output=True, text=True, timeout=1)
            return out.stdout.strip() if out.returncode == 0 else ''
        if sys.platform == 'darwin' and shutil.which('nowplaying-cli'):
            out = subprocess.run(['nowplaying-cli', 'get', 'artist', 'title'],
                                 capture_output=True, text=True, timeout=1)
            parts = [x for x in out.stdout.split('\n') if x.strip()]
            return ' - '.join(parts[:2]) if parts else ''
        if sys.platform == 'win32':
            # winsdk is a big optional dependency; only used if it is there.
            from winsdk.windows.media.control import \
                GlobalSystemMediaTransportControlsSessionManager as Mgr
            import asyncio

            async def get():
                mgr = await Mgr.request_async()
                s = mgr.get_current_session()
                if not s:
                    return ''
                info = await s.try_get_media_properties_async()
                return ' - '.join(x for x in (info.artist, info.title) if x)
            return asyncio.run(get())
    except Exception:
        pass
    return ''


def lines(last_clock, last_np):
    """The next batch to send, plus the state that decides the batch after."""
    out = []
    now = time.time()

    if now - last_clock >= CLOCK_EVERY:
        t = time.localtime()
        out.append('T %d' % (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec))
        last_clock = now

    if psutil:
        out.append('C %d' % round(psutil.cpu_percent()))
        out.append('M %d' % round(psutil.virtual_memory().percent))

    np = now_playing()[:TEXT_MAX]
    if np != last_np:
        out.append('N %s' % np)
        last_np = np

    return out, last_clock, last_np


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', help='serial port; guessed when omitted')
    ap.add_argument('--list', action='store_true', help='list serial ports')
    ap.add_argument('--interval', type=float, default=1.0,
                    help='seconds between updates (default 1)')
    ap.add_argument('--verbose', action='store_true', help='echo every line')
    args = ap.parse_args()

    if args.list:
        for p in list_ports.comports():
            print('%-14s %s' % (p.device, p.description))
        return 0

    port = args.port or find_port()
    if not port:
        print('No dongle found. Run with --list and pass --port.',
              file=sys.stderr)
        return 1
    if psutil is None:
        print('psutil not installed: sending the clock only.', file=sys.stderr)

    print('NEXUS companion -> %s   (ctrl-c to stop)' % port)
    last_clock, last_np = 0.0, None

    # Reconnect rather than exit: the dongle goes away on every reflash, and
    # a companion that dies on the first unplug is a companion you stop using.
    while True:
        try:
            with serial.Serial(port, 115200, timeout=1, write_timeout=2) as s:
                last_clock, last_np = 0.0, None      # resend everything
                while True:
                    batch, last_clock, last_np = lines(last_clock, last_np)
                    for line in batch:
                        s.write((line + '\n').encode('utf-8', 'replace'))
                        if args.verbose:
                            print(' >', line)
                    s.flush()
                    time.sleep(args.interval)
        except KeyboardInterrupt:
            try:
                with serial.Serial(port, 115200, timeout=1) as s:
                    s.write(b'X\n')       # "I am going away", not stale data
            except Exception:
                pass
            print('\nstopped')
            return 0
        except Exception as e:
            print('link lost (%s); retrying' % e, file=sys.stderr)
            time.sleep(3)


if __name__ == '__main__':
    sys.exit(main())
