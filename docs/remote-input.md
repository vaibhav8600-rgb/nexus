# Remote Input

Type and point on the computer NEXUS is plugged into, from a phone across the
room, with nothing installed on the computer.

```
  phone (NEXUS app)  ──BLE, own service──►  dongle  ──ZMK HID──►  USB  ──►  computer
     trackpad, keys,                          │                         sees the same
     text, media                              └── Sofle halves, as ever  keyboard + mouse
```

The phone talks to one custom GATT service. Everything after that is ZMK's
normal HID path, so the computer sees one ordinary USB keyboard and mouse,
and the Sofle keeps typing at the same time. The wire format is in
[remote-input-protocol.md](remote-input-protocol.md).

## Turning it on

**1. The Kconfig**, in your config repo's `nexus_dongle.conf`:

```conf
CONFIG_NEXUS_REMOTE_INPUT=y
```

Off by default, and off means none of it is compiled: no service, no second
identity, no mouse in the HID descriptor.

**2. Room for the phone.** If a shield `.conf` in your config sets these
outright - a `central_dongle.conf` usually does - override them in the same
file as step 1. Your `nexus_dongle.conf` is applied after the shield's, and
only to the NEXUS build, so a dongle firmware that shares
`central_dongle.conf` keeps its own numbers:

```conf
CONFIG_BT_MAX_CONN=8     # was 7: +1, one phone connected at a time
CONFIG_BT_MAX_PAIRED=9   # was 7: +2, two phones bonded
```

A phone pairs into a ZMK Bluetooth profile, and ZMK counts its profiles as
`BT_MAX_PAIRED` minus the two halves - so the two extra bond slots are the
two profiles the phones take, and every host profile you had stays free. If
you leave both at 7 it still works while a profile is free, which on a USB
dongle with no BLE hosts there usually is.

**3. Keys**, optionally. Settings has a `PHONE` row - its value is `ON`,
`OFF` or `LINKED`, and selecting it opens the pairing window - so the
dongle's own button is enough to pair. Turning remote input on and off, and
forgetting phones, are keymap actions; bind them anywhere:

```dts
&nexus_action NEXUS_ACT_REMOTE_PAIR     // open the 60 s pairing window
&nexus_action NEXUS_ACT_REMOTE_TOGGLE   // phone input on / off (remembered)
&nexus_action NEXUS_ACT_REMOTE_CLEAR    // forget paired phones
```

They are harmless no-ops in a build without Remote Input, so a keymap shared
with other firmwares can keep them.

## Pairing a phone

