#!/usr/bin/env python3
"""BLE profile display: BT_SEL, BT_CLR, BT_CLR_ALL, and USB coexistence.

Mirrors refresh_endpoint() in src/status/zmk_events.c and the status tile in
src/ui/home.c, against what ZMK actually does:

  BT_SEL n     -> zmk_ble_prof_select(n), raises ble_active_profile_changed
                  (but early-returns, raising nothing, if already on n)
  BT_CLR       -> clear_profile_bond(active) -> set_profile_address(ANY),
                  which submits the profile-changed work; a profile that was
                  already open raises nothing, because nothing changed
  BT_CLR_ALL   -> clears every bonded profile, then selects profile 0

Run: python tests/status/test_ble_profile.py
"""

import os
import re
import sys

NONE, USB, BLE = range(3)
DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING = range(4)
PROFILE_COUNT = 5          # CONFIG_BT_MAX_PAIRED 7 - 2 split peripherals


class Zmk:
    """The parts of ZMK's ble.c this depends on."""

    def __init__(self):
        self.active = 0
        self.bonded = [False] * PROFILE_COUNT
        self.connected = [False] * PROFILE_COUNT
        self.events = 0

    def _raise(self):
        self.events += 1

    def prof_select(self, n):
        if self.active == n:
            return                      # early return, no event
        self.active = n
        self._raise()

    def clear_bonds(self):
        if self.bonded[self.active]:    # set_profile_address only on a change
            self.bonded[self.active] = False
            self.connected[self.active] = False
            self._raise()

    def clear_all_bonds(self):
        for i in range(PROFILE_COUNT):
            if self.bonded[i]:
                self.bonded[i] = False
                self.connected[i] = False
                self._raise()
        self.prof_select(0)

    # what NEXUS reads
    def index(self):
        return self.active

    def is_open(self):
        return not self.bonded[self.active]

    def is_connected(self):
        return self.connected[self.active]


class Nexus:
    """Mirrors refresh_endpoint() plus the status tile."""

    def __init__(self, zmk):
        self.z = zmk
        self.endpoint = NONE
        self.link_host = DISCONNECTED
        self.profile = 0
        self.bonded = False
        self.bt_connected = False
        self.repaints = 0

    def refresh(self, selected, usb_hid=False):
        profile = self.z.index()
        bonded = not self.z.is_open()
        bt_conn = self.z.is_connected()

        ep, host = NONE, DISCONNECTED
        if selected == USB:
            ep = USB
            host = CONNECTED if usb_hid else CONNECTING
        elif selected == BLE:
            ep = BLE
            host = CONNECTED if bt_conn else (RECONNECTING if bonded else CONNECTING)

        if (self.endpoint == ep and self.link_host == host and
                self.profile == profile and self.bonded == bonded and
                self.bt_connected == bt_conn):
            return
        self.endpoint, self.link_host = ep, host
        self.profile, self.bonded, self.bt_connected = profile, bonded, bt_conn
        self.repaints += 1

    def shown_number(self):
        return self.profile + 1

    def tile(self):
        if not self.bonded:
            return "OPEN"
        return "OK" if self.bt_connected else "DOWN"


def check(name, got, want):
    if got != want:
        print("  FAIL  %s\n        got  %r\n        want %r" % (name, got, want))
        return 1
    print("  ok    %s  (%r)" % (name, got))
    return 0


