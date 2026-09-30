import type { Bytes } from './packets';

// Text the dongle can type: printable ASCII, \n and \t, on a US layout.

// What phone keyboards substitute behind your back, mapped back to what you
// meant. Anything else outside the set is dropped rather than mistyped.
const SUBSTITUTES: Record<string, string> = {
  '‘': "'", '’': "'", '‚': "'", '′': "'",
  '“': '"', '”': '"', '„': '"', '″': '"',
  '–': '-', '—': '-', '−': '-',
  '…': '...', ' ': ' ', '•': '*',
};

export function isTypeable(code: number): boolean {
  return (code >= 0x20 && code <= 0x7e) || code === 0x0a || code === 0x09;
}

/** Normalise line endings and smart punctuation; drop what cannot be typed. */
export function sanitize(s: string): string {
  let out = '';
  for (const ch of s.replace(/\r\n?/g, '\n')) {
    const sub = SUBSTITUTES[ch] ?? ch;
    for (const c of sub) if (isTypeable(c.charCodeAt(0))) out += c;
  }
  return out;
}

/** Split sanitised text into writes of at most @p max bytes. */
export function chunks(s: string, max = 20): Bytes[] {
  const bytes = Uint8Array.from(s, (c) => c.charCodeAt(0));
  const out: Bytes[] = [];
  for (let i = 0; i < bytes.length; i += max) out.push(bytes.subarray(i, i + max));
  return out;
}
