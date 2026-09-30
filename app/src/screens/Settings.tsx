import { link } from '../ble/link';
import type { HostOs } from '../protocol/hid';
import { Ctrl } from '../protocol/packets';
import { updateSettings, useSettings, type Settings } from '../settings';

function Slider(props: { label: string; k: keyof Settings; min: number; max: number; step: number }) {
  const s = useSettings();
  const v = s[props.k] as number;
  return (
    <label className="setting">
      <span>
        {props.label} <b>{v}</b>
      </span>
      <input
        type="range"
        min={props.min}
        max={props.max}
        step={props.step}
        value={v}
        onChange={(e) => updateSettings({ [props.k]: Number(e.target.value) })}
      />
    </label>
  );
}

function Toggle(props: { label: string; k: keyof Settings }) {
  const s = useSettings();
  return (
    <label className="setting toggle">
      <span>{props.label}</span>
      <input type="checkbox" checked={s[props.k] as boolean} onChange={(e) => updateSettings({ [props.k]: e.target.checked })} />
    </label>
  );
}

export function SettingsScreen() {
  const s = useSettings();
  const connected = link.state === 'connected';
  return (
    <div className="settings">
      <h3>Pointer</h3>
      <Slider label="Speed" k="speed" min={0.5} max={4} step={0.1} />
      <Slider label="Acceleration" k="accel" min={0} max={2} step={0.1} />
      <Slider label="Scroll speed" k="scroll" min={0.2} max={3} step={0.1} />
      <Toggle label="Natural scrolling" k="natural" />
      <Toggle label="Tap to click" k="tapToClick" />
      <Toggle label="Long press = right click" k="longPressRight" />

      <h3>Typing</h3>
      <Slider label="Delay between keys, ms" k="typeDelay" min={2} max={50} step={1} />
      <label className="setting">
        <span>Computer</span>
        <select value={s.os} onChange={(e) => updateSettings({ os: e.target.value as HostOs })}>
          <option value="windows">Windows</option>
          <option value="mac">macOS</option>
          <option value="linux">Linux</option>
        </select>
      </label>

      <h3>App</h3>
      <Toggle label="Haptics" k="haptics" />
      <Toggle label="Keep screen awake" k="keepAwake" />
      <label className="setting">
        <span>Theme</span>
        <select value={s.theme} onChange={(e) => updateSettings({ theme: e.target.value as Settings['theme'] })}>
          <option value="auto">Match phone</option>
          <option value="dark">Dark</option>
          <option value="light">Light</option>
        </select>
      </label>

      <h3>NEXUS</h3>
      <div className="row">
        <button disabled={!connected} onClick={() => void link.control(Ctrl.identify).catch(() => undefined)}>
          Identify
        </button>
        <button disabled={!link.name} onClick={() => void link.forget()}>
          Forget {link.name || 'device'}
        </button>
      </div>
      <p className="hint">
        Pairing: on NEXUS open Settings → PHONE (or your pair key), then Connect here and type the six digits NEXUS shows.
      </p>
    </div>
  );
}