def main():
    bad = 0

    # --- BT_SEL across every profile -------------------------------------
    z = Zmk()
    n = Nexus(z)
    z.bonded[0] = z.connected[0] = True
    n.refresh(BLE)
    bad += check("profile 1 connected shows OK", (n.shown_number(), n.tile()), (1, "OK"))

    for p in range(1, PROFILE_COUNT):
        before = n.repaints
        z.prof_select(p)
        n.refresh(BLE)
        if n.repaints == before:
            print("  FAIL  BT_SEL %d did not repaint" % p)
            bad += 1
    bad += check("BT_SEL walks 1..%d" % PROFILE_COUNT,
                 (n.shown_number(), n.tile()), (PROFILE_COUNT, "OPEN"))

    # selecting the profile you are already on raises nothing, and that is
    # correct - there is nothing to redraw
    before = n.repaints
    z.prof_select(PROFILE_COUNT - 1)
    n.refresh(BLE)
    bad += check("re-selecting the same profile does not repaint",
                 n.repaints, before)

    # --- BT_CLR ----------------------------------------------------------
    z.prof_select(0)
    n.refresh(BLE)
    bad += check("back on profile 1, still bonded", n.tile(), "OK")
    z.clear_bonds()
    n.refresh(BLE)
    bad += check("BT_CLR opens the active profile", n.tile(), "OPEN")
    bad += check("BT_CLR leaves the number alone", n.shown_number(), 1)

    before = n.repaints
    z.clear_bonds()
    n.refresh(BLE)
    bad += check("BT_CLR on an already-open profile is a no-op",
                 n.repaints, before)

    # --- BT_CLR_ALL ------------------------------------------------------
    z = Zmk()
    n = Nexus(z)
    for i in range(PROFILE_COUNT):
        z.bonded[i] = True
    z.connected[2] = True
    z.prof_select(2)
    n.refresh(BLE)
    bad += check("profile 3 connected before clear-all",
                 (n.shown_number(), n.tile()), (3, "OK"))

    z.clear_all_bonds()
    n.refresh(BLE)
    bad += check("BT_CLR_ALL clears every bond and lands on profile 1",
                 (n.shown_number(), n.tile()), (1, "OPEN"))
    bad += check("BT_CLR_ALL left no profile bonded", any(z.bonded), False)

    # --- the bug this test exists for: USB selected, BLE state live -------
    z = Zmk()
    n = Nexus(z)
    z.bonded[0] = z.connected[0] = True
    n.refresh(USB, usb_hid=True)
    bad += check("on USB, a healthy BLE profile still reads OK", n.tile(), "OK")
    bad += check("on USB, the profile number is still right", n.shown_number(), 1)

    before = n.repaints
    z.prof_select(3)
    n.refresh(USB, usb_hid=True)
    bad += check("BT_SEL while on USB repaints", n.repaints > before, True)
    bad += check("...and shows the new profile", n.shown_number(), 4)
    bad += check("...whose tile is its own state, not the endpoint's",
                 n.tile(), "OPEN")

    # --- the C must still match ------------------------------------------
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    ze = open(os.path.join(root, "src", "status", "zmk_events.c"),
              encoding="utf-8").read()
    body = ze.split("static void refresh_endpoint")[1].split("\n}\n")[0]

    sw = body.index("switch (selected.transport)")
    for call in ("zmk_ble_active_profile_index()",
                 "zmk_ble_active_profile_is_open()",
                 "zmk_ble_active_profile_is_connected()"):
        at = body.find(call)
        if at == -1:
            print("  FAIL  refresh_endpoint no longer calls %s" % call)
            bad += 1
        elif at > sw:
            print("  FAIL  %s is inside the endpoint switch again" % call)
            bad += 1
        else:
            print("  ok    %s read before the endpoint switch" % call)

    for field in ("st->bt_profile == profile",
                  "st->bt_profile_bonded == bonded",
                  "st->bt_connected == bt_conn"):
        if field not in body:
            print("  FAIL  change check dropped: %s" % field)
            bad += 1
        else:
            print("  ok    change check includes %s" % field.split()[0][4:])

    home = open(os.path.join(root, "src", "ui", "home.c"), encoding="utf-8").read()
    tile = home[home.index("const uint16_t *tile;"):home.index("gfx_glyph(num_x")]
    if "on_usb" in tile:
        print("  FAIL  the status tile still depends on the selected endpoint")
        bad += 1
    else:
        print("  ok    status tile reports the profile, not the endpoint")

    print("\n%s" % ("FAILED (%d)" % bad if bad else "PASSED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
