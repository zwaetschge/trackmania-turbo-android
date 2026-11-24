"""Extract the precompiled D3D11 shader bytecode (DXBC) from the game's
GpuCache_D3D11_SM*.zip for renderer tests. Read-only on the game files; the
output is derived game data and stays in work/ (gitignored).

  extract_gpucache.py <GpuCache zip> <out dir>

Each zip entry is a GBX v6 container ("BUCE": binary, compressed body) whose
body is one LZO1X stream.
"""
import os, struct, sys, zipfile, collections


def lzo1x_decompress(src, out_len):
    out = bytearray()
    ip = 0

    def copy_match(dist, n):
        pos = len(out) - dist
        if pos < 0:
            raise ValueError("lzo: lookbehind overrun")
        for _ in range(n):
            out.append(out[pos]); pos += 1

    def literals(n):
        nonlocal ip
        out.extend(src[ip:ip + n]); ip += n

    state = "start"
    t = src[ip]
    if t > 17:
        ip += 1
        t -= 17
        if t < 4:
            literals(t); t = src[ip]; ip += 1; state = "match"
        else:
            literals(t); state = "first_literal_run"
    else:
        state = "literal_or_match"
    while True:
        if state == "literal_or_match":
            t = src[ip]; ip += 1
            if t >= 16:
                state = "match"; continue
            if t == 0:
                while src[ip] == 0:
                    t += 255; ip += 1
                t += 15 + src[ip]; ip += 1
            literals(t + 3)
            state = "first_literal_run"
        if state == "first_literal_run":
            t = src[ip]; ip += 1
            if t >= 16:
                state = "match"; continue
            dist = 1 + 0x800 + (t >> 2) + (src[ip] << 2); ip += 1
            copy_match(dist, 3)
            state = "match_done"
        if state == "match":
            if t >= 64:
                dist = 1 + ((t >> 2) & 7) + (src[ip] << 3); ip += 1
                copy_match(dist, (t >> 5) - 1 + 2)
            elif t >= 32:
                t &= 31
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 31 + src[ip]; ip += 1
                dist = 1 + ((src[ip] | (src[ip + 1] << 8)) >> 2); ip += 2
                copy_match(dist, t + 2)
            elif t >= 16:
                dist = (t & 8) << 11
                t &= 7
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 7 + src[ip]; ip += 1
                dist += (src[ip] | (src[ip + 1] << 8)) >> 2; ip += 2
                if dist == 0:
                    break                      # end-of-stream marker
                copy_match(dist + 0x4000, t + 2)
            else:
                dist = 1 + (t >> 2) + (src[ip] << 2); ip += 1
                copy_match(dist, 2)
            state = "match_done"
        if state == "match_done":
            t = src[ip - 2] & 3
            if t == 0:
                state = "literal_or_match"; continue
            literals(t)
            t = src[ip]; ip += 1
            state = "match"
    if len(out) != out_len:
        raise ValueError(f"lzo: got {len(out)} bytes, expected {out_len}")
    return bytes(out)


def gbx_body(data):
    if data[:3] != b"GBX":
        raise ValueError("not GBX")
    ver = struct.unpack_from("<H", data, 3)[0]
    fmt = data[5:9]
    off = 9
    off += 4                                   # class id
    if ver >= 6:
        n = struct.unpack_from("<I", data, off)[0]; off += 4 + n
    off += 4                                   # node count
    ext = struct.unpack_from("<I", data, off)[0]; off += 4
    if ext:
        raise ValueError("external references not handled")
    if fmt[2:3] == b"C":
        usize, csize = struct.unpack_from("<II", data, off); off += 8
        return lzo1x_decompress(data[off:off + csize], usize)
    return data[off:]


def main(zpath, outdir):
    os.makedirs(outdir, exist_ok=True)
    z = zipfile.ZipFile(zpath)
    kinds = collections.Counter(); n = bad = 0
    names = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs"}
    for info in z.infolist():
        if info.is_dir():
            continue
        try:
            body = gbx_body(z.read(info))
        except Exception as e:
            bad += 1; print("skip", info.filename, e); continue
        k = body.find(b"DXBC"); idx = 0
        while k >= 0:
            total, nchunks = struct.unpack_from("<II", body, k + 24)
            blob = body[k:k + total]
            kind = "??"
            for c in range(nchunks):
                co = struct.unpack_from("<I", blob, 32 + 4 * c)[0]
                if blob[co:co + 4] in (b"SHEX", b"SHDR"):
                    vt = struct.unpack_from("<I", blob, co + 8)[0]
                    kind = names.get(vt >> 16, "??") + f"_{(vt >> 4) & 15}_{vt & 15}"
            kinds[kind] += 1
            base = info.filename.replace("/", "__").replace(" ", "_").replace(".GpuCache.Gbx", "")
            with open(os.path.join(outdir, f"{base}.{idx}.{kind}.dxbc"), "wb") as f:
                f.write(blob)
            n += 1; idx += 1
            k = body.find(b"DXBC", k + max(total, 4))
    print(f"{n} DXBC blobs from {len(z.infolist())} entries ({bad} skipped):", dict(kinds))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
