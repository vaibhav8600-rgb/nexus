import { useRef, useState, type ChangeEvent, type KeyboardEvent } from 'react';
import type { Ctx } from '../App';
import { link } from '../ble/link';
import { KEY, SPECIAL_KEYS, shortcuts } from '../protocol/hid';
import { Page } from '../protocol/packets';
import { useSettings } from '../settings';
import { ModRow } from './ModRow';

// Live mode keeps one character in the field, so a Backspace on an
// otherwise empty field still has something to delete - and still fires.
const SEED = ' ';

export function Keyboard({ ctx }: { ctx: Ctx }) {
  const { os } = useSettings();
  const [draft, setDraft] = useState('');
  const [live, setLive] = useState(SEED);
  const prev = useRef(SEED);
  const connected = link.state === 'connected';

  const send = () => {
    link.typeText(draft);
    setDraft('');
  };

  // Android keyboards with autocorrect do not send reliable key events, so
  // the field is diffed instead: what vanished becomes Backspaces, what
  // appeared is typed. Both go through the dongle's one ordered queue.
  const onLive = (e: ChangeEvent<HTMLInputElement>) => {
    const next = e.target.value;
    const old = prev.current;
    let same = 0;
    while (same < old.length && same < next.length && old[same] === next[same]) same++;
    for (let i = same; i < old.length; i++) ctx.tapKey(Page.keyboard, KEY.backspace);
    const added = next.slice(same);
    if (added) link.typeText(added);

    const keep = next.length === 0 || next.length > 48 ? SEED : next;
    prev.current = keep;
    setLive(keep);
  };

  const onLiveKey = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      ctx.tapKey(Page.keyboard, KEY.enter);
    }
  };

  const pending = link.textPending;
  const total = link.textQueued;
  const typing = pending > 0 || !!link.status?.typing;

  return (
    <div className="keys-screen">
      <section className="card">
        <textarea
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          placeholder="Type or paste, then Send - NEXUS types it out"
          rows={3}
        />
        <div className="row">
          {typing ? (
            <>
              <progress max={total || 1} value={total - pending} />
              <button onClick={() => link.cancelText()}>Cancel</button>
            </>
          ) : (
            <span className="hint">US layout; emoji and accents are skipped</span>
          )}
          <button className="primary" disabled={!connected || !draft} onClick={send}>
            Send
          </button>
        </div>
      </section>

      <section className="card">
        <label className="hint" htmlFor="live">
          Live - every key goes straight through
        </label>
        <input
          id="live"
          className="live"
          value={live}
          onChange={onLive}
          onKeyDown={onLiveKey}
          autoCapitalize="off"
          autoComplete="off"
          spellCheck={false}
          disabled={!connected}
        />
      </section>

      <ModRow ctx={ctx} />
      <div className="grid">
        {SPECIAL_KEYS.map((k) => (
          <button key={k.label} onClick={() => ctx.tapKey(k.page, k.usage)}>
            {k.label}
          </button>
        ))}
      </div>

      <h3>Shortcuts</h3>
      <div className="chips">
        {shortcuts(os).map((k) => (
          <button key={k.label} onClick={() => ctx.tapKey(k.page, k.usage, k.mods)}>
            {k.label}
          </button>
        ))}
      </div>
    </div>
  );
}
