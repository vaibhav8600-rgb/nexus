// Trackpad gesture recogniser. A pure state machine: pointer events in,
// mouse intents out. No DOM and no timers, so tests feed it synthetic touch
// sequences with made-up timestamps.

export interface PointerIn {
  type: 'down' | 'move' | 'up' | 'cancel';
  id: number;
  x: number;
  y: number;
  t: number; // ms
}

export type Intent =
  | { kind: 'move'; dx: number; dy: number }
  | { kind: 'scroll'; dx: number; dy: number }
  | { kind: 'click'; button: number } // Button.* bit
  | { kind: 'drag'; down: boolean };

export interface GestureOptions {
  tapToClick: boolean;
  longPressRight: boolean;
}

const SLOP = 8; // px a tap may wander and still be a tap
const TAP_MS = 250;
const MULTI_TAP_MS = 400; // two and three fingers land and lift less crisply
const DOUBLE_MS = 300; // tap, then touch again within this: drag
const DOUBLE_PX = 40;
const LONG_MS = 550;

export class Gestures {
  private pts = new Map<number, { x: number; y: number; sx: number; sy: number }>();
  private start = 0;
  private fingers = 0; // most fingers down at once this touch
  private moved = false;
  private dragArmed = false;
  private dragging = false;
  private lastTap = { t: -Infinity, x: 0, y: 0 };

  constructor(public opts: GestureOptions) {}

  handle(e: PointerIn): Intent[] {
    if (e.type === 'down') {
      if (this.pts.size === 0) {
        this.start = e.t;
        this.fingers = 0;
        this.moved = false;
        this.dragArmed =
          e.t - this.lastTap.t < DOUBLE_MS &&
          Math.hypot(e.x - this.lastTap.x, e.y - this.lastTap.y) < DOUBLE_PX;
      }
      this.pts.set(e.id, { x: e.x, y: e.y, sx: e.x, sy: e.y });
      this.fingers = Math.max(this.fingers, this.pts.size);
      return [];
    }

    const p = this.pts.get(e.id);
    if (!p) return [];

    if (e.type === 'move') {
      const dx = e.x - p.x;
      const dy = e.y - p.y;
      p.x = e.x;
      p.y = e.y;
      if (!this.moved && Math.hypot(e.x - p.sx, e.y - p.sy) > SLOP) this.moved = true;
      if (!this.moved) return [];

      // One finger that has only ever been one finger moves the pointer. A
      // finger left behind when a scroll ends does nothing, or the pointer
      // would jump as the second one lifts.
      if (this.pts.size === 1 && this.fingers === 1) {
        const out: Intent[] = [];
        if (this.dragArmed && !this.dragging) {
          this.dragging = true;
          out.push({ kind: 'drag', down: true });
        }
        out.push({ kind: 'move', dx, dy });
        return out;
      }
      // Each finger reports its own move; half of each is the average.
      if (this.pts.size === 2) return [{ kind: 'scroll', dx: dx / 2, dy: dy / 2 }];
      return [];
    }

    // up or cancel
    this.pts.delete(e.id);
    if (this.pts.size > 0) return [];

    if (this.dragging) {
      this.dragging = false;
      this.lastTap.t = -Infinity;
      return [{ kind: 'drag', down: false }];
    }
    if (e.type === 'cancel' || this.moved) return [];

    const held = e.t - this.start;
    if (this.fingers === 1) {
      if (this.opts.longPressRight && held >= LONG_MS) return [{ kind: 'click', button: 2 }];
      if (this.opts.tapToClick && held < TAP_MS) {
        this.lastTap = { t: e.t, x: e.x, y: e.y };
        return [{ kind: 'click', button: 1 }];
      }
      return [];
    }
    if (held < MULTI_TAP_MS) return [{ kind: 'click', button: this.fingers === 2 ? 2 : 4 }];
    return [];
  }
}
