// Settings live on the phone, in localStorage. Nothing here is sent anywhere
// except the typing delay, which the dongle forgets on every reconnect.
import { useSyncExternalStore } from 'react';
import type { HostOs } from './protocol/hid';

export interface Settings {
  speed: number; // pointer gain, 0.5-4
  accel: number; // 0 = linear, 2 = strong
  scroll: number; // notches per 24px of finger travel
  natural: boolean;
  tapToClick: boolean;
  longPressRight: boolean;
  typeDelay: number; // ms, 2-50
  os: HostOs;
  haptics: boolean;
  keepAwake: boolean;
  theme: 'auto' | 'light' | 'dark';
}

export const DEFAULTS: Settings = {
  speed: 1.6,
  accel: 1,
  scroll: 1,
  natural: true,
  tapToClick: true,
  longPressRight: false,
  typeDelay: 8,
  os: 'windows',
  haptics: true,
  keepAwake: true,
  theme: 'auto',
};

const KEY = 'nexus.settings';
const listeners = new Set<() => void>();

function load(): Settings {
  try {
    return { ...DEFAULTS, ...JSON.parse(localStorage.getItem(KEY) ?? '{}') };
  } catch {
    return DEFAULTS;
  }
}

let current = load();

export function getSettings(): Settings {
  return current;
}

export function updateSettings(patch: Partial<Settings>) {
  current = { ...current, ...patch };
  try {
    localStorage.setItem(KEY, JSON.stringify(current));
  } catch {
    /* private mode: settings last for the session */
  }
  for (const l of listeners) l();
}

export function useSettings(): Settings {
  return useSyncExternalStore(
    (fn) => {
      listeners.add(fn);
      return () => listeners.delete(fn);
    },
    () => current,
  );
}

export function haptic() {
  if (current.haptics) navigator.vibrate?.(8);
}
