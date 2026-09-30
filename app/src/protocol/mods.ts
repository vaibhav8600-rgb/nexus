// Sticky modifiers: one tap applies to the next key, a second tap locks it,
// a third lets go. Pure, so the rules are testable without a UI.

export interface ModState {
  once: number;
  locked: number;
}

export const NO_MODS: ModState = { once: 0, locked: 0 };

export function tapMod(s: ModState, bit: number): ModState {
  if (s.locked & bit) return { once: s.once & ~bit, locked: s.locked & ~bit };
  if (s.once & bit) return { once: s.once & ~bit, locked: s.locked | bit };
  return { once: s.once | bit, locked: s.locked };
}

/** The modifiers to send with a key, and the state after sending it. */
export function consumeMods(s: ModState): [number, ModState] {
  return [s.once | s.locked, { once: 0, locked: s.locked }];
}
