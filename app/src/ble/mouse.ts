// Turns trackpad intents into at most one Mouse packet per display frame:
// movement summed with its sub-pixel remainder carried over, nothing at all
// when idle, and a repeat every 200 ms while a button is held so the dongle's
// one-second watchdog never lets go of a drag.
import type { NexusLink } from './link';

const HEARTBEAT_MS = 200;
const MAX = 32767;

export class MouseSender {
  private buttons = 0;
  private sent = 0;
  private release = 0;
  private x = 0;
  private y = 0;
  private wheel = 0;
  private hwheel = 0;
  private frame = 0;
  private last = 0;
  private beat?: ReturnType<typeof setInterval>;

  constructor(private link: NexusLink) {}

  move(dx: number, dy: number) {
    this.x += dx;
    this.y += dy;
    this.kick();
  }

  /** In wheel notches; fractions carry over. */
  scroll(notchesX: number, notchesY: number) {
    this.hwheel += notchesX;
    this.wheel += notchesY;
    this.kick();
  }

  set(bit: number, down: boolean) {
    this.buttons = down ? this.buttons | bit : this.buttons & ~bit;
    this.kick();
    if (this.buttons && !this.beat) {
      this.beat = setInterval(() => this.flush(true), HEARTBEAT_MS);
    } else if (!this.buttons && this.beat) {
      clearInterval(this.beat);
      this.beat = undefined;
    }
  }

  get held(): number {
    return this.buttons;
  }

  /** Down this frame, up the next: two packets, so it cannot merge away. */
  click(bit: number) {
    this.set(bit, true);
    this.release |= bit;
  }

  private kick() {
    if (!this.frame) {
      this.frame = requestAnimationFrame(() => {
        this.frame = 0;
        this.flush(false);
      });
    }
  }

  private take(v: number): [number, number] {
    const whole = Math.max(-MAX, Math.min(MAX, Math.trunc(v)));
    return [whole, v - whole];
  }

  private flush(heartbeat: boolean) {
    let dx, dy, w, h;
    [dx, this.x] = this.take(this.x);
    [dy, this.y] = this.take(this.y);
    [w, this.wheel] = this.take(this.wheel);
    [h, this.hwheel] = this.take(this.hwheel);

    const now = performance.now();
    const due = heartbeat && this.buttons && now - this.last >= HEARTBEAT_MS - 20;
    if (dx || dy || w || h || this.buttons !== this.sent || due) {
      this.link.sendMouse(this.buttons, dx, dy, Math.max(-127, Math.min(127, w)), Math.max(-127, Math.min(127, h)));
      this.sent = this.buttons;
      this.last = now;
    }
    if (this.release) {
      const bits = this.release;
      this.release = 0;
      this.set(bits, false);
    }
  }

  /** Everything up, now. For leaving the screen or the app. */
  releaseAll() {
    this.release = 0;
    if (this.buttons) this.set(this.buttons, false);
  }
}
