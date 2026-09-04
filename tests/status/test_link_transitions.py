#!/usr/bin/env python3
"""Half connect/disconnect chirp logic - see src/status/status.c set_link().

The rules that matter and are easy to get wrong:

  * a chirp fires only on a transition across CONNECTED, not on every report
  * the first transition per slot is silent (boot is not news, and it would
    collide with the splash fanfare)
  * left and right get different cues, after CONFIG_NEXUS_SPLIT_SWAP_SIDES
  * one slot-to-side mapping, so the chirp and the battery card agree
  * a disconnect must not blank the battery - a half that just left still had
    a charge a moment ago, and "--" is for a level never known (Section 26)

Both directions now come from Zephyr connection callbacks (split_conn.c), not
from a silence timeout. The timeout version announced a disconnect every time
a sleeping half stopped reporting, which is what the random beeping was.

Run: python tests/status/test_link_transitions.py
"""

import os
import re
import sys

DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING = range(4)
UNKNOWN = 0xFF


class Model:
    """Mirrors set_link() / nexus_status_half_link() in src/status/status.c."""

    def __init__(self, swap_sides=False, sound_split=True):
        self.link = {True: DISCONNECTED, False: DISCONNECTED}  # keyed by is_left
        self.batt = {True: UNKNOWN, False: UNKNOWN}
        self.announced = [False, False]
        self.swap = swap_sides
        self.sound = sound_split
        self.chirps = []

    def slot_is_left(self, slot):
        return (slot == 0) != self.swap

    def set_link(self, slot, to):
        left = self.slot_is_left(slot)
        if self.link[left] == to:
            return
        was_up = self.link[left] == CONNECTED
        now_up = to == CONNECTED
        self.link[left] = to
        if self.sound and was_up != now_up:
            if self.announced[slot]:
                side = "L" if left else "R"
                self.chirps.append("%s_%s" % (side, "CONNECT" if now_up else "DISCONNECT"))
            self.announced[slot] = True

    # the three things that call it
    def ble_connected(self, slot):
        self.set_link(slot, CONNECTED)

    def ble_disconnected(self, slot):
        self.set_link(slot, DISCONNECTED)

    def battery_report(self, slot, level=80):
        self.batt[self.slot_is_left(slot)] = level
        self.set_link(slot, CONNECTED)


def check(name, got, want):
    if got != want:
        print("  FAIL  %s\n        got  %r\n        want %r" % (name, got, want))
        return 1
    print("  ok    %s" % name)
    return 0


def main():
    bad = 0

    # boot: both halves link up, nothing audible
    m = Model()
    m.ble_connected(0)
    m.ble_connected(1)
    bad += check("boot is silent", m.chirps, [])

    # battery reports on a live link are not transitions
    for _ in range(5):
        m.battery_report(0)
        m.battery_report(1)
    bad += check("battery reports on a live link are silent", m.chirps, [])

    # a half really goes away
    m.ble_disconnected(0)
    bad += check("left drop chirps once", m.chirps, ["L_DISCONNECT"])
    m.ble_disconnected(0)
    bad += check("a repeated disconnect does not re-chirp", m.chirps, ["L_DISCONNECT"])

    # ...and the battery it last reported survives it
    bad += check("drop keeps the last charge", m.batt[True], 80)

    m.ble_connected(0)
    bad += check("left return chirps", m.chirps, ["L_DISCONNECT", "L_CONNECT"])

    # right is independent and audibly different
    m.ble_disconnected(1)
    bad += check("right drop is its own cue", m.chirps[-1], "R_DISCONNECT")

    # a sleeping half that merely stops reporting must NOT chirp: nothing
    # calls set_link at all, which is the whole point of the rewrite
    m = Model()
    m.ble_connected(0)
    m.battery_report(0)
    before = list(m.chirps)
    for _ in range(10):
        pass  # ten battery intervals of silence, no events raised
    bad += check("silence alone never chirps", m.chirps, before)

    # swap_sides flips both the cue and the battery card together
    m = Model(swap_sides=True)
    m.battery_report(0, 55)
    m.ble_disconnected(0)
    bad += check("swap: slot 0 chirps as right", m.chirps, ["R_DISCONNECT"])
    bad += check("swap: slot 0 charges the right card", m.batt[False], 55)
    bad += check("swap: left card untouched", m.batt[True], UNKNOWN)

    # sound off: state still tracks
    m = Model(sound_split=False)
    m.ble_connected(0)
    m.ble_disconnected(0)
    bad += check("sound off: no chirps", m.chirps, [])
    bad += check("sound off: link still tracked", m.link[True], DISCONNECTED)

    # --- the C has to still match the model -------------------------------
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    src = open(os.path.join(root, "src", "status", "status.c"), encoding="utf-8").read()
    body = src.split("static uint32_t set_link")[1].split("\n}")[0]

    for frag, why in [
        ("if (*link == to) {", "no-op guard on an unchanged state"),
        ("was_up != now_up", "chirps only across the CONNECTED boundary"),
        ("if (g_announced[slot])", "first transition per slot is silent"),
        ("slot_is_left(slot)", "side comes from the one shared mapping"),
    ]:
        if frag not in body:
            print("  FAIL  set_link lost: %s" % why)
            bad += 1
        else:
            print("  ok    set_link keeps: %s" % why)

    half = src.split("void nexus_status_half_link")[1].split("\n}\n")[0]
    if "NEXUS_BATTERY_UNKNOWN" in half:
        print("  FAIL  half_link blanks the battery on a drop")
        bad += 1
    else:
        print("  ok    half_link leaves the battery alone")

    conn = open(os.path.join(root, "src", "status", "split_conn.c"),
                encoding="utf-8").read()
    for frag, why in [
        ("BT_CONN_CB_DEFINE", "registers Zephyr connection callbacks"),
        ("BT_CONN_ROLE_CENTRAL", "only counts links where we are the central"),
        ("bt_addr_le_cmp", "maps a peer address to a stable slot"),
    ]:
        if frag not in conn:
            print("  FAIL  split_conn lost: %s" % why)
            bad += 1
        else:
            print("  ok    split_conn keeps: %s" % why)

    # the timeout that caused the random beeping must stay gone
    if "LINK_TIMEOUT" in src or "LINK_TIMEOUT" in open(
            os.path.join(root, "Kconfig"), encoding="utf-8").read():
        print("  FAIL  silence-based link timeout is back")
        bad += 1
    else:
        print("  ok    no silence-based link inference anywhere")

    print("\n%s" % ("FAILED (%d)" % bad if bad else "PASSED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
