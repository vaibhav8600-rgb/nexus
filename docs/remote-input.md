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
advertiser, no mouse in the HID descriptor.

**2. Room for the phone.** If your config sets these outright - a
`central_dongle.conf` usually does - raise them there, because a `.conf`
beats every default the module can set:

```conf
CONFIG_BT_MAX_CONN=8     # was 7: +1, one phone connected at a time
CONFIG_BT_MAX_PAIRED=9   # was 7: +2, two phones bonded
```

ZMK counts its host profiles as `BT_MAX_PAIRED` minus the two halves, so you
will also see two more (unused) BLE profiles. There is no separate knob for
the phones' share. If you leave both at 7 it still works as long as there is
a free bond slot and a free connection - which on a USB dongle with no BLE
hosts there usually is - but pairing a phone into a full table fails.

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
2. In the app, **Connect**, and pick **NEXUS Remote**.
3. The phone asks for a code. NEXUS shows six digits; type them in.
4. Done. The phone reconnects by itself from now on - no window, no code.

Outside the window pairing is refused, so nobody in range can pair without
pressing a key on your keyboard. Up to two phones can be paired; forget them
with `NEXUS_ACT_REMOTE_CLEAR` to pair a third, or to re-pair one that has
forgotten NEXUS itself.

## The app

`app/` in this repo: a web app (PWA) in React, Vite and TypeScript that
talks to NEXUS with Web Bluetooth. One build for every phone, nothing to sign
or put in a store. CI deploys it to GitHub Pages from `main`; turn Pages on
once under **Settings > Pages > Source: GitHub Actions**.

| Phone | How |
| --- | --- |
| Android | Open the Pages URL in **Chrome**, tap Connect. Menu > **Add to Home screen** for a full-screen app. |
| iPhone | Safari has no Web Bluetooth. Open the URL in **Bluefy** (free, App Store). |

Screens: **Trackpad** (tap, two-finger tap, three-finger tap, two-finger
scroll, tap-then-drag, L/M/R buttons, drag lock, quick keys), **Keyboard**
(send box with progress and cancel, live typing, F-keys and navigation,
sticky Ctrl/Shift/Alt/Win, shortcut chips for Windows, macOS or Linux),
**Media**, and **Settings** (pointer speed and acceleration, scroll speed and
direction, tap to click, typing speed, haptics, keep awake, theme, identify,
forget).

The app sends at most one mouse packet per display frame and nothing when
idle; text is handed over only as fast as NEXUS reports room for it. When the
phone backgrounds the app the link drops, NEXUS releases everything, and the
app reconnects when it comes back.

Develop with `npm install`, `npm run dev` (serves on your LAN - Chrome on
Android allows Web Bluetooth from `localhost` or HTTPS only, so test on the
phone against the Pages build or a tunnel), and `npm test`.

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
- **Pairing**: the countdown, then the passkey. Press the button to cancel.
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

**The phone is never a ZMK host.** ZMK's BLE code sends keystrokes to, and
takes pairings for, whatever connects to its advertiser on the default
identity. The phone connects to a second one: NEXUS creates a second
Bluetooth identity (`BT_ID_MAX=2`) and runs a second advertiser on it
(`BT_EXT_ADV`, two sets), named "NEXUS Remote" and carrying only this
service's UUID. The phone's bonds are stored under that identity, which is
also why forgetting phones (`bt_unpair(id, NULL)`) cannot touch a host's or a
half's bond.

**The phone cannot see NEXUS as a keyboard.** Zephyr keeps one GATT database
for every connection, so the phone discovers ZMK's HID service whatever
identity it connected to - and a phone that bonds to something with a HID
service may adopt it as a hardware keyboard and hide its own on-screen one.
`BT_GATT_AUTHORIZATION_CUSTOM` closes that: on the phone's link, every read
and write inside the HID service's handle range fails, so the phone's OS can
never load the report map. The same callback refuses this service to every
other connection.

**Pairing without touching ZMK's callbacks.** Zephyr allows one global set of
pairing callbacks and ZMK owns it. `bt_conn_auth_cb_overlay()` replaces them
for one connection, so the phone's link gets a passkey display (which makes it
a display-only device, so LE Secure Connections uses passkey entry) and the
pairing-window check, and every other link keeps ZMK's.

**One thing ZMK still does.** ZMK's `auth_pairing_complete()` checks a new
pairing's role but not its identity, so when the active BLE profile is open,
ZMK records the phone's address in it. Nothing is ever sent there - ZMK looks
profiles up on the default identity - but the profile would read as taken.
`zmk_fix_fn()` in `remote.c` gives it back with `zmk_ble_clear_bonds()`. The
upstream fix is one identity check in ZMK; the workaround is marked to go
when that lands.

ZMK's `connected()` also runs for the phone and restarts its own advertising,
which it finds already running and logs `Advertising failed to start
(err -120)`. That line is harmless.

## Memory and timing

| | |
| --- | --- |
| RAM | the text queue (512), one more connection and advertising set in the Bluetooth stack, and under 100 bytes of state. `arm-zephyr-eabi-size` in CI prints the real total for the build with it on. |
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
the dongle, and is where this is actually proven:

- [ ] Flag off: behaves exactly like `main` (Sofle, USB, display, Studio,
      games).
- [ ] Split halves reconnect after a dongle reset while a phone is connected.
- [ ] ZMK Studio still connects over USB.
- [ ] iPhone and Android: pairing only inside the window, passkey from the
      screen; a write before pairing is refused.
- [ ] iPhone and Android: after pairing, the phone's own on-screen keyboard
      still appears (the phone did not adopt NEXUS as a keyboard).
- [ ] The active BLE profile is still open after a phone pairs.
- [ ] Sofle typing and phone input at the same time.
- [ ] 500-character paste arrives exactly, default speed, Windows and macOS.
- [ ] Every special key, shortcut and media key, Windows and macOS.
- [ ] Kill the app mid-drag and mid-key-hold: everything released within 1 s.
- [ ] Remote off: nothing from the phone reaches the computer.

Before the app exists, drive it from **nRF Connect** on a phone: connect to
NEXUS Remote, read Status (this pairs), then write the example bytes from the
protocol page to each characteristic.

## Known limits

- US keyboard layout for typed text. A host set to another layout gets the
  keys a US keyboard would press.
- A Shift held on the Sofle while text is typing changes the case of what is
  typed; the HID report is shared, which is also what lets both work at once.
- One phone connected at a time.
- macOS usually wants F14/F15 for brightness rather than the consumer keys.
