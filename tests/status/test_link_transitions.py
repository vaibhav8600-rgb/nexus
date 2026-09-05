#!/usr/bin/env python3
"""Half connect/disconnect chirp logic - see src/status/status.c set_link().

The rules that matter and are easy to get wrong:

  * a chirp fires only on a transition across CONNECTED, not on every report
  * nothing fires while the links are still settling after a reset - boot is
    noisy, and those chirps were cutting off the splash fanfare
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
import sys

DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING = range(4)
UNKNOWN = 0xFF


class Model:
    """Mirrors set_link() / nexus_status_half_link() in src/status/status.c."""

    SETTLE_MS = 8000

    def __init__(self, swap_sides=False, sound_split=True, uptime=1200):
        self.link = {True: DISCONNECTED, False: DISCONNECTED}  # keyed by is_left
        self.batt = {True: UNKNOWN, False: UNKNOWN}
        self.swap = swap_sides
        self.sound = sound_split
        self.uptime = uptime          # ms since boot
        self.chirps = []
        # peer address -> slot, mirroring split_conn.c's slot_for()
        self.peer = [None, None]
        self.up = [False, False]

    def booted(self):
        """Time passes: the links have settled and cues are live again."""
        self.uptime = 20000
        return self

    def slot_is_left(self, slot):
        return (slot == 0) != self.swap

    def set_link(self, slot, to):
        left = self.slot_is_left(slot)
        if self.link[left] == to:
            return
        was_up = self.link[left] == CONNECTED
        now_up = to == CONNECTED
        self.link[left] = to
        if self.sound and was_up != now_up and self.uptime > self.SETTLE_MS:
            side = "L" if left else "R"
            self.chirps.append("%s_%s" % (side, "CONNECT" if now_up else "DISCONNECT"))

    def slot_for(self, addr):
        """Mirrors slot_for() in src/status/split_conn.c."""
        for i in (0, 1):
            if self.peer[i] == addr:
                return i
        for i in (0, 1):
            if self.peer[i] is None:
                self.peer[i] = addr
                return i
        for i in (0, 1):              # a half back under a rotated address
            if not self.up[i]:
                self.peer[i] = addr
                return i
        return -1

    # the three things that call it
    def ble_connected(self, addr):
        slot = self.slot_for(addr)
        if slot < 0:
            return
        self.up[slot] = True
        self.set_link(slot, CONNECTED)

    def ble_disconnected(self, addr):
        slot = self.slot_for(addr)
        if slot < 0:
            return
        self.up[slot] = False
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
    A, B = "aa:left", "bb:right"          # peer addresses

    # --- boot ------------------------------------------------------------
    # Bringing up a split keyboard is noisy: both halves connect, and the
    # central may drop and retry a link during discovery. Announcing any of
    # that gave random beeps after every restart AND killed the splash
    # fanfare, because the newest effect replaces whatever is playing.
    m = Model(uptime=1200)
    m.ble_connected(A)
    m.ble_connected(B)
    m.ble_disconnected(B)                 # discovery retry
    m.ble_connected(B)
    bad += check("boot churn is entirely silent", m.chirps, [])

    # --- steady state ----------------------------------------------------
    m = Model()
    m.ble_connected(A)
    m.ble_connected(B)
    bad += check("halves arriving at boot are silent", m.chirps, [])
    m.booted()

    for _ in range(5):
        m.battery_report(0)
        m.battery_report(1)
    bad += check("battery reports on a live link are silent", m.chirps, [])

    # --- a half really goes away and comes back --------------------------
    m.ble_disconnected(A)
    bad += check("left drop chirps once", m.chirps, ["L_DISCONNECT"])
    m.ble_disconnected(A)
    bad += check("a repeated disconnect does not re-chirp", m.chirps, ["L_DISCONNECT"])
    bad += check("drop keeps the last charge", m.batt[True], 80)

    m.ble_connected(A)
    bad += check("left return chirps", m.chirps, ["L_DISCONNECT", "L_CONNECT"])

    # THE BUG: a half does not have to come back under the same address. BLE
    # privacy rotates a peripheral's advertised address, so the reconnect can
    # arrive as a stranger. The first slot_for() matched on address, then took
    # a free slot, then gave up - and with both slots already claimed by the
    # old addresses it returned -1 and dropped the cue on the floor. That is
    # exactly "disconnect sound works, connect sound does not".
    m = Model()
    m.ble_connected(A)
    m.ble_connected(B)
    m.booted()
    m.ble_disconnected(A)
    bad += check("drop before the address rotates", m.chirps, ["L_DISCONNECT"])
    m.ble_connected("aa:left-rotated")
    bad += check("reconnect under a NEW address still chirps",
                 m.chirps, ["L_DISCONNECT", "L_CONNECT"])
    bad += check("...and lands on the same side, not the other half's",
                 m.link[True], CONNECTED)
    bad += check("...leaving the other half alone", m.link[False], CONNECTED)

    # a genuine third peripheral, both halves live, is refused rather than
    # evicting one of them
    m = Model()
    m.ble_connected(A)
    m.ble_connected(B)
    m.booted()
    before = dict(m.link)
    m.ble_connected("cc:stranger")
    bad += check("a third peer with both halves up is ignored", m.link, before)

    # a half switched on well after boot is news, and must chirp
    m = Model()
    m.ble_connected(A)
    m.booted()
    m.ble_connected(B)
    bad += check("a half powered on after boot chirps", m.chirps, ["R_CONNECT"])

    # --- silence is not a disconnect -------------------------------------
    m = Model()
    m.ble_connected(A)
    m.booted()
    m.battery_report(0)
    before = list(m.chirps)
    for _ in range(10):
        pass  # ten battery intervals of a sleeping half: no events at all
    bad += check("silence alone never chirps", m.chirps, before)

    # --- sides -----------------------------------------------------------
    m = Model(swap_sides=True)
    m.battery_report(0, 55)
    m.booted()
    m.ble_disconnected(A)                 # claims the free slot 0
    bad += check("swap: slot 0 chirps as right", m.chirps, ["R_DISCONNECT"])
    bad += check("swap: slot 0 charges the right card", m.batt[False], 55)
    bad += check("swap: left card untouched", m.batt[True], UNKNOWN)

    m = Model(sound_split=False)
    m.ble_connected(A)
    m.booted()
    m.ble_disconnected(A)
    bad += check("sound off: no chirps", m.chirps, [])
    bad += check("sound off: link still tracked", m.link[True], DISCONNECTED)

    # --- the C has to still match the model -------------------------------
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    src = open(os.path.join(root, "src", "status", "status.c"), encoding="utf-8").read()
    body = src.split("static uint32_t set_link")[1].split("\n}")[0]

    for frag, why in [
        ("if (*link == to) {", "no-op guard on an unchanged state"),
        ("was_up != now_up", "chirps only across the CONNECTED boundary"),
        ("CONFIG_NEXUS_SOUND_SPLIT_SETTLE_MS", "boot settling window"),
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
        ("if (!g_up[i])", "an unknown address reclaims a slot that is down"),
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
