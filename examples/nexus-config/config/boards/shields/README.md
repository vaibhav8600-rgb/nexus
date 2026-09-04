# Your keyboard shields go here

NEXUS deliberately ships **no keyboard definition**. `nexus_dongle` is hardware
only -- display, buzzer, action button, central role -- so it composes with
whatever keyboard you already have instead of replacing it (Section 3,
Requirement H).

For an existing Sofle setup you keep your current shields and change three
things:

1. **Your dongle shield** (`sofle_dongle/`) keeps the keymap, the matrix
   transform and the physical layout exactly as they are. It already declares
   `zmk,kscan = &mock_kscan`. Nothing about it changes.

2. **Build it alongside `nexus_dongle`** -- two shields, space separated:

   ```yaml
   shield: nexus_dongle sofle_dongle
   ```

   `nexus_dongle` brings the display and peripherals; `sofle_dongle` brings the
   keys. Neither knows about the other.

3. **The halves become peripherals.** With a dongle as central, the left half
   is no longer central. If your `sofle_left` shield hard-codes
   `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`, add a `sofle_left_peripheral` variant
   that sets it to `n`. The right half already is one.

Your left/right firmware is otherwise untouched, and neither half compiles a
single line of NEXUS -- `CONFIG_NEXUS` is off unless the dongle shield turns it
on (Section 81).

## Copying the demo instead

If you just want to see the dongle boot before wiring a keyboard to it, build
the shipped `nexus_dongle_demo` shield -- it is the `nexus_standalone` entry in
`build.yaml` and needs nothing from this directory.
