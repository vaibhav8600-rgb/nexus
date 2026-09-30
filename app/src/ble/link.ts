// The Bluetooth side of the app: one NEXUS, its five characteristics, and
// the write scheduling Web Bluetooth needs - it allows one GATT operation at
// a time and throws on a second.
//
// Two lanes. Mouse packets go first, merged while they wait, so the pointer
// never queues behind a paste. Everything else - keys, text, control - is
// one strict FIFO, because the dongle types keys and text from one ordered
// queue and "type this, then Enter" must leave the phone in that order too.
// A paste is one job in that FIFO, fed out as NEXUS reports room for it.
import { CHAR, Ctrl, KeyAction, PROTOCOL_VERSION, SERVICE, decodeStatus, encodeControl, encodeKey, encodeMouse, type Bytes, type Status } from '../protocol/packets';
import { chunks, sanitize } from '../protocol/text';

export type LinkState = 'idle' | 'connecting' | 'connected' | 'reconnecting';
type CharName = keyof typeof CHAR;

interface Op {
  char: CharName;
  data: Bytes;
  resolve: () => void;
  reject: (e: unknown) => void;
}

/** Text waiting its turn in the same FIFO as keys. */
interface TextJob {
  text: string;
}

interface MousePending {
  buttons: number;
  dx: number;
  dy: number;
  wheel: number;
  hwheel: number;
}

const REMEMBER = 'nexus.device';

export class NexusLink {
  state: LinkState = 'idle';
  status: Status | null = null;
  name = '';
  error = '';
  /** Text sent and not yet typed, for the progress bar. */
  textQueued = 0;

  private device?: BluetoothDevice;
  private chars: Partial<Record<CharName, BluetoothRemoteGATTCharacteristic>> = {};
  private jobs: (Op | TextJob)[] = [];
  private mouse: MousePending[] = [];
  private pumping = false;
  private listeners = new Set<() => void>();
  private freeEstimate = 0;
  private userClosed = false;

  static supported(): boolean {
    return typeof navigator !== 'undefined' && !!navigator.bluetooth;
  }

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private emit() {
    for (const fn of this.listeners) fn();
  }

  private set(state: LinkState, error = '') {
    this.state = state;
    this.error = error;
    this.emit();
  }

  // ---- connecting --------------------------------------------------------

