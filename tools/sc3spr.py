#!/usr/bin/env python3
"""SimCity 3000 sprite archive (.DAT, IXF index) reader. Reads the user's own game files.
   Library: ixf_index(path) -> [(type, group, inst, off, size)], refpack(data) -> bytes"""
import struct, sys

def ixf_index(d):
    assert d[:4] == b'\xd7\x81\xc3\x80', 'not an IXF file'
    p, ents = 4, []
    while p + 20 <= len(d):
        t, g, i, o, s = struct.unpack_from('<5I', d, p)
        if t == 0 and g == 0 and o == 0: break
        ents.append((t, g, i, o, s)); p += 20
    return ents

def refpack(src):
    """EA RefPack (QFS) decompression."""
    flags, pos = src[0], 2
    if src[1] != 0xFB: raise ValueError('bad refpack magic')
    n = 4 if flags & 0x80 else 3
    usize = int.from_bytes(src[pos:pos + n], 'big'); pos += n
    if flags & 0x01: pos += n
    out = bytearray()
    while pos < len(src):
        b0 = src[pos]
        if b0 < 0x80:
            b1 = src[pos + 1]; pos += 2
            plain, copy, off = b0 & 3, ((b0 & 0x1c) >> 2) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = src[pos + 1], src[pos + 2]; pos += 3
            plain, copy, off = (b1 >> 6) & 3, (b0 & 0x3f) + 4, ((b1 & 0x3f) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = src[pos + 1], src[pos + 2], src[pos + 3]; pos += 4
            plain, copy, off = b0 & 3, ((b0 & 0x0c) << 6) + b3 + 5, ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            pos += 1; plain, copy, off = ((b0 & 0x1f) << 2) + 4, 0, 0
        else:
            pos += 1; plain = b0 & 3
            out += src[pos:pos + plain]; break
        out += src[pos:pos + plain]; pos += plain
        for _ in range(copy): out.append(out[-off])
    if len(out) != usize: raise ValueError('size mismatch %d != %d' % (len(out), usize))
    return bytes(out)

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    ents = ixf_index(d)
    want = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    for t, g, i, o, s in ents:
        if want is not None and g != want: continue
        rec = d[o:o + s]
        if i == 1: print('group %08x inst 1: %s' % (g, rec.hex())); continue
        hdr = struct.unpack_from('<5I', rec, 0)
        raw = refpack(rec[20:])
        print('group %08x hdr %s raw %d' % (g, [hex(x) for x in hdr], len(raw)))
        print('   ', raw[:96].hex())
        if want is None: break

def decode_sprite(raw):
    """Decompressed SC3 sprite -> (w, h, rgba bytes, (u1, u2)).
    Layout: u32 total, u16 w, u16 h, u16 u1, u16 u2, u32 key (RGB565, 0xF81F), u32 0, then one run per row:
    u16 x, u16 len (bit15 = flag), u32 cumulative pixel offset after this run (absent for the last row),
    then RGB565 pixels for all runs back to back."""
    total, w, h, u1, u2, key = struct.unpack_from('<IHHHHI', raw, 0)
    p, prev, runs = 20, 0, []
    for y in range(h):
        x, ln = struct.unpack_from('<HH', raw, p); ln &= 0x7fff
        runs.append((x, ln, prev))
        if y < h - 1:
            prev = struct.unpack_from('<I', raw, p + 4)[0]; p += 8
        else:
            p += 4
    pix0 = p
    img = bytearray(w * h * 4)
    for y, (x, ln, start) in enumerate(runs):
        for k in range(ln):
            q = pix0 + 2 * (start + k)
            if q + 2 > len(raw) or x + k >= w: continue
            v = raw[q] | raw[q + 1] << 8
            if v == (key & 0xffff): continue
            o = (y * w + x + k) * 4
            img[o:o + 4] = bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3, 255))
    return w, h, bytes(img), (u1, u2)

def load_dat(path):
    d = open(path, 'rb').read()
    return d, {(g, i): (o, s) for t, g, i, o, s in ixf_index(d)}

def sprite(d, ents, group):
    o, s = ents[(group, 0)]
    return decode_sprite(refpack(d[o + 20:o + s]))

def anchor(d, ents, group):
    o, s = ents.get((group, 1), (0, 0))
    return struct.unpack_from('<4h', d, o) if s == 8 else None
