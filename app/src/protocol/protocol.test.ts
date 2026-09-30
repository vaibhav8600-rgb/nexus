// Every example in docs/remote-input-protocol.md, byte for byte. If the doc
// and the app disagree, one of these fails.
import { describe, expect, it } from 'vitest';
import { Gestures, type Intent, type PointerIn } from './gestures';
import { letter, shortcuts } from './hid';
import { Ctrl, KeyAction, Mod, Page, decodeStatus, encodeControl, encodeKey, encodeMouse } from './packets';
import { chunks, sanitize } from './text';

const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0').toUpperCase()).join(' ');
const bytes = (s: string) => Uint8Array.from(s.split(' '), (x) => parseInt(x, 16));

describe('packets match the protocol doc', () => {
  it('mouse', () => {
    expect(hex(encodeMouse(1, 5, -3, 1, 0))).toBe('01 05 00 FD FF 01 00 00');
  });

  it('mouse clamps instead of wrapping', () => {
    expect(hex(encodeMouse(0xff, 40000, -40000, 300, -300))).toBe('1F FF 7F 00 80 7F 80 00');
  });

  it('keys', () => {
    expect(hex(encodeKey(KeyAction.tap, Mod.ctrl, Page.keyboard, letter('c')))).toBe('02 01 07 00 06 00');
    expect(hex(encodeKey(KeyAction.press, 0, Page.keyboard, letter('a')))).toBe('01 00 07 00 04 00');
    expect(hex(encodeKey(KeyAction.release, 0, Page.keyboard, letter('a')))).toBe('00 00 07 00 04 00');
    expect(hex(encodeKey(KeyAction.press, Mod.ctrl, Page.keyboard, 0))).toBe('01 01 07 00 00 00');
    expect(hex(encodeKey(KeyAction.tap, 0, Page.consumer, 0xcd))).toBe('02 00 0C 00 CD 00');
  });

  it('control', () => {
    expect(hex(encodeControl(Ctrl.releaseAll))).toBe('01');
    expect(hex(encodeControl(Ctrl.typeDelay, 10))).toBe('04 0A');
  });

  it('status', () => {
    const s = decodeStatus(new DataView(bytes('01 03 02 00 02 07').buffer));
    expect(s).toMatchObject({
      version: 1, remoteOn: true, usb: true, typing: false, pairing: false,
      capsLock: true, numLock: false, textFree: 512,
      features: { text: true, consumer: true, hwheel: true },
    });
  });
});

describe('hid tables', () => {
  it('letters and digits', () => {
    expect(letter('a')).toBe(0x04);
    expect(letter('Z')).toBe(0x1d);
    expect(letter('1')).toBe(0x1e);
    expect(letter('0')).toBe(0x27);
  });

  it('shortcuts swap Ctrl for Cmd on a Mac', () => {
    const copy = (os: 'mac' | 'windows') => shortcuts(os).find((s) => s.label === 'Copy')!;
    expect(copy('windows').mods).toBe(Mod.ctrl);
    expect(copy('mac').mods).toBe(Mod.gui);
    expect(shortcuts('windows')).toHaveLength(16);
  });
});

describe('text', () => {
  it('keeps what can be typed and fixes what phones substitute', () => {
    expect(sanitize('Hi\r\nthere')).toBe('Hi\nthere');
    expect(sanitize('it’s “fine” — ok…')).toBe('it\'s "fine" - ok...');
    expect(sanitize('café \u{1F600}!')).toBe('caf !');
    expect(sanitize('a\tb')).toBe('a\tb');
  });

  it('chunks at any MTU without losing a byte', () => {
    const text = 'x'.repeat(500);
    for (const max of [1, 20, 64, 185, 244, 1000]) {
      const parts = chunks(text, max);
      expect(parts.every((p) => p.length <= max && p.length > 0)).toBe(true);
      expect(parts.reduce((n, p) => n + p.length, 0)).toBe(500);
    }
    expect(chunks('', 20)).toEqual([]);
    expect(Array.from(chunks('Hi\n')[0])).toEqual([0x48, 0x69, 0x0a]);
  });
});

