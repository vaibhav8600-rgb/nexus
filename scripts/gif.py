"""A GIF89a writer, stdlib only.

Same constraint as png2c.py and for the same reason: this has to run in a
container that has never heard of Pillow.

Quantisation is deliberately absent. A NEXUS frame uses 36 colours for Snake
and 124 for the busiest dashboard - the UI is flat fills over one gradient, not
a photograph - so the exact colours fit in a 256-entry table with room to
spare. The encoder builds that table from the frames it is given and refuses
rather than degrade if a set ever exceeds it: a silently dithered screenshot
would undermine the whole point of generating these from source.

LZW here is the textbook GIF variant - codes start one bit wider than the
minimum code size, the table grows to 12 bits, and a clear code resets it.
"""

import struct


def _lzw(indices, min_code_size):
    """GIF's LZW. Returns the packed code stream, without block framing."""
    clear = 1 << min_code_size
    eoi = clear + 1

    table = {bytes([i]): i for i in range(clear)}
    nxt = eoi + 1
    width = min_code_size + 1

    out = bytearray()
    bitbuf = 0
    bitcnt = 0

    def emit(code):
        nonlocal bitbuf, bitcnt
        bitbuf |= code << bitcnt
        bitcnt += width
        while bitcnt >= 8:
            out.append(bitbuf & 0xFF)
            bitbuf >>= 8
            bitcnt -= 8

    emit(clear)
    prefix = b''
    for value in indices:
        cur = prefix + bytes([value])
        if cur in table:
            prefix = cur
            continue
        emit(table[prefix])
        if nxt < 4096:
            table[cur] = nxt
            nxt += 1
            # The width grows AFTER the code that filled the old range, which
            # is the off-by-one every hand-written GIF encoder hits once.
            if nxt - 1 == (1 << width) and width < 12:
                width += 1
        else:
            emit(clear)
            table = {bytes([i]): i for i in range(clear)}
            nxt = eoi + 1
            width = min_code_size + 1
        prefix = bytes([value])

    if prefix:
        emit(table[prefix])
    emit(eoi)
    if bitcnt:
        out.append(bitbuf & 0xFF)
    return bytes(out)


def _blocks(data):
    """Sub-block framing: at most 255 bytes each, terminated by an empty one."""
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def write_gif(path, w, h, frames, delays_cs, scale=1, loop=0):
    """
    frames      list of flat RGB565 buffers, w*h each
    delays_cs   per-frame delay in centiseconds (GIF's unit, not ms)
    scale       integer nearest-neighbour upscale
    loop        0 = forever
    """
    if len(frames) != len(delays_cs):
        raise ValueError('one delay per frame')

    def rgb(v):
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)

    palette = sorted({c for f in frames for c in f})
    if len(palette) > 256:
        raise ValueError('%d colours; GIF holds 256 and this writer does not '
                         'quantise' % len(palette))
    index = {c: i for i, c in enumerate(palette)}

    bits = max(1, (len(palette) - 1).bit_length())
    size = 1 << bits
    gct = b''.join(bytes(rgb(c)) for c in palette)
    gct += b'\x00\x00\x00' * (size - len(palette))

    ow, oh = w * scale, h * scale
    out = bytearray(b'GIF89a')
    out += struct.pack('<HHBBB', ow, oh, 0xF0 | (bits - 1), 0, 0)
    out += gct
    out += b'\x21\xFF\x0BNETSCAPE2.0\x03\x01' + struct.pack('<H', loop) + b'\x00'

    mcs = max(2, bits)
    prev = None
    for frame, delay in zip(frames, delays_cs):
        # Only the rectangle that actually changed goes in the file. These
        # screens are mostly static ground with a small live area - a Snake
        # board is 192px of a 240px panel and the rest never moves - so full
        # frames spend most of their bytes re-encoding pixels already on
        # screen. Disposal 1 (leave in place) is what makes that legal.
        x0, y0, x1, y1 = 0, 0, w, h
        if prev is not None:
            diff = [i for i, (a, b) in enumerate(zip(prev, frame)) if a != b]
            if not diff:
                # Nothing moved. A 1x1 no-op still carries the delay.
                x0, y0, x1, y1 = 0, 0, 1, 1
            else:
                xs = [i % w for i in diff]
                ys = [i // w for i in diff]
                x0, x1 = min(xs), max(xs) + 1
                y0, y1 = min(ys), max(ys) + 1

        out += b'\x21\xF9\x04\x04' + struct.pack('<H', delay) + b'\x00\x00'
        out += b'\x2C' + struct.pack('<HHHHB', x0 * scale, y0 * scale,
                                     (x1 - x0) * scale, (y1 - y0) * scale, 0)

        idx = bytearray()
        for y in range(y0, y1):
            row = bytes(index[c] for c in frame[y * w + x0:y * w + x1])
            if scale > 1:
                row = bytes(b for px in row for b in (px,) * scale)
            idx += row * scale

        out.append(mcs)
        out += _blocks(_lzw(idx, mcs))
        prev = frame

    out += b'\x3B'
    open(path, 'wb').write(bytes(out))
    return len(out), len(palette)


if __name__ == '__main__':
    import os
    import tempfile

    # Round-trip check: a 4x4 two-colour checker, decoded back by hand.
    W = H = 4
    a, b = 0x0000, 0xFFFF
    f = [a if (x + y) % 2 else b for y in range(H) for x in range(W)]
    path = os.path.join(tempfile.gettempdir(), '_gif_selftest.gif')
    n, ncol = write_gif(path, W, H, [f, f[::-1]], [50, 50])
    data = open(path, 'rb').read()
    assert data[:6] == b'GIF89a', 'header'
    assert data[-1:] == b'\x3B', 'trailer'
    assert b'NETSCAPE2.0' in data, 'loop extension'
    assert ncol == 2, ncol
    print('gif.py self-test ok: %d bytes, %d colours' % (n, ncol))
