import { useEffect, useMemo, useState, useSyncExternalStore } from 'react';
import { NexusLink, link } from './ble/link';
import { MouseSender } from './ble/mouse';
import { NO_MODS, consumeMods, tapMod, type ModState } from './protocol/mods';
import { Ctrl, Page } from './protocol/packets';
import { Keyboard } from './screens/Keyboard';
import { Media } from './screens/Media';
import { SettingsScreen } from './screens/Settings';
import { Trackpad } from './screens/Trackpad';
import { haptic, useSettings } from './settings';

type Tab = 'pad' | 'keys' | 'media' | 'settings';

export interface Ctx {
  mouse: MouseSender;
  mods: ModState;
  tapModifier: (bit: number) => void;
  /** Tap a key with whatever sticky modifiers are set, then clear the one-shots. */
  tapKey: (page: number, usage: number, extraMods?: number) => void;
  /** The sticky modifiers to wrap a click in, consuming the one-shots. */
  takeMods: () => number;
}

function useLink() {
  return useSyncExternalStore(
    (fn) => link.subscribe(fn),
    () => `${link.state}|${link.error}|${link.textPending}|${link.textQueued}|${JSON.stringify(link.status)}`,
  );
}

export function App() {
  useLink();
  const settings = useSettings();
  const [tab, setTab] = useState<Tab>('pad');
  const [mods, setMods] = useState<ModState>(NO_MODS);
  const mouse = useMemo(() => new MouseSender(link), []);

  // Reconnect on launch and whenever the app returns to the foreground; the
  // link drops in the background and the dongle lets go of everything.
  useEffect(() => {
    void link.resume();
    const onVisible = () => {
      if (document.visibilityState === 'visible') link.wake();
      else mouse.releaseAll();
    };
    document.addEventListener('visibilitychange', onVisible);
    return () => document.removeEventListener('visibilitychange', onVisible);
  }, [mouse]);

  // The session's typing speed, sent again on every connect.
  useEffect(() => {
    if (link.state === 'connected') void link.control(Ctrl.typeDelay, settings.typeDelay).catch(() => undefined);
  }, [link.state, settings.typeDelay]);

  // Keep the screen on while connected, where the browser can.
  useEffect(() => {
    if (!settings.keepAwake || link.state !== 'connected' || !navigator.wakeLock) return;
    let lock: WakeLockSentinel | undefined;
    const take = () => {
      if (document.visibilityState === 'visible') {
        navigator.wakeLock.request('screen').then((l) => (lock = l), () => undefined);
      }
    };
    take();
    document.addEventListener('visibilitychange', take);
    return () => {
      document.removeEventListener('visibilitychange', take);
      void lock?.release();
    };
  }, [settings.keepAwake, link.state]);

  useEffect(() => {
    document.documentElement.dataset.theme = settings.theme;
  }, [settings.theme]);

  const ctx: Ctx = {
    mouse,
    mods,
    tapModifier: (bit) => {
      haptic();
      setMods((m) => tapMod(m, bit));
    },
    tapKey: (page, usage, extraMods = 0) => {
      haptic();
      const [held, next] = consumeMods(mods);
      setMods(next);
      void link.tap(page === Page.consumer ? 0 : held | extraMods, page, usage).catch(() => undefined);
    },
    takeMods: () => {
      const [held, next] = consumeMods(mods);
      setMods(next);
      return held;
    },
  };

  return (
    <div className="app">
      <Header />
      <main>
        {tab === 'pad' && <Trackpad ctx={ctx} />}
        {tab === 'keys' && <Keyboard ctx={ctx} />}
        {tab === 'media' && <Media ctx={ctx} />}
        {tab === 'settings' && <SettingsScreen />}
      </main>
      <nav className="tabs">
        {(['pad', 'keys', 'media', 'settings'] as Tab[]).map((t) => (
          <button key={t} className={t === tab ? 'on' : ''} onClick={() => setTab(t)}>
            {{ pad: 'Trackpad', keys: 'Keyboard', media: 'Media', settings: 'Settings' }[t]}
          </button>
        ))}
      </nav>
    </div>
  );
}

function Header() {
  const s = link.status;
  const connected = link.state === 'connected';

  if (!NexusLink.supported()) {
    return (
      <header className="bar warn">
        This browser has no Bluetooth. Use Chrome on Android, or Bluefy on iPhone.
      </header>
    );
  }

  let chip = 'Not connected';
  if (link.state === 'connecting') chip = 'Connecting…';
  if (link.state === 'reconnecting') chip = 'Reconnecting…';
  if (connected) chip = s?.remoteOn ? 'Connected' : 'Remote off on NEXUS';

  return (
    <header className="bar">
      <span className={`dot ${connected && s?.remoteOn ? 'ok' : connected ? 'warn' : ''}`} />
      <span className="chip">{chip}</span>
      {connected && s?.capsLock && <span className="chip caps">CAPS</span>}
      <span className="grow" />
      {link.state === 'idle' ? (
        <button className="primary" onClick={() => void link.pick()}>
          Connect
        </button>
      ) : (
        <button onClick={() => link.disconnect()}>Disconnect</button>
      )}
      {link.error && <div className="error">{link.error}</div>}
    </header>
  );
}