describe('gestures', () => {
  const run = (events: PointerIn[], opts = { tapToClick: true, longPressRight: false }) => {
    const g = new Gestures(opts);
    return events.flatMap((e) => g.handle(e));
  };
  const ev = (type: PointerIn['type'], id: number, x: number, y: number, t: number): PointerIn => ({ type, id, x, y, t });
  const kinds = (out: Intent[]) => out.map((o) => (o.kind === 'click' ? `click${o.button}` : o.kind === 'drag' ? `drag${o.down ? 'Down' : 'Up'}` : o.kind));

  it('one-finger tap is a left click', () => {
    expect(run([ev('down', 1, 100, 100, 0), ev('up', 1, 101, 100, 90)])).toEqual([{ kind: 'click', button: 1 }]);
  });

  it('tap-to-click off: no click', () => {
    expect(run([ev('down', 1, 100, 100, 0), ev('up', 1, 100, 100, 90)], { tapToClick: false, longPressRight: false })).toEqual([]);
  });

  it('a slow press is not a tap', () => {
    expect(run([ev('down', 1, 100, 100, 0), ev('up', 1, 100, 100, 400)])).toEqual([]);
  });

  it('long press is a right click when enabled', () => {
    expect(run([ev('down', 1, 100, 100, 0), ev('up', 1, 100, 100, 700)], { tapToClick: true, longPressRight: true }))
      .toEqual([{ kind: 'click', button: 2 }]);
  });

  it('two- and three-finger taps', () => {
    expect(run([ev('down', 1, 100, 100, 0), ev('down', 2, 150, 100, 20), ev('up', 1, 100, 100, 120), ev('up', 2, 150, 100, 130)]))
      .toEqual([{ kind: 'click', button: 2 }]);
    expect(kinds(run([
      ev('down', 1, 100, 100, 0), ev('down', 2, 150, 100, 10), ev('down', 3, 200, 100, 20),
      ev('up', 1, 100, 100, 150), ev('up', 2, 150, 100, 160), ev('up', 3, 200, 100, 170),
    ]))).toEqual(['click4']);
  });

  it('one finger moves the pointer, and the slop is not a jump', () => {
    const out = run([ev('down', 1, 100, 100, 0), ev('move', 1, 104, 100, 10), ev('move', 1, 120, 100, 20), ev('move', 1, 130, 110, 30), ev('up', 1, 130, 110, 40)]);
    expect(out).toEqual([{ kind: 'move', dx: 16, dy: 0 }, { kind: 'move', dx: 10, dy: 10 }]);
  });

  it('two fingers scroll by their average, and no click after', () => {
    const out = run([
      ev('down', 1, 100, 100, 0), ev('down', 2, 150, 100, 5),
      ev('move', 1, 100, 120, 20), ev('move', 2, 150, 120, 20),
      ev('up', 1, 100, 120, 60), ev('move', 2, 150, 160, 70), ev('up', 2, 150, 160, 80),
    ]);
    expect(out).toEqual([{ kind: 'scroll', dx: 0, dy: 10 }, { kind: 'scroll', dx: 0, dy: 10 }]);
  });

  it('tap then touch-and-move drags, releasing on lift', () => {
    const out = run([
      ev('down', 1, 100, 100, 0), ev('up', 1, 100, 100, 80),
      ev('down', 1, 102, 101, 200), ev('move', 1, 130, 101, 260), ev('up', 1, 130, 101, 300),
    ]);
    expect(kinds(out)).toEqual(['click1', 'dragDown', 'move', 'dragUp']);
  });

  it('two quick taps are two clicks: a double click', () => {
    const out = run([ev('down', 1, 100, 100, 0), ev('up', 1, 100, 100, 80), ev('down', 1, 101, 100, 200), ev('up', 1, 101, 100, 260)]);
    expect(kinds(out)).toEqual(['click1', 'click1']);
  });

  it('cancel never clicks', () => {
    expect(run([ev('down', 1, 100, 100, 0), ev('cancel', 1, 100, 100, 50)])).toEqual([]);
  });
});

describe('sticky modifiers', () => {
  it('tap once for the next key, twice to lock, three times to let go', async () => {
    const { NO_MODS, consumeMods, tapMod } = await import('./mods');
    let s = tapMod(NO_MODS, Mod.ctrl);
    let [mods, after] = consumeMods(s);
    expect(mods).toBe(Mod.ctrl);
    expect(after).toEqual(NO_MODS);

    s = tapMod(tapMod(NO_MODS, Mod.ctrl), Mod.ctrl);
    [mods, after] = consumeMods(s);
    expect(mods).toBe(Mod.ctrl);
    expect(after.locked).toBe(Mod.ctrl);

    expect(tapMod(s, Mod.ctrl)).toEqual(NO_MODS);
    expect(consumeMods(tapMod(tapMod(NO_MODS, Mod.ctrl), Mod.shift))[0]).toBe(Mod.ctrl | Mod.shift);
  });
});
