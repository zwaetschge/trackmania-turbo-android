#!/usr/bin/env python3
"""Rough DXBC (SM4/5) disassembler, enough to read what a game shader does.

    tools/dxbc_dis.py [-d] <file.dxbc>...      (-d also prints the declarations)

Resource names come from the RDEF chunk (t0 -> TMapColor ...), constant-buffer
members are listed with their offsets so cb1[3] can be looked up by hand.
"""
import struct
import sys

OPS = {0: 'add', 1: 'and', 2: 'break', 3: 'breakc', 4: 'call', 5: 'callc', 6: 'case', 7: 'continue',
       8: 'continuec', 9: 'cut', 10: 'default', 11: 'deriv_rtx', 12: 'deriv_rty', 13: 'discard',
       14: 'div', 15: 'dp2', 16: 'dp3', 17: 'dp4', 18: 'else', 19: 'emit', 20: 'emitthencut',
       21: 'endif', 22: 'endloop', 23: 'endswitch', 24: 'eq', 25: 'exp', 26: 'frc', 27: 'ftoi',
       28: 'ftou', 29: 'ge', 30: 'iadd', 31: 'if', 32: 'ieq', 33: 'ige', 34: 'ilt', 35: 'imad',
       36: 'imax', 37: 'imin', 38: 'imul', 39: 'ine', 40: 'ineg', 41: 'ishl', 42: 'ishr', 43: 'itof',
       44: 'label', 45: 'ld', 46: 'ld_ms', 47: 'log', 48: 'loop', 49: 'lt', 50: 'mad', 51: 'min',
       52: 'max', 53: 'customdata', 54: 'mov', 55: 'movc', 56: 'mul', 57: 'ne', 58: 'nop', 59: 'not',
       60: 'or', 61: 'resinfo', 62: 'ret', 63: 'retc', 64: 'round_ne', 65: 'round_ni', 66: 'round_pi',
       67: 'round_z', 68: 'rsq', 69: 'sample', 70: 'sample_c', 71: 'sample_c_lz', 72: 'sample_l',
       73: 'sample_d', 74: 'sample_b', 75: 'sqrt', 76: 'switch', 77: 'sincos', 78: 'udiv', 79: 'ult',
       80: 'uge', 81: 'umul', 82: 'umad', 83: 'umax', 84: 'umin', 85: 'ushr', 86: 'utof', 87: 'xor',
       88: 'dcl_resource', 89: 'dcl_constantbuffer', 90: 'dcl_sampler', 91: 'dcl_indexrange',
       92: 'dcl_gs_out_topology', 93: 'dcl_gs_in_primitive', 94: 'dcl_max_out_vertices',
       95: 'dcl_input', 96: 'dcl_input_sgv', 97: 'dcl_input_siv', 98: 'dcl_input_ps',
       99: 'dcl_input_ps_sgv', 100: 'dcl_input_ps_siv', 101: 'dcl_output', 102: 'dcl_output_sgv',
       103: 'dcl_output_siv', 104: 'dcl_temps', 105: 'dcl_indexableTemp', 106: 'dcl_globalFlags',
       108: 'lod', 109: 'gather4', 110: 'sample_pos', 111: 'sample_info', 122: 'deriv_rtx_coarse',
       123: 'deriv_rtx_fine', 124: 'deriv_rty_coarse', 125: 'deriv_rty_fine', 126: 'gather4_c',
       127: 'gather4_po', 129: 'rcp', 130: 'f32tof16', 131: 'f16tof32', 134: 'countbits',
       138: 'ubfe', 139: 'ibfe', 140: 'bfi', 141: 'bfrev', 142: 'swapc'}
TYPES = {0: 'r', 1: 'v', 2: 'o', 3: 'x', 4: 'l', 5: 'l', 6: 's', 7: 't', 8: 'cb', 9: 'icb',
         10: 'label', 11: 'vPrim', 12: 'oDepth', 13: 'null', 14: 'rast', 15: 'oMask'}


def chunks(d):
    n = struct.unpack_from('<I', d, 28)[0]
    for o in struct.unpack_from('<%dI' % n, d, 32):
        size = struct.unpack_from('<I', d, o + 4)[0]
        yield d[o:o + 4], d[o + 8:o + 8 + size]