1. Settings > `PHONE` (or the pair key). NEXUS shows a 60-second countdown.
2. In the app, **Connect**, and pick **NEXUS** (the dongle's keyboard name).
3. The phone asks for a code. NEXUS shows six digits; type them in.
4. Done. The phone reconnects by itself from now on - no window, no code.

While the window is open, NEXUS switches ZMK to a free Bluetooth profile -
the highest one, leaving the low ones for hosts - and switches back when it
closes. Over USB that changes nothing you can see. Outside the window a phone
cannot pair, so nobody in range can pair without pressing a key on your
keyboard. Up to two phones can be paired; forget them with
`NEXUS_ACT_REMOTE_CLEAR` to pair a third, or to re-pair one that has forgotten
NEXUS itself. With no phone slot or no free profile, NEXUS says **NO ROOM**.

**If the phone will not pair, check its own Bluetooth list first.** A phone
that has ever paired with NEXUS - an earlier attempt, a firmware since
reflashed, phones forgotten on the dongle - keeps a record of it under
Settings > Bluetooth, and that stale record makes every new attempt fail.
Tap NEXUS there, **Forget This Device**, then pair again.

## The app

**NEXUS Remote** is its own project, not part of this module: a web app
(PWA) in React, Vite and TypeScript that talks to NEXUS with Web Bluetooth,
hosted on Vercel. One build for every phone, nothing to sign or put in a
store. It is built against [the protocol page](remote-input-protocol.md) and
nothing else, so either side can change as long as that page holds.

| Phone | How |
| --- | --- |
| Android | Open the app's URL in **Chrome**, tap Connect. Menu > **Add to Home screen** for a full-screen app. |
| iPhone | Safari has no Web Bluetooth. Open the URL in **Bluefy** (free, App Store). |

When the phone backgrounds the app the link drops, NEXUS releases
everything, and the app reconnects when it comes back.

## Options

| Option | Default | |
| --- | --- | --- |
| `CONFIG_NEXUS_REMOTE_INPUT` | n | The whole feature |
| `CONFIG_NEXUS_REMOTE_INPUT_TEXT_QUEUE_SIZE` | 512 | Text and keys waiting to be typed, bytes of RAM |
| `CONFIG_NEXUS_REMOTE_INPUT_TYPE_DELAY_MS` | 8 | Delay after each key report; about 60 characters a second |
| `CONFIG_NEXUS_REMOTE_INPUT_HOLD_TIMEOUT_MS` | 1000 | Release everything this long after the phone goes quiet |

The queue does not need to hold a whole paste: the app sends only what fits
and tops it up as NEXUS types, so 512 bytes types a 5,000-character paste as
well as 4 KB would.

## On the screen

- **Home**, top-left corner of the brand plate: a phone outline when Remote
  Input is on and waiting, filled in the accent colour when a phone is
  connected, green while it is typing, and a grey outline when a phone is
  connected but you have switched remote input off. Nothing at all when it
  is off and no phone is there.
- **Pairing**: the countdown, then the passkey, then **PAIRED**. A wrong
  passkey goes back to the countdown for another try; press the button to
  cancel. **NO ROOM** when two phones are paired or no profile is free.
- **Identify** (from the app): the screen flashes and NEXUS beeps once.

## Safety

- Every write needs an encrypted, passkey-authenticated link.
- With remote input off, every write from the phone is refused and nothing
  reaches the computer.
- Keys and buttons the phone holds are released after a second without
  traffic, on disconnect, and when remote input is switched off. Queued text
  is dropped on disconnect and on switch-off.

## How it stays out of ZMK's way

This is the part to read before touching `src/remote/`.

**The phone is a ZMK profile, and that is all the radio sees.** It finds
NEXUS through ZMK's own advertising, pairs into a free profile slot, and
reconnects the way any bonded host does. Nothing about ZMK's Bluetooth
changes - no second advertiser, no second identity - so BLE hosts, the
halves and Studio behave exactly as on `main`. Two other designs were tried
on the hardware and both broke that:

- A second advertiser needs extended advertising (`BT_EXT_ADV`), which hangs
  this dongle in Bluetooth controller start-up, before USB or the display -
  with Remote Input's code compiled out entirely.
- A second identity on the one legacy advertiser cannot advertise while the
  dongle scans for or connects to its halves: they share one random address,
  so the phone's advertising never started and nothing was discoverable.

**A phone never acts as a keyboard host.** ZMK sends reports only to the
active profile, and a phone's profile is active only for the seconds it takes
to pair. More than that, `BT_GATT_AUTHORIZATION_CUSTOM` refuses every read
and write in ZMK's HID service on a phone's link, so the phone's OS can
neither subscribe to a report nor load the report map and adopt NEXUS as a
hardware keyboard - which is what would hide its own on-screen one. The same
callback keeps everyone but the phone out of the Remote Input service.

**Which links are phones.** NEXUS keeps the addresses of paired phones in its
settings (`nexus/remote/phones`). A connection from one of them is a phone;
so is whichever connection arrives while the pairing window is open. A BLE
host that connects in that same minute is taken for the phone - keep the
window for pairing phones.

**Pairing without touching ZMK's callbacks.** Zephyr allows one global set of
pairing callbacks and ZMK owns it. `bt_conn_auth_cb_overlay()` replaces them
for the one connection that is pairing, so it gets a passkey on the NEXUS
screen (LE Secure Connections with passkey entry) and the window check; every
other link keeps ZMK's. ZMK's own `pairing_complete` then files the phone into
the free profile, which is exactly what is wanted, and NEXUS switches back to
the profile that was active before.

## Memory and timing

| | |
| --- | --- |
| RAM | the text queue (512), one more connection in the Bluetooth stack, and under 100 bytes of state. `arm-zephyr-eabi-size` in CI prints the real total for the build with it on. |
| Threads | none added. Bluetooth callbacks validate and hand off; all HID work runs on the system work queue, the same one ZMK processes the halves' keys on. |
| Mouse | merged, not queued: packets that arrive before the work item runs become one report with summed movement and the latest buttons. |
| Link | the phone is asked for a 15 ms interval with no latency - the fastest iOS allows, so pointer updates top out around 66 per second. |
| ATT MTU | left at the default. Text goes in 20-byte writes; bigger buffers were not worth their RAM for a 60-character-a-second typist. |

## Checked against ZMK and Zephyr

ZMK `main` on Zephyr `v4.1.0+zmk-fixes`. Neither is pinned to a commit in
this module's CI or in the config repo, so re-check these after a ZMK bump:

| Planned | Used |
| --- | --- |
| `ZMK_POINTING` or `ZMK_MOUSE` | `ZMK_POINTING`; `ZMK_MOUSE` is a deprecated alias |
| input device + `zmk,input-listener` preferred | not used: `zmk_hid_mouse_*` plus `zmk_endpoint_send_mouse_report()`. The phone already applies speed and acceleration, so ZMK's input processors had nothing to add, and this needs no devicetree node in your config |
| one auth callback set | `bt_conn_auth_cb_overlay()` per connection; `bt_conn_auth_info_cb_register()` allows several |
| seq/flags on Text | dropped; see the protocol page |
| three behaviors | three `&nexus_action` IDs on the existing behavior |

## Test checklist (real hardware)

The firmware builds in CI with the flag on and off. Everything below needs
the dongle, and is where this is actually proven. Ticked: verified on the
hardware, 2026-10-01.

- [x] Flag off: behaves exactly like `main`.
- [x] Flag on: everything NEXUS and ZMK did before still works (Sofle, USB,
      display, games, BLE profiles).
- [ ] Split halves reconnect after a dongle reset while a phone is connected.
- [ ] ZMK Studio still connects over USB.
- [x] iPhone: pairs inside the window with the passkey from the screen.
- [ ] Android: the same.
- [ ] A write before pairing is refused.
- [ ] iPhone and Android: after pairing, the phone's own on-screen keyboard
      still appears (the phone did not adopt NEXUS as a keyboard).
- [ ] After pairing, the profile that was active before is active again,
      and the phone sits in the highest free profile.
- [x] A BLE host (second laptop) still pairs and types with Remote Input on.
- [ ] Sofle typing and phone input at the same time.
- [ ] 500-character paste arrives exactly, default speed, Windows and macOS.
- [ ] Every special key, shortcut and media key, Windows and macOS.
- [ ] Kill the app mid-drag and mid-key-hold: everything released within 1 s.
- [ ] Remote off: nothing from the phone reaches the computer.

Without the app, drive it from **nRF Connect** on a phone: open the PHONE
window, connect to NEXUS, read Status (this pairs), then write the example
bytes from the protocol page to each characteristic.

## Known limits

- US keyboard layout for typed text. A host set to another layout gets the
  keys a US keyboard would press.
- A Shift held on the Sofle while text is typing changes the case of what is
  typed; the HID report is shared, which is also what lets both work at once.
- One phone connected at a time.
- A phone reconnects through ZMK's advertising, which ZMK stops while its
  active BLE host is connected. Over USB it always advertises; with a BLE
  host active, connect the phone before the host, or switch to USB.
- `&bt BT_CLR_ALL` forgets phones too, as it does every bond; pair them again.
- macOS usually wants F14/F15 for brightness rather than the consumer keys.
