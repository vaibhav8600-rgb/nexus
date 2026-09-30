// HID usages the UI sends. Keyboard page 0x07, consumer page 0x0C.
import { Mod, Page } from './packets';

export interface KeyDef {
  label: string;
  page: number;
  usage: number;
  mods?: number;
}

const k = (label: string, usage: number, mods = 0): KeyDef => ({ label, page: Page.keyboard, usage, mods });
const c = (label: string, usage: number): KeyDef => ({ label, page: Page.consumer, usage });

export const KEY = {
  enter: 0x28, esc: 0x29, backspace: 0x2a, tab: 0x2b, space: 0x2c,
  printScreen: 0x46, insert: 0x49, home: 0x4a, pageUp: 0x4b, del: 0x4c,
  end: 0x4d, pageDown: 0x4e, right: 0x4f, left: 0x50, down: 0x51, up: 0x52,
  f1: 0x3a,
} as const;

/** Keyboard usage for a letter or digit, for shortcuts. */
export function letter(ch: string): number {
  const code = ch.toLowerCase().charCodeAt(0);
  if (code >= 97 && code <= 122) return 0x04 + code - 97;
  if (code === 48) return 0x27;
  if (code >= 49 && code <= 57) return 0x1e + code - 49;
  throw new Error(`no usage for ${ch}`);
}

export const SPECIAL_KEYS: KeyDef[] = [
  k('Esc', KEY.esc), k('Tab', KEY.tab), k('Enter', KEY.enter), k('⌫', KEY.backspace),
  k('Del', KEY.del), k('←', KEY.left), k('↑', KEY.up), k('↓', KEY.down), k('→', KEY.right),
  k('Home', KEY.home), k('End', KEY.end), k('PgUp', KEY.pageUp), k('PgDn', KEY.pageDown),
  k('Ins', KEY.insert), k('PrtSc', KEY.printScreen),
  ...Array.from({ length: 12 }, (_, i) => k(`F${i + 1}`, KEY.f1 + i)),
];

export const MEDIA_KEYS: KeyDef[] = [
  c('Vol −', 0xea), c('Mute', 0xe2), c('Vol +', 0xe9),
  c('⏮', 0xb6), c('⏯', 0xcd), c('⏭', 0xb5),
  c('Bright −', 0x70), c('Bright +', 0x6f),
];

export type HostOs = 'windows' | 'mac' | 'linux';

/** The primary shortcut modifier: Cmd on a Mac, Ctrl elsewhere. */
const primary = (os: HostOs) => (os === 'mac' ? Mod.gui : Mod.ctrl);

export function shortcuts(os: HostOs): KeyDef[] {
  const p = primary(os);
  const mac = os === 'mac';
  return [
    k('Copy', letter('c'), p), k('Paste', letter('v'), p), k('Cut', letter('x'), p),
    k('Undo', letter('z'), p),
    mac ? k('Redo', letter('z'), p | Mod.shift) : k('Redo', letter('y'), p),
    k('Select all', letter('a'), p), k('Save', letter('s'), p), k('Find', letter('f'), p),
    k('Switch app', KEY.tab, mac ? Mod.gui : Mod.alt),
    mac ? k('Desktop', 0x44 /* F11 */) : k('Desktop', letter('d'), Mod.gui),
    mac ? k('Lock', letter('q'), Mod.ctrl | Mod.gui) : k('Lock', letter('l'), Mod.gui),
    mac ? k('Close', letter('w'), Mod.gui) : k('Close', 0x3d /* F4 */, Mod.alt),
    k('New tab', letter('t'), p), k('Close tab', letter('w'), p),
    k('Next tab', KEY.tab, Mod.ctrl),
    mac ? k('Force quit', KEY.esc, Mod.gui | Mod.alt) : k('Task mgr', KEY.esc, Mod.ctrl | Mod.shift),
  ];
}
