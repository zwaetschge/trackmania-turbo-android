#!/usr/bin/env python3
"""Decompress the body of a GBX file (version 6, LZO1X-compressed body) for inspection.

    tools/gbx_body.py <file.Gbx> [out.bin]

Prints the header fields and a hex/ASCII dump of the decompressed body (or writes it).
"""
import struct
import sys


def lzo1x_decompress(src, out_len):
    """Plain LZO1X-1 decompressor (as in minilzo's lzo1x_decompress)."""
    dst = bytearray()
    ip = 0

    def copy_match(dist, n):
        p = len(dst) - dist
        for i in range(n):
            dst.append(dst[p + i])

    t = src[ip]
    if t > 17:
        ip += 1
        t -= 17
        dst += src[ip:ip + t]; ip += t
        t = src[ip]; ip += 1
        if t < 16:
            # match after literal run of first block
            dist = 1 + 0x0800 + (t >> 2) + (src[ip] << 2); ip += 1
            copy_match(dist, 3)
            t = src[ip - 2] & 3
            if t:
                dst += src[ip:ip + t]; ip += t
            t = None
    else:
        t = None
    state = 0 if t is None else None
    while True:
        if t is None:
            t = src[ip]; ip += 1
            if t < 16:
                if state == 0:            # literal run
                    if t == 0:
                        while src[ip] == 0:
                            t += 255; ip += 1
                        t += 15 + src[ip]; ip += 1
                    t += 3
                    dst += src[ip:ip + t]; ip += t
                    state = 4
                    t = src[ip]; ip += 1
                    if t >= 16:
                        state = 1
                        continue_match = True
                    else:
                        dist = 1 + 0x0800 + (t >> 2) + (src[ip] << 2); ip += 1
                        copy_match(dist, 3)
                        state = src[ip - 2] & 3
                        if state:
                            dst += src[ip:ip + state]; ip += state
                        t = None
                        continue
                else:
                    dist = 1 + (t >> 2) + (src[ip] << 2); ip += 1
                    copy_match(dist, 2)
                    state = src[ip - 2] & 3
                    if state:
                        dst += src[ip:ip + state]; ip += state
                    t = None
                    continue
        # here t >= 16: a match
        if t >= 64:
            dist = 1 + ((t >> 2) & 7) + (src[ip] << 3); ip += 1
            n = (t >> 5) - 1 + 2
        elif t >= 32:
            n = t & 31
            if n == 0:
                while src[ip] == 0:
                    n += 255; ip += 1
                n += 31 + src[ip]; ip += 1
            n += 2
            dist = 1 + (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2
        elif t >= 16:
            dist = (t & 8) << 11
            n = t & 7
            if n == 0:
                while src[ip] == 0:
                    n += 255; ip += 1
                n += 7 + src[ip]; ip += 1
            n += 2
            dist += (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2
            if dist == 0:
                break                     # end of stream
            dist += 0x4000
        copy_match(dist, n)
        state = src[ip - 2] & 3
        if state:
            dst += src[ip:ip + state]; ip += state
            state = state
        t = None
        if state == 0:
            state = 0
        else:
            state = 4
    if len(dst) != out_len:
        print('warning: decompressed %d bytes, expected %d' % (len(dst), out_len), file=sys.stderr)
    return bytes(dst)


def main():
    d = open(sys.argv[1], 'rb').read()
    assert d[:3] == b'GBX'
    ver = struct.unpack_from('<H', d, 3)[0]
    fmt = d[5:9].decode()
    cls = struct.unpack_from('<I', d, 9)[0]
    p = 13
    udsize = struct.unpack_from('<I', d, p)[0]; p += 4
    p += udsize
    nodes = struct.unpack_from('<I', d, p)[0]; p += 4
    ext = struct.unpack_from('<I', d, p)[0]; p += 4
    print('version %d format %s class %08X user data %d bytes, %d nodes, %d external' % (ver, fmt, cls, udsize, nodes, ext))
    if ext:
        print('external refs present: not handled'); return
    if fmt[2] == 'C':
        usize, csize = struct.unpack_from('<II', d, p); p += 8
        body = lzo1x_decompress(d[p:p + csize], usize)
    else:
        body = d[p:]
    if len(sys.argv) > 2:
        open(sys.argv[2], 'wb').write(body); print('body: %d bytes -> %s' % (len(body), sys.argv[2])); return
    for i in range(0, len(body), 16):
        b = body[i:i + 16]
        print('%04X  %-48s %s' % (i, ' '.join('%02x' % x for x in b), ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)))


if __name__ == '__main__':
    main()
