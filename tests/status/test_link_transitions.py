#!/usr/bin/env python3
"""Half connect/disconnect chirp logic - see src/status/status.c set_link().

The rules that matter and are easy to get wrong:

  * a chirp fires only on a transition across CONNECTED, not on every report
  * the first transition per slot is silent (boot is not news, and it would
    collide with the splash fanfare)
  * left and right get different cues, after CONFIG_NEXUS_SPLIT_SWAP_SIDES
  * a disconnect can only ever come from the silence timeout, because ZMK's
    split central raises no events at all

Run: python tests/status/test_link_transitions.py
"""

import os
import re
import sys

DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING = range(4)


class Model:
    """Mirrors set_link() in src/status/status.c."""

    def __init__(self, swap_sides=False, sound_split=True):
        self.link = {True: DISCONNECTED, False: DISCONNECTED}  # keyed by is_left
        self.announced = [False, False]                        # keyed by slot
        self.swap = swap_sides
        self.sound = sound_split
        self.chirps = []

    def set_link(self, slot, to):
        left = (slot == 0) != self.swap
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

    # the two things that actually call it
    def battery_report(self, slot):
        self.set_link(slot, CONNECTED)

    def silence_timeout(self, slot):
        self.set_link(slot, RECONNECTING)


def check(name, got, want):
    if got != want:
        print("  FAIL  %s\n        got  %r\n        want %r" % (name, got, want))
        return 1
    print("  ok    %s" % name)
    return 0


def main():
    bad = 0

    # boot: both halves report in, nothing audible
    m = Model()
    m.battery_report(0)
    m.battery_report(1)
    bad += check("boot is silent", m.chirps, [])

    # steady state: repeated reports are not transitions
    for _ in range(5):
        m.battery_report(0)
        m.battery_report(1)
    bad += check("repeat reports are silent", m.chirps, [])

    # left goes quiet, then comes back
    m.silence_timeout(0)
    bad += check("left drop chirps once", m.chirps, ["L_DISCONNECT"])
    m.silence_timeout(0)
    bad += check("still-quiet does not re-chirp", m.chirps, ["L_DISCONNECT"])
    m.battery_report(0)
    bad += check("left return chirps", m.chirps, ["L_DISCONNECT", "L_CONNECT"])

    # right is independent and sounds different
    m.silence_timeout(1)
    bad += check("right drop is its own cue", m.chirps[-1], "R_DISCONNECT")

    # A half that has never been up cannot "disconnect". The sweep may demote
    # it DISCONNECTED -> RECONNECTING, but that does not cross CONNECTED, so
    # it is not a transition and must not consume the boot suppression - the
    # half's real arrival afterwards is still boot, and still silent.
    m = Model()
    m.silence_timeout(0)
    bad += check("timeout on a never-connected half is silent", m.chirps, [])
    m.battery_report(0)
    bad += check("its first arrival is still boot, still silent", m.chirps, [])
    m.silence_timeout(0)
    bad += check("the next real drop does chirp", m.chirps, ["L_DISCONNECT"])

    # swap_sides really swaps which cue plays
    m = Model(swap_sides=True)
    m.battery_report(0)
    m.silence_timeout(0)
    bad += check("swap_sides flips slot 0 to the right cue", m.chirps, ["R_DISCONNECT"])

    # sound off means no chirps but state still tracks
    m = Model(sound_split=False)
    m.battery_report(0)
    m.silence_timeout(0)
    bad += check("sound off: no chirps", m.chirps, [])
    bad += check("sound off: link still tracked", m.link[True], RECONNECTING)

    # --- the C has to still match the model -------------------------------
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    src = open(os.path.join(root, "src", "status", "status.c"), encoding="utf-8").read()
    body = src.split("static uint32_t set_link")[1].split("\n}")[0]

    for frag, why in [
        ("if (*link == to) {", "no-op guard on an unchanged state"),
        ("was_up != now_up", "chirps only across the CONNECTED boundary"),
        ("if (g_announced[slot])", "first transition per slot is silent"),
        ("g_announced[slot] = true;", "the slot is marked announced"),
    ]:
        if frag not in body:
            print("  FAIL  set_link lost: %s" % why)
            bad += 1
        else:
            print("  ok    set_link keeps: %s" % why)

    # a disconnect must not blank the battery - that regression is why
    # NEXUS_STATUS_STALE_MS exists separately (Section 26)
    # the definition, not the forward declaration above it
    sweep = src.split("static void stale_work_cb(struct k_work *work)\n{")[1].split("\n}")[0]
    if "NEXUS_LINK_RECONNECTING" not in sweep:
        print("  FAIL  sweep no longer demotes the link")
        bad += 1
    elif "LINK_TIMEOUT_MS" not in sweep or "STALE_MS" not in sweep:
        print("  FAIL  sweep lost one of its two independent timeouts")
        bad += 1
    else:
        print("  ok    sweep keeps link and battery timeouts independent")

    print("\n%s" % ("FAILED (%d)" % bad if bad else "PASSED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
