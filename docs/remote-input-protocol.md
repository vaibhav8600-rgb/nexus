# Remote Input protocol, version 1

The wire format between the NEXUS Remote app and the dongle. The firmware
(`src/remote/`) and the app (its `src/protocol/`) are both built against
this page, and the app's unit tests check its encoders against the example
bytes below -- change one and the tests say so.

Setup, pairing and the keymap are in [remote-input.md](remote-input.md).

## Rules that apply to everything

- All multi-byte fields are **little-endian**.
- Every characteristic needs an **encrypted, authenticated (LE Secure
  Connections, passkey) link**. The first read of Status on a fresh phone is
  what triggers pairing; do that before anything else, because a Write Without
  Response on an unencrypted link is silently dropped.
- While remote mode is **off**, every write fails and nothing reaches the
  computer, and NEXUS stops advertising to phones. A phone already
  connected stays connected and can still read Status, so the app can say
  why.
- One phone is connected at a time. Two may be bonded.

## Service

Base UUID `7e4e0000-5c1a-4b2e-9d3f-8a6b4c2d1e0f`; each UUID below replaces the
`0000` in the first group.

| Characteristic | UUID | Properties | Size |
| --- | --- | --- | --- |
| Service | `7e4e0001-…` | | |
| Mouse | `7e4e0002-…` | Write Without Response | 8 |
| Key | `7e4e0003-…` | Write | 6 |
| Text | `7e4e0004-…` | Write | 1-244 |
| Control | `7e4e0005-…` | Write | 1-2 |
| Status | `7e4e0006-…` | Read, Notify | 6 |

The dongle advertises as **NEXUS Remote** with the service UUID in the
advertising packet, so a scan filtered by the service finds it.

## Mouse (8 bytes, Write Without Response)

```
u8  buttons   bit0 left, bit1 right, bit2 middle, bit3 back, bit4 forward
i16 dx        relative X, positive = right
i16 dy        relative Y, positive = down
i8  wheel     vertical scroll, positive = up
i8  hwheel    horizontal scroll, positive = right
u8  reserved  send 0
```

`buttons` is the whole current state, not an event: the dongle presses and
releases whatever differs from the last packet, so a lost packet is corrected
by the next one instead of leaving a button stuck. While any button is held the
app sends a packet at least every 250 ms, even with no movement.

Packets are merged on the dongle. Ten packets that arrive before the dongle
gets to them become one report with the summed movement and the latest buttons,
so the app can send at display rate without anything queueing up. A button
pressed and released inside that window still clicks - it goes out down, then
up - but a double click needs its two presses in separate frames, which is
how the app sends them anyway.

Example -- left button held, 5 right, 3 up, one notch of scroll up:

```
01 05 00 FD FF 01 00 00
```

## Key (6 bytes, Write)

```
u8  action    0 release, 1 press, 2 tap
u8  mods      bit0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI, 4 RCtrl, 5 RShift, 6 RAlt, 7 RGUI
u16 page      0x07 keyboard, 0x0C consumer
u16 usage     HID usage ID on that page
```

- **tap** sends the key with the modifiers in the same report, waits the
  typing delay, then releases both. This is what ZMK itself does for
  `&kp LC(C)`, and it is what shortcut buttons should use.
- **press** holds the modifiers, then the key, until a matching **release**.
  A press with `usage` 0 and non-zero `mods` holds just the modifiers -- that
  is a sticky Ctrl.
- **release** lets go of the key and then of the modifiers named in `mods`.

Keys are queued behind any text still being typed, so "type this, then press
Enter" arrives in that order.

Keyboard usages are 0x00-0xFF (F13-F24 are 0x68-0x73, modifiers 0xE0-0xE7).
Consumer usages are 0x001-0xFFF; the ones the stock ZMK consumer report
carries are 0x00-0xFF, which covers every media key below.

Examples:

```
02 01 07 00 06 00    tap Ctrl+C
01 00 07 00 04 00    press A
00 00 07 00 04 00    release A
01 01 07 00 00 00    hold LCtrl alone
02 00 0C 00 CD 00    tap Play/Pause
```

Useful consumer usages: 0xE9 volume up, 0xEA volume down, 0xE2 mute, 0xCD
play/pause, 0xB5 next, 0xB6 previous, 0x6F brightness up, 0x70 brightness
down.

## Text (1-244 bytes, Write)

```
u8[n] text    printable ASCII 0x20-0x7E, plus \n (Enter) and \t (Tab)
```

The dongle queues the bytes and types them on a US layout at the typing speed
(Control `0x04`). A write is accepted whole or rejected whole:

- any byte outside the allowed set -> ATT error `0x13` (value not allowed)
- not enough room in the queue     -> ATT error `0x81`

The app keeps each write within the link's MTU (20 bytes is always safe), and
never sends more than the Status `text_free` it last saw. That is the flow
control; the error is only the backstop.

Example -- `Hi` then Enter: `48 69 0A`

## Control (Write)

| Opcode | Payload | Effect |
| --- | --- | --- |
| `0x01` | | Release every key and button this service is holding |
| `0x02` | | Keepalive while a key or button is held |
| `0x03` | | Cancel queued text (and release its key) |
| `0x04` | `u8` ms | Typing delay, 2-50 ms, default 8. Not saved; resets on reconnect |
| `0x05` | | Identify: show NEXUS on the screen and beep once |

Examples: `01` release all, `04 0A` typing delay 10 ms.

## Status (6 bytes, Read and Notify)

```
u8  version     1
u8  state       bit0 remote enabled, bit1 USB connected,
                bit2 typing in progress, bit3 pairing window open
u8  host_leds   bit0 Num Lock, bit1 Caps Lock, bit2 Scroll Lock
u16 text_free   free bytes in the text queue
u8  features    bit0 text, bit1 consumer keys, bit2 horizontal scroll
```

Notified on any change, at most every 100 ms.

Example -- enabled, USB up, Caps Lock on, 512 bytes free, all features:

```
01 03 02 00 02 07
```

## ATT errors

| Code | Meaning |
| --- | --- |
| `0x0D` | Wrong length for this characteristic |
| `0x13` | A byte or field outside the allowed values |
| `0x80` | Remote mode is off |
| `0x81` | Text queue full |

Web Bluetooth reports every one of these as the same generic failure, so the
app treats a failed write as "read Status and look" rather than parsing codes.

## Safety

- Held keys and buttons are released after 1000 ms
  (`CONFIG_NEXUS_REMOTE_INPUT_HOLD_TIMEOUT_MS`) without a Mouse, Key or
  keepalive packet.
- Everything is released, and queued text dropped, on disconnect and when
  remote mode is turned off.

## Versioning

`version` goes up only for a change an old app would get wrong. New fields go
at the end of Status and new opcodes into Control; an app ignores what it does
not know, and checks `features` before offering something.
