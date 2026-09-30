import type { Ctx } from '../App';
import { MEDIA_KEYS } from '../protocol/hid';

export function Media({ ctx }: { ctx: Ctx }) {
  return (
    <div className="media">
      {MEDIA_KEYS.map((k) => (
        <button key={k.label} onClick={() => ctx.tapKey(k.page, k.usage)}>
          {k.label}
        </button>
      ))}
    </div>
  );
}
