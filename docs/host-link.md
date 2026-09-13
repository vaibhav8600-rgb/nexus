# Host link

Everything else on this device comes from ZMK, which knows about the keyboard
and nothing else. The host link is the other half: a small program on the
machine the dongle is plugged into, telling it what only that machine knows.

The clock is the reason it exists. The dongle has no RTC, so the only real time
it will ever see is a time somebody hands it.

```
  companion (your PC)  ──USB serial──►  dongle   ──►  HOST screen
     time, date, CPU,                    parses,       clock, meters,
     RAM, now playing                    forgets load  now playing
                                         after 5 s
```

**One way.** The dongle never writes back. The companion cannot type, press
keys, read your keymap or ask the dongle anything -- it can only push these
fields at it. That is a design constraint, not an oversight.

## Turning it on

Three things, because two of them are yours: NEXUS will not add a USB serial
port to your machine unless you ask it to.

**1. The Kconfig, in your config repo:**

```conf
CONFIG_NEXUS_HOST_LINK=y
```

It is off by default, and off means none of this exists: no host link code is
compiled, no devicetree node is needed, no second serial port appears, and
the HOST screen, the `COMPANION` row and the clock on the home plate are all
left out. A config that never mentions it builds and behaves exactly as NEXUS
did before the host link existed. A test holds every call into the host link
to that.

**2. The devicetree node,** in your config's `nexus_dongle.overlay`. This lives
in your repo rather than in the module because a module that references
`&zephyr_udc0` would fail to build for anyone whose board does not define it:

```dts
&zephyr_udc0 {
    nexus_host_cdc: nexus_host_cdc {
        compatible = "zephyr,cdc-acm-uart";
    };
};
```

The firmware will not build without it -- the `#error` in `host_link.c` points
back here rather than failing somewhere confusing.

**3. The companion,** on the machine -- optional, and it runs with nothing
installed on Windows, Linux or macOS.

What you get depends on how much of it you do, and the first row needs
nothing at all:

| On the host | HOST screen shows |
| --- | --- |
| Nothing | How long this host has been connected (`2H 14M`) |
| The companion ran once since the dongle powered up | Time and date, still counting after it stops |
| The companion running | Time and date, CPU, RAM, title and artist |

Why the first row is not the whole screen: USB has no way for a device to ask
the host anything. A keyboard never learns the time, the CPU load or what is
playing unless something on the machine tells it -- every keyboard with a
clock either carries a coin-cell RTC or runs a companion. How long a host has
been connected is the one thing the dongle can know by itself, so that is what
it shows. It resets when the host sleeps.

The time and date also appear on the home screen, either side of the NEXUS
name, as soon as a companion has sent them.

The clock and date survive the companion stopping, because they stay true for
as long as the dongle has power: it counts forward on its own. Load and track
do not -- they go to dashes a few seconds after the companion goes quiet.

On **Windows**, double-click **`tools\nexus-host\install.cmd`**. That is the
whole setup. No drivers -- Windows binds its own `usbser.sys` to the dongle's
serial interface -- no admin prompt, and nothing to install. It copies the
companion to `%LOCALAPPDATA%\NEXUS`, sets it to start with Windows, and
starts it minimised. From then on it starts when you log in and reconnects by
itself across reflashes, reboots and unplugs. It is listed in Task Manager
under **Startup apps** as `NEXUS companion`, where you can switch it off like
anything else; `uninstall.cmd` removes it entirely.

To run it by hand instead:

```powershell
powershell -ExecutionPolicy Bypass -File tools\nexus-host\nexus_host.ps1
```

`-ExecutionPolicy Bypass` because a repo downloaded as a ZIP carries the
mark-of-the-web and will not run otherwise.

**Which port.** With ZMK Studio built in, the dongle has two serial ports:
Studio's and the host link, in an order that depends on the build. The
companion finds out rather than guessing: it sends each port a read-only
Studio request (get lock state). Studio's port answers; the host link is one
way and stays silent, so the silent one is used and Studio's is left alone -
Studio can still connect while the companion runs. `-List` shows what each
port did. To skip the check, pin it with `-Port COM14`;
`nexus_host.ps1 -Install -Port COM14` pins it in the installed copy too.

On **Linux and macOS**, the Python one. Same protocol, and nothing but the
Python that is already there:

```sh
python3 tools/nexus-host/nexus_host.py --list    # the ports it will use
python3 tools/nexus-host/nexus_host.py           # run it
```

It opens the serial port as the tty it is, reads CPU and memory from `/proc`
on Linux, and gets now playing from `playerctl` on Linux or the Music and
Spotify apps on macOS. `pyserial` and `psutil` are used when they happen to be
installed and never required -- on macOS, `psutil` is what adds CPU and RAM.

- **Linux:** the ports are found by name under `/dev/serial/by-id/`, and as
  on Windows the one that does not answer a Studio request is used. Writing
  to it needs group `dialout` (`sudo usermod -aG dialout $USER`, then log
  back in).
- **macOS:** without pyserial it cannot tell the dongle from any other USB
  serial device, so pass the port: `--port /dev/cu.usbmodem...`. The first
  now-playing read asks for permission to talk to Music or Spotify; say yes.

