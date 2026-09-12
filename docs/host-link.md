# Host link

Everything else on this device comes from ZMK, which knows about the keyboard
and nothing else. The host link is the other half: a small program on the
machine the dongle is plugged into, telling it what only that machine knows.

The clock is the reason it exists. The dongle has no RTC, so the only real time
it will ever see is a time somebody hands it.

```
  companion (your PC)  ──USB serial──►  dongle   ──►  HOST screen
     time, CPU, memory,                  parses,       clock, meters,
     now playing                         forgets       now playing
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

**3. The companion:**

```sh
pip install pyserial psutil
python tools/nexus-host/nexus_host.py --list    # find the port
python tools/nexus-host/nexus_host.py           # run it
```

Then open the HOST screen on the dongle: `SETTINGS -> COMPANION`, where the
row reads `LINKED` when the companion is talking to it.

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
| `C 37` | CPU load, 0-100. |
| `M 62` | Memory used, 0-100. |
| `N Artist - Title` | Now playing. Empty clears it. Truncated at 39 characters. |
| `X` | The companion is quitting. Everything reads unknown again immediately. |

Out-of-range numbers are treated as unknown rather than clamped: a companion
that sends `C 900` has a bug, and showing `100%` would hide it.

You can drive it by hand, which is the main reason it is text:

```sh
echo "T 48720" > /dev/ttyACM1      # or a terminal on COMx
```

## Staleness

`CONFIG_NEXUS_HOST_STALE_S` (default 5) is how long the dongle keeps
believing what it was told. After that the HOST screen goes back to dashes.

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
The companion resends `T` every minute, so the drift never accumulates past
that. Seconds are not displayed, on purpose: a seconds digit would repaint that
card once a second forever, which is exactly what a screen sitting idle on a
desk must not do.

## What it costs, and what it cannot do

- **USB only.** On BLE to a phone or a TV there is no companion, and the HOST
  screen reads `NO LINK`. That is correct, not a failure.
- **Now playing is per-OS and best effort.** The PowerShell companion reads
  Windows' own media session -- the one the volume flyout shows -- so any app
  that reports to it works with nothing installed. Elsewhere it is `playerctl`
  on Linux and `nowplaying-cli` on macOS. Without one the field is empty;
  everything else still works.
- **The panel's font is ASCII 32-90: space through Z, upper case only.** Track
  titles are folded to that on arrival -- lower case folds up, anything else is
  dropped. The firmware does it rather than the companions, because the
  constraint belongs to the font, so no companion needs to know about it.
- **psutil is optional.** Without it the companion sends only the clock.
- **One more USB interface.** The dongle already exposes one serial port for
  ZMK Studio; this is a second. If your machine is short of USB endpoints,
  this is the thing to turn off first.
- **About 100 bytes of RAM** on the dongle for the model and the line buffers,
  none of it in the compositor band that `UI STATIC` reports.

## Privacy

Now-playing is a window into what you are doing, and it leaves your machine
over a cable to a device on your desk. It goes nowhere else -- the dongle has
no storage and no way to send it on -- but if you would rather it did not
happen at all, do not install the per-OS helper and the field stays empty.