  /** Show the browser's device picker. Needs a user gesture. */
  async pick(): Promise<void> {
    try {
      const device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] });
      this.adopt(device);
      await this.open();
    } catch (e) {
      // Closing the picker is not an error worth showing.
      if ((e as DOMException).name !== 'NotFoundError') this.set('idle', message(e));
    }
  }

  /** Reconnect to the last NEXUS without the picker, where the browser allows it. */
  async resume(): Promise<void> {
    if (this.state !== 'idle' || !navigator.bluetooth.getDevices) return;
    const id = localStorage.getItem(REMEMBER);
    const device = (await navigator.bluetooth.getDevices().catch(() => [])).find((d) => d.id === id);
    if (!device) return;
    this.adopt(device);
    await this.open().catch(() => undefined);
  }

  private adopt(device: BluetoothDevice) {
    if (this.device !== device) {
      this.device?.removeEventListener('gattserverdisconnected', this.onDrop);
      device.addEventListener('gattserverdisconnected', this.onDrop);
    }
    this.device = device;
    this.name = device.name ?? 'NEXUS Remote';
    this.userClosed = false;
    try {
      localStorage.setItem(REMEMBER, device.id);
    } catch {
      /* private mode: reconnecting just needs a tap */
    }
  }

  private async open(): Promise<void> {
    const device = this.device;
    if (!device?.gatt) return;
    this.set(this.state === 'reconnecting' ? 'reconnecting' : 'connecting');
    try {
      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(SERVICE);
      for (const name of Object.keys(CHAR) as CharName[]) {
        this.chars[name] = await service.getCharacteristic(CHAR[name]);
      }
      const status = this.chars.status!;
      // The first encrypted read is what makes the phone pair.
      this.onStatus(await status.readValue());
      status.addEventListener('characteristicvaluechanged', this.onNotify);
      await status.startNotifications();
      if (this.status && this.status.version !== PROTOCOL_VERSION) {
        this.set('idle', `This NEXUS speaks protocol ${this.status.version}; the app speaks ${PROTOCOL_VERSION}. Update one of them.`);
        device.gatt.disconnect();
        return;
      }
      this.set('connected');
      this.pump();
    } catch (e) {
      this.chars = {};
      if (this.state === 'reconnecting') throw e;
      this.set('idle', message(e));
    }
  }

  private onNotify = (e: Event) => {
    this.onStatus((e.target as BluetoothRemoteGATTCharacteristic).value!);
  };

  private onStatus(v: DataView) {
    this.status = decodeStatus(v);
    this.freeEstimate = this.status.textFree;
    this.pump();
    this.emit();
  }

  private onDrop = () => {
    this.chars = {};
    this.mouse = [];
    // Text does not survive a drop - the dongle dropped its half of it too.
    for (const j of this.jobs.splice(0)) if (!('text' in j)) j.reject(new Error('disconnected'));
    this.textQueued = 0;
    if (this.userClosed) {
      this.set('idle');
      return;
    }
    this.set('reconnecting');
    void this.retry();
  };

  /** Back off to every few seconds; the dongle has already let go of everything. */
  private async retry() {
    for (let i = 0; this.state === 'reconnecting' && !this.userClosed; i++) {
      if (document.visibilityState === 'visible') {
        try {
          await this.open();
          return;
        } catch {
          /* try again */
        }
      }
      await sleep(Math.min(1000 * (i + 1), 5000));
    }
  }

  /** Called when the app comes back to the foreground. */
  wake() {
    if (this.state === 'idle') void this.resume();
  }

  disconnect() {
    this.userClosed = true;
    this.cancelText();
    this.device?.gatt?.disconnect();
    this.set('idle');
  }

  async forget() {
    const device = this.device;
    this.disconnect();
    this.device = undefined;
    try {
      localStorage.removeItem(REMEMBER);
    } catch {
      /* nothing to remove */
    }
    await device?.forget?.();
  }

  // ---- writing -----------------------------------------------------------

  private queue(char: CharName, data: Bytes): Promise<void> {
    return new Promise((resolve, reject) => {
      this.jobs.push({ char, data, resolve, reject });
      this.pump();
    });
  }

  private async pump() {
    if (this.pumping) return;
    this.pumping = true;
    try {
      while (this.state === 'connected') {
        const m = this.mouse.shift();
        if (m) {
          await this.chars.mouse!.writeValueWithoutResponse(encodeMouse(m.buttons, m.dx, m.dy, m.wheel, m.hwheel)).catch(() => undefined);
          continue;
        }
        const job = this.jobs[0];
        if (!job) {
          this.settleText();
          break;
        }
        if ('text' in job) {
          if (!(await this.feedText(job))) break;
          continue;
        }
        // A key is a 6-byte record in the same queue as text: wait for room
        // rather than have it refused mid-paste.
        if (job.char === 'key' && this.freeEstimate < 6) {
          this.recheckSoon();
          break;
        }
        this.jobs.shift();
        try {
          await this.chars[job.char]!.writeValueWithResponse(job.data);
          if (job.char === 'key') this.freeEstimate -= 6;
          job.resolve();
        } catch (e) {
          job.reject(e);
        }
      }
    } finally {
      this.pumping = false;
    }
  }

  /** Mouse state for this frame. Merged with anything still waiting that
   *  has the same buttons; a button change always gets its own packet. */
  sendMouse(buttons: number, dx: number, dy: number, wheel = 0, hwheel = 0) {
    const last = this.mouse[this.mouse.length - 1];
    if (last && last.buttons === buttons) {
      last.dx += dx;
      last.dy += dy;
      last.wheel += wheel;
      last.hwheel += hwheel;
    } else {
      this.mouse.push({ buttons, dx, dy, wheel, hwheel });
    }
    this.pump();
  }

  key(action: number, mods: number, page: number, usage: number): Promise<void> {
    return this.queue('key', encodeKey(action, mods, page, usage));
  }

  tap(mods: number, page: number, usage: number): Promise<void> {
    return this.key(KeyAction.tap, mods, page, usage);
  }

  control(op: number, arg?: number): Promise<void> {
    return this.queue('control', encodeControl(op, arg));
  }

  // ---- text, paced by the dongle's free queue space ----------------------

  /** Queue text to type, in order with keys; handed over as NEXUS makes room. */
  typeText(raw: string) {
    const clean = sanitize(raw);
    if (!clean) return;
    const last = this.jobs[this.jobs.length - 1];
    if (last && 'text' in last) last.text += clean;
    else this.jobs.push({ text: clean });
    this.textQueued += clean.length;
    this.emit();
    this.pump();
  }

  cancelText() {
    this.jobs = this.jobs.filter((j) => !('text' in j));
    this.textQueued = 0;
    this.emit();
    if (this.state === 'connected') void this.control(Ctrl.cancelText).catch(() => undefined);
  }

  get textPending(): number {
    return this.jobs.reduce((n, j) => n + ('text' in j ? j.text.length : 0), 0);
  }

  /** The progress bar is done once NEXUS has typed the last of it. */
  private settleText() {
    if (this.textQueued && !this.status?.typing) {
      this.textQueued = 0;
      this.emit();
    }
  }

  /**
   * Hand NEXUS one chunk of @p job if it has room. False when it has none -
   * never waits inside the pump, or the mouse would stall behind a paste. A
   * status notification or the timer below restarts it.
   */
  private async feedText(job: TextJob): Promise<boolean> {
    const room = Math.min(20, this.freeEstimate);
    if (room <= 0) {
      this.recheckSoon();
      return false;
    }
    const [chunk] = chunks(job.text.slice(0, room), room);
    try {
      await this.chars.text!.writeValueWithResponse(chunk);
      // Slice the front off: text appended during the write stays queued.
      job.text = job.text.slice(chunk.length);
      if (!job.text) this.jobs.shift();
      this.freeEstimate -= chunk.length;
      this.emit();
      return true;
    } catch {
      // Full after all, or remote switched off: wait for the real figure.
      this.freeEstimate = 0;
      this.recheckSoon();
      return false;
    }
  }

  private recheck?: ReturnType<typeof setTimeout>;

  /** In case a notification is missed: trust the last status in 300 ms. */
  private recheckSoon() {
    clearTimeout(this.recheck);
    this.recheck = setTimeout(() => {
      if (this.status) this.freeEstimate = this.status.textFree;
      this.pump();
    }, 300);
  }
}

function sleep(ms: number) {
  return new Promise<void>((r) => setTimeout(r, ms));
}

function message(e: unknown): string {
  const err = e as DOMException;
  switch (err?.name) {
    case 'NotAllowedError':
    case 'SecurityError':
      return 'Bluetooth permission was denied.';
    case 'NotSupportedError':
      return 'This NEXUS does not have Remote Input turned on.';
    case 'NetworkError':
      return 'Could not reach NEXUS. Is it in range and paired?';
    default:
      return err?.message || 'Connection failed.';
  }
}

export const link = new NexusLink();
