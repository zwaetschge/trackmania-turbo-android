"""Print the input/output signatures (and size) of DXBC shader blobs.
usage: dxbc_sig.py <file.dxbc> [...]"""
import struct, sys
def sig(d, off):
    n, _ = struct.unpack_from('<II', d, off + 8); out = []
    for i in range(n):
        name_off, idx, sv, comp, reg = struct.unpack_from('<IIIII', d, off + 16 + 24 * i)
        mask, rw = d[off + 16 + 24 * i + 20], d[off + 16 + 24 * i + 21]
        name = d[off + 8 + name_off:d.index(b'\0', off + 8 + name_off)].decode()
        out.append(f"{name}{idx}:r{reg}:mask{mask:X}/{rw:X}" + (f":sv{sv}" if sv else ''))
    return ' '.join(out)
for f in sys.argv[1:]:
    d = open(f, 'rb').read()
    if d[:4] != b'DXBC': print(f, 'not DXBC'); continue
    n = struct.unpack_from('<I', d, 28)[0]
    parts = []
    for i in range(n):
        o = struct.unpack_from('<I', d, 32 + 4 * i)[0]; tag = d[o:o + 4].decode('latin1')
        if tag in ('ISGN', 'OSGN', 'ISG1', 'OSG1'): parts.append(f"{tag}[{sig(d, o)}]")
        elif tag in ('SHDR', 'SHEX'):
            ver = struct.unpack_from('<I', d, o + 8)[0]; parts.append(f"{tag} {'vs' if (ver>>16)==1 else 'ps' if (ver>>16)==0 else 'gs'}_{(ver>>4)&15}_{ver&15} {struct.unpack_from('<I', d, o+4)[0]}B")
    print(f.split('/')[-1], len(d), 'bytes |', ' | '.join(parts))
