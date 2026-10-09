# Host link

Everything else on this device comes from ZMK, which knows about the keyboard
and nothing else. The host link is the other half: a small program on the
machine the dongle is connected to, telling it what only that machine knows.

The clock is the reason it exists. The dongle has no RTC, so the only real time
it will ever see is a time somebody hands it.

```
  companion (your PC)  ──USB serial──►  dongle   ──►  HOST screen
     time, date, CPU,    or Bluetooth    parses,       clock, meters,
     RAM, now playing                    forgets load  now playing
                                         after 5 s
```

It arrives over a second USB serial port, or - [since the Bluetooth
transport](#over-bluetooth) - over the Bluetooth connection the host already
has, which is what makes it work with the dongle on a charger.

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

The firmware will not build without it -- the `#error` in `host_link_usb.c`
points back here rather than failing somewhere confusing. It is the USB
transport that needs the node: a build with `CONFIG_NEXUS_HOST_LINK_USB=n`
and the Bluetooth transport on does not.

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

## Over Bluetooth

With the dongle powered from a charger and reaching the laptop over Bluetooth
alone, there is no USB serial port to write to. The Bluetooth transport gives
the companion a second way in:

```conf
CONFIG_NEXUS_HOST_LINK=y
CONFIG_NEXUS_HOST_LINK_BLE=y
```

It adds one small **write-only** service to the Bluetooth connection the host
already has as a keyboard. The companion writes the same lines to it. Nothing
else about the radio changes: no new connection, no advertising, no
connection parameters, no extra bond.

| | |
| --- | --- |
| Service | `bbb8ee45-91c5-40fb-8c33-1c8b9c4795cc` |
| Characteristic | `40064c5a-dc78-4b9e-b148-9e13bcbd7708` |
| Properties | Write Without Response. Nothing to read, notify or indicate |
| Needs | The encrypted link every paired host has |

**The HOST screen shows the host you are typing into.** With two ways in,
the dongle believes one at a time:

| Keys are going out over | Lines that count |
| --- | --- |
| USB | The USB serial port's |
| Bluetooth | Bluetooth's, from the host on the active profile only |

Lines from the other source are dropped. When you switch - `&out`, a
different `&bt BT_SEL` profile, the cable coming out - the load and the track
are cleared at once, because they were the other machine's; the clock and the
date are kept. A second paired host, a Remote Input phone and the keyboard
halves cannot write to the HOST screen at all.

So with the dongle in one PC's USB port and typing into another over
Bluetooth, HOST shows the second PC if it runs a companion, and dashes if it
does not. That is intended: the alternative is two machines' numbers on one
screen.

This rule exists only in a build with the Bluetooth transport. Without
`CONFIG_NEXUS_HOST_LINK_BLE`, the dongle listens to its USB port whatever the
output, exactly as before.

**The companion.** On Windows, the same `nexus_host.ps1`, still with nothing
to install:

```powershell
nexus_host.ps1                   # Auto: USB if it is there, Bluetooth if it is there, both if both
nexus_host.ps1 -Transport Ble    # Bluetooth only
nexus_host.ps1 -Transport Usb    # USB only, as before
nexus_host.ps1 -List             # also lists connected Bluetooth devices and whether they have the service
```

Run `install.cmd` again after updating, so the installed copy is the new one.
It finds NEXUS among the devices Windows is already connected to - it scans
for nothing and pairs with nothing - writes 20 bytes at a time, looks again
every 10 seconds while the dongle is not there, and reconnects by itself
after the PC sleeps.

An older companion only knows the USB port. With the Bluetooth transport on
and keys going out over Bluetooth it shows dashes, so update the companion
when you turn the option on.

The Python companion (Linux, macOS) is USB only for now, and says so if asked
for `--transport ble`.

Windows gives a Bluetooth service to one program at a time, as it does a
serial port. With a companion already running, `-List` reads "host link
service, in use", and a second copy waits rather than fighting over it.

**If Windows does not find the service** after a reflash - `-List` says "no
host link service" - Windows is using the list of services it remembered when
it paired. The companion asks the dongle directly once each time it starts,
which is normally enough. If it is not, remove NEXUS under Bluetooth settings
and pair it again, once.

### Test checklist (real hardware)

The firmware builds in CI in all three combinations. Everything below needs
the dongle. Ticked: run on the hardware, 2026-10-09 (Windows 11 laptop,
nice!nano dongle with both transports and Remote Input built in).

- [ ] Option off: everything as on `main` - typing, split, USB, BLE profiles,
      Studio, display, games, sound. (CI: the four builds without the host
      link are byte-identical to `main`.)
- [ ] `HOST_LINK` with USB only: HOST and the companion exactly as before.
- [x] Option on: typing, split, USB, BLE profiles, display and games as
      before.
- [x] Bluetooth on, dongle in the laptop's USB, output USB: data, via USB.
- [x] The same, output switched to BLE on the same laptop: data continues.
- [x] **Dongle on a wall charger, BLE only to the laptop: HOST shows clock,
      date, CPU, RAM and track; pause and play follow.**
- [x] Switch to a BLE profile whose host has no companion: load and track go
      to dashes at once, the clock stays. Switch back: data returns in a
      second or two, the track within ten.
- [x] Laptop sleeps and wakes: dashes after the stale time, then it recovers
      without restarting the companion.
- [ ] Bluetooth, then the dongle moved to USB, then back to a charger: the
      data returns over Bluetooth by itself, the time within ten seconds.
      (On 2026-10-09 the time took up to a minute, which read as no data
      until the companion was restarted or the output toggled: the clock was
      resent once a minute and the dongle had just lost it with its power.
      The companion now resends it every ten seconds; to be run again.)
- [x] After a reflash, Windows finds the new service with no re-pair.
- [ ] Reflash the dongle while the new companion runs: it finds the dongle
      again by itself.
- [x] Remote Input on: the iPhone connects and types as before.
- [ ] A second paired host that is not the active profile, or a phone, writes
      to the characteristic (nRF Connect): nothing changes on screen.
- [x] ZMK Studio connects over USB while the companion runs.
- [x] Reset the dongle while the companion runs over BLE: both halves
      reconnect, and the data comes back.
- [x] Typing and a game of Tetris with the companion running over BLE: no
      lag, no dropped keys, no split disconnects.
- [ ] nRF Connect, from the active host: `C 4` and `2\n` as two writes shows
      CPU 42.

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
update in one USB packet arrives whole. Over Bluetooth a line may be cut
anywhere between writes; it is the same byte stream. A write the ring cannot
take whole is dropped whole, and the line it was part of is skipped rather
than read with a piece missing.

A NUL byte is not part of any line. The dongle treats one as "bytes are
missing here" and skips to the next newline.

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
The companion resends `T` and `D` every ten seconds, so the drift never
accumulates past that - and a dongle that has just lost power, and with it
the time, has it back within those ten seconds. Nothing can ask for it: the
link is one way. Seconds are not displayed, on purpose: a seconds digit would repaint that
card once a second forever, which is exactly what a screen sitting idle on a
desk must not do.

## What it costs, and what it cannot do

- **A companion has to run on the host.** On BLE to a phone or a TV there is
  none, and the HOST screen reads `NO LINK`. That is correct, not a failure.
- **Over Bluetooth, Windows only so far.** The Linux and macOS companion
  writes to the USB port.
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
  a line buffer, and the model with its title and artist. The Bluetooth
  transport adds a ring and a line buffer of its own, about 350 bytes more.
  None of it is in the compositor band that `UI STATIC` reports.

## Privacy

Now-playing is a window into what you are doing, and it leaves your machine
over a cable to a device on your desk. It goes nowhere else -- the dongle has
no storage and no way to send it on -- but if you would rather it did not
happen at all, do not install the per-OS helper and the field stays empty.