Then open the HOST screen on the dongle: `SETTINGS -> COMPANION`, where the
row reads `LINKED` when the companion is talking to it.

On Linux and macOS, start it with your session the usual way -- a systemd
user unit or a launchd agent. It reconnects on its own, so it can simply be
left running.

A menu walk is no way to read a clock, so there is also a direct action,
`NEXUS_ACT_HOST`, to bind to a key:

```dts
#define NX_HOST  &nexus_action NEXUS_ACT_HOST
```

## The protocol

Lines of text, `\n` terminated, at any rate you like. One letter, a space, a
value. Unknown letters are **ignored**, so a newer companion can talk to older
firmware without breaking it.

| Line | Meaning |
| --- | --- |
| `T 48720` | Local time as seconds since midnight. `48720` is 13:32. |
| `D 20709` | Local date as days since 1970-01-01. `20709` is Sunday 13 September 2026. Send it straight after `T`, from the same instant. |
| `C 37` | CPU load, 0-100. |
| `M 62` | Memory used, 0-100. Drawn as `RAM`. |
| `N So What` | The track title. Empty clears it. Truncated at 39 characters. |
| `A Miles Davis` | Its artist, the same way. |
| `P 1` | Playing (`1`) or paused (`0`). The level bars move only while it plays. Never sent means playing. |
| `X` | The companion is quitting. Load and track read unknown again immediately; the clock and date keep counting. |

`D`, `A` and `P` are newer than the rest. Firmware from before them ignores
them, and shows the title alone. A companion from before them sends `N Artist -
Title`, which newer firmware shows as the title, with no artist line, and
sends no `P`, so its track reads as playing.

Out-of-range numbers are treated as unknown rather than clamped: a companion
that sends `C 900` has a bug, and showing `100%` would hide it. So is a number
with anything after it -- `C 37abc` is a mangled line, not 37.

Send lines as fast as you like, back to back in one write: the dongle queues
bytes in a 256 byte ring and assembles lines off the interrupt, so a whole
update in one USB packet arrives whole.

You can drive it by hand, which is the main reason it is text:

```sh
echo "T 48720" > /dev/ttyACM1      # or a terminal on COMx
```

## Staleness

`CONFIG_NEXUS_HOST_STALE_S` (default 5) is how long the dongle keeps
believing what it was told. After that CPU, RAM and the track go back to
dashes, and the pill says `NO LINK`. The clock and date keep counting: they
do not go stale, they drift - slowly, and a slow drift is not what
staleness is for.

This matters more than it looks. A CPU meter frozen at 3% because the cable
came out is worse than an empty one -- it looks like it is working. The dongle
would rather show you nothing than something that stopped being true.

## The clock, and drift

`T` is seconds since **local** midnight, not a Unix epoch. The dongle has no
timezone database and no business having one; your machine already knows what
the clock on its own wall says.

The dongle draws it as **12 hour with AM or PM** by default, which is what most
desk clocks show. `CONFIG_NEXUS_HOST_CLOCK_24H=y` switches to 14:32. That is
only how it is drawn -- the companion sends the same seconds-since-midnight
either way, so changing it needs nothing on the host.

Between updates the dongle counts forward with its kernel uptime, which drifts.
The companion resends `T` and `D` every minute, so the drift never
accumulates past that. Seconds are not displayed, on purpose: a seconds digit would repaint that
card once a second forever, which is exactly what a screen sitting idle on a
desk must not do.

## What it costs, and what it cannot do

- **USB only.** On BLE to a phone or a TV there is no companion, and the HOST
  screen reads `NO LINK`. That is correct, not a failure.
- **Now playing is per-OS and best effort.** The PowerShell companion reads
  Windows' own media session -- the one the volume flyout shows -- so any app
  that reports to it works with nothing installed. On macOS the Music and
  Spotify apps answer with nothing installed; on Linux it is `playerctl`, which
  most desktops have. Without one the field is empty; everything else still
  works.
- **The panel's font is ASCII 32-90: space through Z, upper case only.** Track
  titles are folded to that on arrival -- lower case folds up, anything else is
  dropped. The firmware does it rather than the companions, because the
  constraint belongs to the font, so no companion needs to know about it.
- **CPU and RAM on macOS need psutil.** Linux reads `/proc` and Windows asks
  its own performance counters; macOS has no stdlib equivalent, so without
  psutil those two cards stay dashes there.
- **One more USB interface.** The dongle already exposes one serial port for
  ZMK Studio; this is a second. If your machine is short of USB endpoints,
  this is the thing to turn off first.
- **About half a kilobyte of RAM** on the dongle: the 256 byte receive ring,
  a line buffer, and the model with its title and artist. None of it is in the
  compositor band that `UI STATIC` reports.

## Privacy

Now-playing is a window into what you are doing, and it leaves your machine
over a cable to a device on your desk. It goes nowhere else -- the dongle has
no storage and no way to send it on -- but if you would rather it did not
happen at all, do not install the per-OS helper and the field stays empty.
