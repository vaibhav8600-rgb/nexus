import type { Ctx } from '../App';
import { Mod } from '../protocol/packets';
import { useSettings } from '../settings';

/** Sticky modifiers: tap for the next key, tap again to lock. */
export function ModRow({ ctx }: { ctx: Ctx }) {
  const { os } = useSettings();
  const mods: [string, number][] = [
    ['Ctrl', Mod.ctrl],
    ['Shift', Mod.shift],
    [os === 'mac' ? 'Opt' : 'Alt', Mod.alt],
    [os === 'mac' ? 'Cmd' : os === 'windows' ? 'Win' : 'Super', Mod.gui],
  ];
  return (
    <div className="mods">
      {mods.map(([label, bit]) => (
        <button
          key={label}
          className={ctx.mods.locked & bit ? 'locked' : ctx.mods.once & bit ? 'on' : ''}
          onClick={() => ctx.tapModifier(bit)}
        >
          {label}
        </button>
      ))}
    </div>
  );
}
