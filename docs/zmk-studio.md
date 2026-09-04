# ZMK Studio

NEXUS uses the official ZMK Studio integration. It does not implement, wrap,
proxy or replace any part of the RPC protocol (Section 30, Requirement E).

Everything below is standard ZMK. It is documented here only because NEXUS has
to stay out of its way.

## Enabling it

Studio needs two things: the config symbol and the USB transport snippet.

`build.yaml`:

```yaml
- board: nice_nano@2.0.0//zmk
  shield: nexus_dongle sofle_dongle
  snippet: studio-rpc-usb-uart
  artifact-name: nexus_dongle
```

and:

```
CONFIG_ZMK_STUDIO=y
```

The shipped `nexus_dongle_demo` shield sets `CONFIG_ZMK_STUDIO=y` for you.

## Unlocking

Studio refuses to write a locked device. Bind ZMK's own unlock behavior
somewhere in your keymap:

```dts
bindings = <&studio_unlock>;
```

Do not invent another mechanism. NEXUS deliberately provides no unlock path of
its own -- the physical action button cannot unlock Studio and should not be
able to (Section 32).

## What NEXUS guarantees

- **Runtime keymap changes keep working.** The dashboard reads layer names
  through `zmk_keymap_layer_name()` on every layer change, so a layer renamed
  in Studio shows its new name immediately. Nothing caches a keymap.
- **No layer names are assumed.** An unnamed layer renders as `L3`, not as a
  guess (Section 28).
- **`&nexus_action` is Studio-representable.** It is a normal
  devicetree-declared behavior with one parameter, so Studio can show and
  reassign it like any other (Section 97). It is *not* a runtime-invented
  behavior, which Studio could not handle.
- **The UI does not fight for the USB link.** Studio RPC runs on ZMK's CDC-ACM
  endpoint; NEXUS never opens a USB endpoint.
- **Nothing blocks the RPC thread.** Every NEXUS work item is short and runs on
  the display queue. There is no game loop and no NEXUS thread (Section 35).

## Physical layouts

Studio needs a `zmk,physical-layout` to place keys. That comes from your
*keyboard's* shield, not from NEXUS -- `nexus_dongle` has no keys and declares
no layout, which is why it composes with any keyboard.

The `nexus_dongle_demo` shield declares a two-key layout purely so a standalone
bring-up build is Studio-testable.

## Memory

Studio costs flash and RAM, and it takes priority over anything visual
(Sections 34, 67). If a build stops fitting, cut in this order:

1. `CONFIG_NEXUS_SPLASH_MAX_DIM` down, or drop the custom splash (51 KB → 0).
2. `CONFIG_NEXUS_GAME_CENTER=n` (removes the launcher, engine and Tetris).
3. `CONFIG_NEXUS_ANIMATIONS=n`.
4. `CONFIG_NEXUS_SOUND=n`.

`CONFIG_LV_Z_VDB_SIZE` is already at its floor (10). LVGL draws nothing in this
firmware -- the UI is composited into a static 5,760-byte band -- so there is
nothing left to reclaim there, and raising it only wastes RAM.

Do not cut `CONFIG_BT_MAX_CONN`, split battery fetching, or Studio itself.
The CI build prints `arm-zephyr-eabi-size` output for every artifact -- that is
the number to watch.

## Verifying it works

Compiling is not evidence (Section 136). Actually check:

1. Plug the dongle in over USB.
2. Open ZMK Studio, connect over the serial device.
3. Press your `&studio_unlock` key.
4. Read the keymap. Change one binding. Save.
5. Confirm the change takes effect without a reflash.
6. Confirm the NEXUS dashboard is still updating -- WPM, layer, batteries.
7. Rename a layer in Studio and confirm the dashboard shows the new name.
8. Lock, disconnect, reconnect.
