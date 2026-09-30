// Remote Input protocol v1 - docs/remote-input-protocol.md. Pure encoders
// and decoders; no Bluetooth in here, so every byte is unit-testable.

const uuid = (n: number) => `7e4e000${n}-5c1a-4b2e-9d3f-8a6b4c2d1e0f`;

export const SERVICE = uuid(1);
export const CHAR = {
  mouse: uuid(2),
  key: uuid(3),
  text: uuid(4),
  control: uuid(5),
  status: uuid(6),
} as const;

export const PROTOCOL_VERSION = 1;

/** What Web Bluetooth writes: a byte array over a plain ArrayBuffer. */
export type Bytes = Uint8Array<ArrayBuffer>;

export const Button = { left: 1, right: 2, middle: 4, back: 8, forward: 16 } as const;

export const Mod = {
  ctrl: 0x01,
  shift: 0x02,
  alt: 0x04,
  gui: 0x08,
} as const;

export const Page = { keyboard: 0x07, consumer: 0x0c } as const;
export const KeyAction = { release: 0, press: 1, tap: 2 } as const;
export const Ctrl = {
  releaseAll: 0x01,
  keepalive: 0x02,
  cancelText: 0x03,
  typeDelay: 0x04,
  identify: 0x05,
} as const;

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.trunc(v)));

export function encodeMouse(buttons: number, dx: number, dy: number, wheel = 0, hwheel = 0): Bytes {
  const b = new Uint8Array(8);
  const v = new DataView(b.buffer);
  v.setUint8(0, buttons & 0x1f);
  v.setInt16(1, clamp(dx, -32768, 32767), true);
  v.setInt16(3, clamp(dy, -32768, 32767), true);
  v.setInt8(5, clamp(wheel, -128, 127));
  v.setInt8(6, clamp(hwheel, -128, 127));
  return b;
}

export function encodeKey(action: number, mods: number, page: number, usage: number): Bytes {
  const b = new Uint8Array(6);
  const v = new DataView(b.buffer);
  v.setUint8(0, action);
  v.setUint8(1, mods);
  v.setUint16(2, page, true);
  v.setUint16(4, usage, true);
  return b;
}

export function encodeControl(op: number, arg?: number): Bytes {
  return arg === undefined ? Uint8Array.of(op) : Uint8Array.of(op, arg);
}

export interface Status {
  version: number;
  remoteOn: boolean;
  usb: boolean;
  typing: boolean;
  pairing: boolean;
  numLock: boolean;
  capsLock: boolean;
  scrollLock: boolean;
  textFree: number;
  features: { text: boolean; consumer: boolean; hwheel: boolean };
}

export function decodeStatus(data: DataView): Status {
  const state = data.getUint8(1);
  const leds = data.getUint8(2);
  const feat = data.getUint8(5);
  return {
    version: data.getUint8(0),
    remoteOn: !!(state & 1),
    usb: !!(state & 2),
    typing: !!(state & 4),
    pairing: !!(state & 8),
    numLock: !!(leds & 1),
    capsLock: !!(leds & 2),
    scrollLock: !!(leds & 4),
    textFree: data.getUint16(3, true),
    features: { text: !!(feat & 1), consumer: !!(feat & 2), hwheel: !!(feat & 4) },
  };
}