def rdef(c):
    ncb, ocb, nrb, orb = struct.unpack_from('<4I', c, 0)

    def s(off):
        return c[off:c.index(b'\0', off)].decode('latin1')
    names, cbs = {}, []
    for i in range(nrb):
        name, typ, _ret, _dim, _ns, bind, _cnt, _fl = struct.unpack_from('<8I', c, orb + 32 * i)
        pre = {0: 'cb', 2: 't', 3: 's'}.get(typ)
        if pre:
            names['%s%d' % (pre, bind)] = s(name)
    for i in range(ncb):
        name, nv, ov, sz, _fl, _ty = struct.unpack_from('<6I', c, ocb + 24 * i)
        vs = []
        for j in range(nv):
            vn, vo, vsz, vfl = struct.unpack_from('<4I', c, ov + 40 * j)
            if vfl & 2:
                vs.append('%s@%d[%d]+%d (%d B)' % (s(vn), vo // 16, vo // 16, vo % 16, vsz))
        cbs.append('%s (%d B): %s' % (s(name), sz, ', '.join(vs)))
    return names, cbs


def comps(tok):
    nc = tok & 3
    if nc != 2:
        return ''
    sel = (tok >> 2) & 3
    s = 'xyzw'
    if sel == 0:
        m = (tok >> 4) & 15
        return '.' + ''.join(s[i] for i in range(4) if m >> i & 1)
    if sel == 1:
        return '.' + ''.join(s[(tok >> (4 + 2 * i)) & 3] for i in range(4))
    return '.' + s[(tok >> 4) & 3]


def operand(t, i):
    tok = t[i]
    i += 1
    ext = tok >> 31
    mod = ''
    while ext:
        e = t[i]
        i += 1
        ext = e >> 31
        if (e & 63) == 1:
            mod = {0: '', 1: '-', 2: 'abs', 3: '-abs'}.get((e >> 6) & 255, '?')
    ty = (tok >> 12) & 255
    dim = (tok >> 20) & 3
    if ty == 4:
        n = 4 if (tok & 3) == 2 else 1
        fs = []
        for v in t[i:i + n]:
            f = struct.unpack('<f', struct.pack('<I', v))[0]
            fs.append('%g' % f if v == 0 or 1e-6 < abs(f) < 1e9 else '0x%x' % v)
        return 'l(' + ','.join(fs) + ')', i + n
    idx = []
    for k in range(dim):
        rep = (tok >> (22 + 3 * k)) & 7
        if rep == 0:
            idx.append(str(t[i]))
            i += 1
        elif rep == 1:
            idx.append(str(t[i] | (t[i + 1] << 32)))
            i += 2
        elif rep == 2:
            s, i = operand(t, i)
            idx.append(s)
        else:
            base = t[i]
            i += 1
            s, i = operand(t, i)
            idx.append('%s+%d' % (s, base))
    name = TYPES.get(ty, 'T%d' % ty)
    if idx:
        name += idx[0] + ''.join('[%s]' % x for x in idx[1:])
    s = name + comps(tok)
    if mod:
        s = '%s(%s)' % (mod, s) if 'abs' in mod else '-' + s
    return s, i


def dis(t, decls=False):
    i = 2
    depth = 0
    while i < len(t):
        tok = t[i]
        op = tok & 0x7ff
        ln = (tok >> 24) & 0x7f
        if op == 53:
            ln = t[i + 1]
            yield 'customdata (%d tokens)' % ln
            i += ln
            continue
        if ln == 0:
            break
        j = i + 1
        ext = tok >> 31
        while ext:
            ext = t[j] >> 31
            j += 1
        name = OPS.get(op, 'op%d' % op)
        if name.startswith('dcl_'):
            if decls:
                yield name + ' ' + ' '.join('%x' % x for x in t[j:i + ln])
            i += ln
            continue
        args = []
        try:
            while j < i + ln:
                s, j = operand(t, j)
                args.append(s)
        except (IndexError, struct.error):
            args.append('?')
        if name in ('if', 'breakc', 'discard', 'retc', 'continuec'):
            name += '_nz' if (tok >> 18) & 1 else '_z'
        if (tok >> 13) & 1:
            name += '_sat'
        if name in ('endif', 'endloop', 'else', 'endswitch'):
            depth -= 1
        yield '  ' * max(depth, 0) + name + ' ' + ', '.join(args)
        if name.startswith('if') or name in ('loop', 'else', 'switch'):
            depth += 1
        i += ln


def main():
    args = sys.argv[1:]
    decls = '-d' in args
    for p in [a for a in args if a != '-d']:
        d = open(p, 'rb').read()
        print('==', p)
        for tag, c in chunks(d):
            if tag == b'RDEF':
                names, cbs = rdef(c)
                print('  resources:', ' '.join('%s=%s' % kv for kv in sorted(names.items())))
                for cb in cbs:
                    print('  ', cb)
            elif tag in (b'SHDR', b'SHEX'):
                for line in dis(struct.unpack_from('<%dI' % (len(c) // 4), c, 0), decls):
                    print('    ' + line)


if __name__ == '__main__':
    main()
