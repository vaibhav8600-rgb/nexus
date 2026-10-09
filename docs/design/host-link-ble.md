# Design: host link over Bluetooth

Status: **approved 2026-10-09 and implemented** on `feature/host-link-ble`,
cut from `main` at `805e464`. Waiting for the hardware checklist in
[host-link.md](../host-link.md#test-checklist-real-hardware). Open questions
1 to 3 were answered as proposed. What changed while building it is under
[Found while building](#found-while-building).

## What it does

The companion on the PC (time, date, CPU, RAM, now playing) reaches the HOST
screen today only through a second USB serial port. This adds a second way
in: a small write-only GATT service on the Bluetooth connection the host
already has. The companion writes the same text lines to it. With it, the
HOST screen works when the dongle is on a wall charger and the laptop is
connected over BLE only.

Nothing changes for a build that does not turn the new option on.

## Checked before designing

| Question | Answer | How |
| --- | --- | --- |
| Can Windows reach a custom service on the already-paired NEXUS? | Yes | Ran a read-only WinRT probe from stock PowerShell 5.1 on the laptop, with NEXUS connected over BLE: it listed Remote Input's service and its five characteristics, cached and uncached. No re-pair. |
| Does a new service appear after a reflash without re-pairing? | Expected yes, to confirm on hardware | Zephyr's Service Changed is on by default (the probe saw the GATT service `0x1801`), and the companion will look services up uncached. Hardware checklist item 8 confirms it. |
| Which "endpoint" does the status model hold? | The one ZMK is actually sending to | `zmk_events.c` reads `zmk_endpoint_get_selected()`. On a wall charger with the output preference still on USB, ZMK selects BLE, and so does this. |
| Free status observer slots? | 4 of 6 in the worst case | Subscribers today: Remote Input (always) and the home screen (while shown). This adds one. |
| Where do status observers run? | On the NEXUS work queue | `notify_work_cb` in `status.c`. The same queue parses host lines, so the two never race. |
| Is the host link built in CI? | No | None of the four matrix entries sets `NEXUS_HOST_LINK`. |

Baseline on `main`, before any change: every script check and all 13 host
test files pass. Flash and RAM per CI build are in the table at the end.

## Design

### Kconfig

```kconfig
config NEXUS_HOST_LINK            # unchanged name: parser, model, HOST screen
	depends on NEXUS_DISPLAY && (ZMK_USB || ZMK_BLE)
	select RING_BUFFER

config NEXUS_HOST_LINK_USB        # today's transport
	default y
	depends on NEXUS_HOST_LINK && ZMK_USB
	select SERIAL, UART_INTERRUPT_DRIVEN, USB_CDC_ACM

config NEXUS_HOST_LINK_BLE        # new
	default n
	depends on NEXUS_HOST_LINK && ZMK_BLE
```

| Config sets | Result |
| --- | --- |
| nothing | No host link. Identical to `main`. |
| `HOST_LINK=y` (every config that has it today) | USB transport, as `main`. No edits needed. |
| `HOST_LINK=y`, `HOST_LINK_BLE=y` | USB and BLE. |
| `HOST_LINK=y`, `HOST_LINK_USB=n`, `HOST_LINK_BLE=y` | BLE only. No CDC devicetree node needed, no second serial port on the PC. |
| `HOST_LINK=y`, both transports off | Builds. The HOST screen shows only "since connected", which needs no companion. |

No symbol is renamed or removed.

### Files

| File | Holds | Built with |
| --- | --- | --- |
| `src/host/host_link.c` | The model, `parse_line()`, the stale timer, the sources, the choice of source | `HOST_LINK` |
| `src/host/host_lines.c/.h` | Bytes to lines. Pure C, no Zephyr, so it gets a real unit test | `HOST_LINK` |
| `src/host/host_link_usb.c` | The CDC UART, its interrupt, the devicetree `#error` | `HOST_LINK_USB` |
| `src/host/host_link_ble.c` | The GATT service and its write handler | `HOST_LINK_BLE` |

### One buffer per source

USB and BLE bytes can arrive in the same instant. Each source gets its own
ring (256 bytes, the size the USB ring has today) and its own partial-line
buffer (72 bytes), so one cannot splice into the other's line.

The transports only copy bytes into their ring and post the parse work to
the NEXUS work queue. Everything else happens on that queue, as now.

### Which source is believed

The HOST screen describes the host you are typing into.

- Endpoint **USB**: only USB lines are parsed.
- Endpoint **BLE**: only BLE lines are parsed, and only from the connection
  whose address is ZMK's active profile.
- Bytes from the other source are drained and thrown away.
- When the pair (endpoint, active profile) changes: CPU, RAM, track, paused
  and link are cleared as an `X` line clears them, the clock and date are
  kept, and both partial lines are reset. The status flag that reports this
  also fires when a host merely connects or drops, so the pair is compared
  with the last one seen and nothing is cleared when it is the same.

**Only in a build with the BLE transport.** A USB-only build parses every
USB line whatever the endpoint, exactly as `main` does, so nobody who leaves
the new option off sees any change.

### The GATT service

| | |
| --- | --- |
| Service | `bbb8ee45-91c5-40fb-8c33-1c8b9c4795cc` |
| Characteristic | `40064c5a-dc78-4b9e-b148-9e13bcbd7708` |
| Properties | Write Without Response. No read, notify or indicate |
| Permission | Encrypted link (`BT_GATT_PERM_WRITE_ENCRYPT`), which every bonded ZMK host has |

The write handler runs on the Bluetooth RX thread and does four things:
refuses a non-zero offset; checks the link is one where NEXUS is the
peripheral and its address is the active profile's; copies the bytes into the
BLE ring under its lock; posts the parse work. No parsing, no logging per
packet. If the ring has no room for the whole write, the whole write is
dropped and the source skips to the next newline, so half a line is never
parsed.

It uses `zmk_ble_active_profile_addr()`, not `..._conn()`: the second logs a
warning each time the profile is not connected and returns a reference that
must be released.

Nothing else on the radio changes: no connection parameters, no advertising,
no profile switching, no `BT_MAX_CONN` or `BT_MAX_PAIRED`.

### Remote Input

Remote Input owns the one GATT authorization callback. It already lets a
host, and a phone outside the HID service, reach other attributes, so the new
characteristic is reachable with no change to `remote.c`. The active-profile
check in the write handler is what keeps phones and other bonded hosts out:
a phone's profile is the active one only for the half-second in which it
pairs.

### Companion

`nexus_host.ps1` gains `-Transport Auto|Usb|Ble`, default `Auto`: it sends to
the USB port if there is one, and to a connected Bluetooth device that has
the service if there is one. Sending to both is fine; the dongle picks.
It looks for the Bluetooth device every 10 s while it has none, and drops and
re-finds it when a write fails. Writes are chunked to 20 bytes.

One change applies to both transports: **the track, artist and play state are
resent every 10 s**, not only when they change. The link is one-way, so the
companion cannot know the dongle just cleared them on a switch of host. The
dongle already ignores a value that has not changed, so this costs no
repaint.

Linux and macOS companions keep USB only for now. Each prints one line saying
so when asked for BLE.

## Concurrency

| Data | Written by | Read by | Protected by |
| --- | --- | --- | --- |
| USB ring | UART interrupt | work queue | its spinlock, held for a `memcpy` |
| BLE ring | Bluetooth RX thread | work queue | its spinlock, held for a `memcpy` |
| BLE "skip to newline" flag | Bluetooth RX thread | work queue | atomic |
| Partial-line buffers | work queue | work queue | one context |
| Host model (`g_host`) | work queue | work queue (screens draw there) | one context |
| Last (endpoint, profile) pair | work queue (status observer) | work queue | one context |
| ZMK's active profile address | ZMK | Bluetooth RX thread | read-only compare of 7 bytes; a torn read can only mis-accept or mis-drop one write |

No new threads, no heap, no work in the interrupt or the Bluetooth thread
beyond the copy.

## Cost

| Build | Flash | RAM | Why |
| --- | --- | --- | --- |
| Feature off | 0 | 0 | None of the new files compile. |
| USB only (refactor) | about +100 B | about +8 B | One struct per source instead of bare globals. |
| With BLE | about +1.5 KB | about +350 B | 256 B ring, 72 B line buffer, the service's attributes, one observer. |

Budgets from the brief: USB only at most +256 B flash and +64 B RAM; BLE at
most +3 KB flash and +512 B RAM. Real figures will come from CI.

## Alternatives rejected

| Alternative | Why not |
| --- | --- |
| Most recent source wins | With the dongle in one PC's USB and typing into another over BLE, the screen would flip between two machines' numbers. |
| BLE accepted whatever the endpoint | The same problem from the other side: USB output selected, another host's data on screen. |
| USB as fallback when BLE is silent | Keeps an old companion working on BLE output, but shows the wrong machine in the two-PC case. See open question 1. |
| Write With Response | Gives flow control, but adds a reply per packet for about 150 bytes a second. Dropping a whole write and resyncing is enough. |
| Reusing Remote Input's service | That service is for phones and needs a passkey-paired link. Hosts are bonded without one. |
| A larger ATT MTU | Costs buffers for every connection. 20-byte writes carry a full update in under ten packets. |

## Deviations from the brief

1. **BLE ring 256 bytes, not about 128.** A full update with a long title and
   artist passes 128 bytes, and the display can hold the work queue for a
   frame. 256 is what USB uses and fits the RAM budget.
2. **No thread-analyzer build.** Its output comes through logging, and
   logging builds do not boot on this dongle. Instead: no stack is resized
   and the handler does a bounded copy; if a number is wanted, the
   Diagnostics screen can show stack headroom in a later change.
3. **No Linux BLE companion yet.** It cannot be tested here. USB is
   unchanged on Linux.
4. **The source rule applies only when the BLE transport is built**, to keep
   USB-only builds behaving exactly as `main`.
5. **Companion resends the track every 10 s**, for the reason above.
6. **Testing needs the config repo**: `CONFIG_NEXUS_HOST_LINK_BLE=y` and the
   `nexus` revision pointed at this branch, on a `test/` branch there.

## Risks

| Risk | Mitigation |
| --- | --- |
| Windows caches the old service list after a reflash | Uncached lookup in the companion; Service Changed is on. If it still fails: remove and re-pair once, and the docs will say so. |
| Extra radio traffic disturbs typing or the halves | About 150 bytes a second on a link that is already open. Hardware item 13 checks it. |
| A Kconfig dependency loop from the new symbols | The selects are the ones `NEXUS_HOST_LINK` has today, moved to a child symbol. CI builds all three combinations. |
| The refactor changes USB behaviour | First commit is the split alone, with the host test's 151 checks and a USB CI build proving it. |

## Found while building

1. **The service is named `zz_nexus_host_svc`, to sort last.** Zephyr lays
   static GATT services out in name order, so a service added in the middle
   moves the handle of everything after it. `nexus_host_svc` would have
   landed before `nexus_remote_svc` and moved Remote Input's handles, which
   paired phones have cached. Last in the database, nothing existing moves.
2. **A gap marker instead of a flag.** A dropped Bluetooth write has to make
   the assembler skip the torn line. A flag beside the ring is read at the
   wrong moment when a later write has already gone in: `C 1` + dropped +
   `0` would read as CPU 10. A NUL put into the ring where the bytes are
   missing is in the right place by construction. One byte of the ring is
   kept free for it.
3. **A source that stops being believed part-way through a line** skips the
   rest of that line when it is believed again, so the tail of a title is
   never read as a line of its own.
4. **Changing profile while typing over USB is not a change of host.** The
   pair compared is (endpoint, profile only when the endpoint is Bluetooth).
5. **The companion writes 20 bytes at a time, always,** rather than asking
   the link for its MTU: every link carries 20, a full update is eight
   writes, and the dongle's ring takes a write whole or not at all.
6. **The companion asks Windows' cache first** and the device once per run,
   so looking for the dongle every 10 s puts nothing on the air.
7. **`CryptographicBuffer` does not work from PowerShell 5.1** for
   `WriteValueAsync`; `AsBuffer` does. Found by running the companion
   against the dongle before any firmware existed, with the UUIDs pointed at
   a service it already has.
8. **CI first.** The host link was not compiled by CI, so the USB build was
   added before the refactor, to give it something to be compared with.

## Open questions for Vaibhav

1. **Old companion with the BLE transport on.** With `HOST_LINK_BLE=y`, an
   old (USB-only) companion shows dashes while the output is BLE, where
   `main` shows data. Proposed: accept that, and update the companion when
   turning the option on. The alternative is the USB-fallback rule above.
2. **USB/BT badge on the HOST screen** (brief 5.7). Proposed: not now; decide
   after the feature works on hardware.
3. **Linux BLE companion.** Proposed: later, when someone needs it.

## Plan after approval

Small commits, each building and passing tests alone:

1. Split the USB transport out of the core (pure refactor)
2. A line assembler per source, with a C unit test
3. Kconfig for the two transports
4. The GATT service
5. Only the host you are typing into
6. Windows companion over BLE
7. CI builds: USB, BLE, and both with Remote Input
8. Docs

Then the hardware checklist from the brief, on a `test/` branch of the config
repo, before any merge.

## Baseline (main)

From CI on this branch at `c53d780`, which changes no firmware. Flash and RAM
are the linker's own figures for the two memory regions.

| Build | text | data | bss | Flash used | RAM used |
| --- | --- | --- | --- | --- | --- |
| `nexus_dongle` | 334,856 | 32,298 | 87,502 | 367,168 B (45.27%) | 93,194 B (35.55%) |
| `nexus_dongle_no_studio` | 326,880 | 30,324 | 80,820 | 357,212 B (44.05%) | 85,946 B (32.79%) |
| `nexus_dongle_remote` | 347,328 | 34,875 | 92,621 | 382,208 B (47.13%) | 99,302 B (37.88%) |
| `settings_reset` | 49,008 | 3,485 | 12,150 | 52,508 B (6.47%) | 12,840 B (4.90%) |

None of these builds turns the host link on, so each must come out
byte-identical at the end of this work.
