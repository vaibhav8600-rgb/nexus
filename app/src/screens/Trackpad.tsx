import { useEffect, useMemo, useRef, useState, type PointerEvent } from 'react';
import type { Ctx } from '../App';
import { link } from '../ble/link';
import { Gestures, type Intent } from '../protocol/gestures';
import { KEY } from '../protocol/hid';
import { Button, KeyAction, Page } from '../protocol/packets';
import { haptic, useSettings } from '../settings';
import { ModRow } from './ModRow';

const QUICK_KEYS: [string, number][] = [
  ['Esc', KEY.esc], ['Tab', KEY.tab], ['⏎', KEY.enter], ['⌫', KEY.backspace],
  ['←', KEY.left], ['↑', KEY.up], ['↓', KEY.down], ['→', KEY.right],
];

export function Trackpad({ ctx }: { ctx: Ctx }) {
  const s = useSettings();
  const gestures = useMemo(() => new Gestures({ tapToClick: true, longPressRight: false }), []);
  gestures.opts = { tapToClick: s.tapToClick, longPressRight: s.longPressRight };
  const lastMove = useRef(0);
  const [dragLock, setDragLock] = useState(false);

  // Nothing stays held when the trackpad goes away.
  useEffect(() => () => ctx.mouse.releaseAll(), [ctx.mouse]);

  const click = async (bit: number) => {
    haptic();
    const mods = ctx.takeMods();
    // Ctrl-click and friends: the modifiers go down first, and only once the
    // dongle has them does the click go - the mouse lane would overtake.
    if (mods) await link.key(KeyAction.press, mods, Page.keyboard, 0).catch(() => undefined);
    ctx.mouse.click(bit);
    if (mods) setTimeout(() => void link.key(KeyAction.release, mods, Page.keyboard, 0).catch(() => undefined), 80);
  };

  const apply = (out: Intent[], t: number) => {
    for (const o of out) {
      if (o.kind === 'move') {
        const dt = Math.max(1, t - lastMove.current);
        lastMove.current = t;
        // Speed in px/ms: slow strokes stay precise, fast ones cross the screen.
        const v = Math.hypot(o.dx, o.dy) / dt;
        const gain = s.speed * (1 + s.accel * Math.min(Math.max(v - 0.25, 0), 3));
        ctx.mouse.move(o.dx * gain, o.dy * gain);
      } else if (o.kind === 'scroll') {
        const k = s.scroll / 24;
        const sign = s.natural ? 1 : -1;
        // HID wheel up is positive. Natural scrolling moves the content with
        // the fingers, so fingers going up scroll the view down.
        ctx.mouse.scroll(-sign * o.dx * k, sign * o.dy * k);
      } else if (o.kind === 'click') {
        void click(o.button);
      } else {
        haptic();
        ctx.mouse.set(Button.left, o.down);
      }
    }
  };

  const on = (type: 'down' | 'move' | 'up' | 'cancel') => (e: PointerEvent<HTMLDivElement>) => {
    e.preventDefault();
    if (type === 'down') {
      e.currentTarget.setPointerCapture(e.pointerId);
      lastMove.current = e.timeStamp;
    }
    apply(gestures.handle({ type, id: e.pointerId, x: e.clientX, y: e.clientY, t: e.timeStamp }), e.timeStamp);
  };

  const hold = (bit: number) => ({
    onPointerDown: (e: PointerEvent) => {
      e.preventDefault();
      haptic();
      ctx.mouse.set(bit, true);
    },
    onPointerUp: () => ctx.mouse.set(bit, false),
    onPointerCancel: () => ctx.mouse.set(bit, false),
    onPointerLeave: () => {
      if (ctx.mouse.held & bit && !(dragLock && bit === Button.left)) ctx.mouse.set(bit, false);
    },
  });

  const toggleLock = () => {
    haptic();
    setDragLock(!dragLock);
    ctx.mouse.set(Button.left, !dragLock);
  };

  return (
    <div className="pad-screen">
      <div
        className="pad"
        onPointerDown={on('down')}
        onPointerMove={on('move')}
        onPointerUp={on('up')}
        onPointerCancel={on('cancel')}
        onContextMenu={(e) => e.preventDefault()}
      >
        <span className="pad-hint">{link.state === 'connected' ? '' : 'Connect to NEXUS to start'}</span>
      </div>
      <div className="mouse-buttons">
        <button {...hold(Button.left)}>L</button>
        <button {...hold(Button.middle)}>M</button>
        <button {...hold(Button.right)}>R</button>
        <button className={dragLock ? 'on' : ''} onClick={toggleLock} title="Hold the left button">
          🔒
        </button>
      </div>
      <ModRow ctx={ctx} />
      <div className="strip">
        {QUICK_KEYS.map(([label, usage]) => (
          <button key={label} onClick={() => ctx.tapKey(Page.keyboard, usage)}>
            {label}
          </button>
        ))}
      </div>
    </div>
  );
}
