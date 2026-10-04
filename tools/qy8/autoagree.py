#!/usr/bin/env python3
"""Get past the telematics consent screen on a Clarion QY8 head unit.

Two builds, both patching ScreenStateCC.dll inside NK2:

skip   The consent screen never appears. SK_CWS_AGREE_EXECUTE and SK_CWS_AGREE_SKIP
       are handled side by side in INI_STARTUP's EventHandler and both run the same
       composite action; the only difference between them is the string written to
       VAR_CWS_AGREE_SKIP, so whether the screen comes up is that one variable. The
       EXECUTE handler is rewritten to write "true" instead of "false" and, in the
       instructions that frees, to do the agree itself: VAR_I_AGREE_STATUS and
       VAR_START_UP_I_AGREE_STATUS both "true", then ExecuteFunction(25,
       TELEMA_NOTIFY_AGREE). Telema gets its notice, CONF 0x08 stays 1, and
       NissanConnect EV stays in the Info menu. 29 instructions in, 29 out.

timer  The real screen runs and accepts itself. State 838's entry action is rewritten
       to spend its slack on SetEventTimer(SK_SCT_AGREE_CLICK, 2000), so the state
       delivers the click to itself and the stock OK path replays. ZE1 only so far.

Patch the map card. The nav image runs from the card and patching it alone is enough,
tested against a stock NAND, so the NAND never has to be written. The same script
patches a NAND dump if you want the fallback copy done too. Whichever you patch, the
512-byte .mod header stays byte-identical, which is what lets the card's copy boot.

ScreenStateCC's code section is uncompressed in the ROM but lives inside an LZ4 chunk,
so that chunk is recompressed with LZ4_compress_HC. It comes out smaller than the stock
packer managed, so it still fits its sectors.

Every checksum in the container is a 32-bit word sum. The loader sums the first 0x1e0
bytes of the MTCP header sector plus the chunk sectors and nothing else, so the
compensating word goes in the reserved zeros at MTCP+0x1c. Put it in the sector's tail
padding instead and the loader never sees it: it prints "Multi Load NK2(SD) Load
failed" and falls back to the NAND.

usage: autoagree.py SRC DST [skip|timer] [DELAY_MS]
"""
import ctypes.util, hashlib, os, shutil, struct, subprocess, sys

# ScreenState.dll entry points, as thunks inside ScreenStateCC
BUILDS = {
    'G218ENNI.120': dict(                       # ZE1, 2018+
        mod_len=31984640, sec0_file=0xa7c000, sec0_va=0x413d1000,
        get_context=0x41b2f560, set_var=0x41b2f3d0, check_var=0x41b2f3e0,
        get_state=0x41b2f430, get_state2=0x41b2f3f0, ev_handler=0x41b2f640,
        exec_fn=0x41b2f5b0, set_timer=0x41b2f650,
        skip=dict(
            va=0x41a0a5c0, tail=0x41a0a680, p_true=0x41a0a690,
            var_skip=73, var_agree=257, var_startup=628,
            action=4892, ctrl=25, notify=1687,
            stock=[0xeb0493e6, 0xe58d0134, 0xe59f20bc, 0xe3a01049, 0xe59d0134,
                   0xeb04937d, 0xeb0493e0, 0xe58d0138, 0xe59d0138, 0xeb049391,
                   0xe58d013c, 0xe301131c, 0xe59d013c, 0xeb049411, 0xe58d0140,
                   0xe59d3140, 0xe3530000, 0x1a000007, 0xeb0493d4, 0xe58d0144,
                   0xe59d0144, 0xeb049375, 0xe58d0148, 0xe301131c, 0xe59d0148,
                   0xeb049405, 0xe3a03001, 0xe58d3000, 0xea000012]),
        timer=dict(
            va=0x419b8290, tail=0x419bb13c, p_true=0x419b91c8, p_false=0x419b91cc,
            var_startup=628, event=0xc93,
            stock=[0xeb05dcb2, 0xe58d0004, 0xe59f2f2c, 0xe3a01f9d, 0xe59d0004,
                   0xeb05dc4d, 0xe58d0008, 0xe59d3008, 0xe3530000, 0x0a000005,
                   0xeb05dca8, 0xe58d000c, 0xe59f2f00, 0xe3a01f9d, 0xe59d000c,
                   0xeb05dc3f, 0xe3a03001, 0xe58d3000, 0xea000b97]),
    ),
    'G214ELNI.062': dict(                       # ZE0, 2014-2017
        mod_len=30647808, sec0_file=0xa11000, sec0_va=0x412e1000,
        get_context=0x419d376c, set_var=0x419d35dc, check_var=0x419d35ec,
        get_state=0x419d363c, get_state2=0x419d35fc, ev_handler=0x419d384c,
        exec_fn=0x419d37bc, set_timer=0x419d385c,
        skip=dict(
            va=0x418b6488, tail=0x418b6530, p_true=0x418b6540,
            var_skip=47, var_agree=223, var_startup=563,
            action=4676, ctrl=25, notify=1609,
            stock=[0xeb0474b7, 0xe58d00d4, 0xe59f20a4, 0xe3a0102f, 0xe59d00d4,
                   0xeb04744e, 0xeb0474b1, 0xe58d00d8, 0xe59d00d8, 0xeb047462,
                   0xe58d00dc, 0xe3011244, 0xe59d00dc, 0xeb0474e2, 0xe58d00e0,
                   0xe59d30e0, 0xe3530000, 0x1a000007, 0xeb0474a5, 0xe58d00e4,
                   0xe59d00e4, 0xeb047446, 0xe58d00e8, 0xe3011244, 0xe59d00e8,
                   0xeb0474d6, 0xe3a03001, 0xe58d3000, 0xea00000c]),
    ),
}

DELAY_MS = 2000
MODE = 'skip'

ssum = lambda b: sum(struct.unpack_from('<%dI' % (len(b) // 4), b)) & 0xffffffff


def bl(at, to):  return 0xeb000000 | (((to - (at + 8)) >> 2) & 0xffffff)
def b_(at, to):  return 0xea000000 | (((to - (at + 8)) >> 2) & 0xffffff)
def beq(at, to): return 0x0a000000 | (((to - (at + 8)) >> 2) & 0xffffff)
def bne(at, to): return 0x1a000000 | (((to - (at + 8)) >> 2) & 0xffffff)
def movw(rd, imm): return 0xe3000000 | ((imm >> 12) << 16) | (rd << 12) | (imm & 0xfff)

def mov(rd, imm):
    """mov Rd, #imm for the values this patch needs (8 bits, even rotate)."""
    for rot in range(16):
        v = ((imm << 2 * rot) | (imm >> (32 - 2 * rot))) & 0xffffffff
        if v < 0x100: return 0xe3a00000 | (rot << 8) | (rd << 12) | v
    raise ValueError(f'{imm} is not an ARM immediate')

def movi(rd, imm):
    """mov Rd, #imm where ARM can rotate it, movw otherwise."""
    try:
        return mov(rd, imm)
    except ValueError:
        return movw(rd, imm)


def ldr_pc(at, rd, to):
    off = to - (at + 8)
    assert 0 <= off <= 0xfff, f'pc-relative load out of range: {off:#x}'
    return 0xe59f0000 | (rd << 12) | off


def skip_body(b, p):
    """SK_CWS_AGREE_EXECUTE, rewritten to skip the screen and agree on its own."""
    a, done = p['va'], p['va'] + 0x68
    w = [
        bl(a + 0x00, b['get_context']),
        ldr_pc(a + 0x04, 2, p['p_true']),
        movi(1, p['var_skip']),
        bl(a + 0x0c, b['set_var']),         # VAR_CWS_AGREE_SKIP = "true" -> no screen
        bl(a + 0x10, b['get_context']),
        ldr_pc(a + 0x14, 2, p['p_true']),
        movi(1, p['var_agree']),
        bl(a + 0x1c, b['set_var']),         # VAR_I_AGREE_STATUS = "true"
        bl(a + 0x20, b['get_context']),
        ldr_pc(a + 0x24, 2, p['p_true']),
        movi(1, p['var_startup']),
        bl(a + 0x2c, b['set_var']),         # VAR_START_UP_I_AGREE_STATUS = "true"
        bl(a + 0x30, b['get_context']),
        movw(2, p['notify']),
        movi(1, p['ctrl']),
        bl(a + 0x3c, b['exec_fn']),         # TELEMA_NOTIFY_AGREE
        bl(a + 0x40, b['get_context']),
        bl(a + 0x44, b['get_state']),
        movw(1, p['action']),
        bl(a + 0x4c, b['ev_handler']),
        0xe3500000,                         # cmp r0, #0
        bne(a + 0x54, done),
        bl(a + 0x58, b['get_context']),
        bl(a + 0x5c, b['get_state2']),
        movw(1, p['action']),
        bl(a + 0x64, b['ev_handler']),
        0xe3a03001,                         # done: mov r3, #1
        0xe58d3000,                         # str r3, [sp]
        b_(a + 0x70, p['tail']),
    ]
    return b''.join(struct.pack('<I', x) for x in w)


def timer_body(b, p):
    """State 838's entry action, rewritten to arm a timer for its own OK click."""
    a, skip = p['va'], p['va'] + 0x28
    w = [
        bl(a + 0x00, b['get_context']),
        ldr_pc(a + 0x04, 2, p['p_false']),
        movi(1, p['var_startup']),
        bl(a + 0x0c, b['check_var']),       # CheckVariable(628, "false")
        0xe3500000,                         # cmp r0, #0
        beq(a + 0x14, skip),
        bl(a + 0x18, b['get_context']),
        ldr_pc(a + 0x1c, 2, p['p_true']),
        movi(1, p['var_startup']),
        bl(a + 0x24, b['set_var']),
        bl(a + 0x28, b['get_context']),     # skip:
        movw(1, p['event']),
        movw(2, DELAY_MS),
        bl(a + 0x34, b['set_timer']),       # SetEventTimer(SK_SCT_AGREE_CLICK, 2000)
        0xe3a03001,                         # mov r3, #1
        0xe58d3000,                         # str r3, [sp]
        b_(a + 0x40, p['tail']),
        0xe1a00000,                         # nop
        0xe1a00000,
    ]
    return b''.join(struct.pack('<I', x) for x in w)


def lz4_decode(src, want):
    dst = bytearray(); i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                bb = src[i]; i += 1; ll += bb
                if bb != 255: break
        dst += src[i:i + ll]; i += ll
        if i >= n: break
        mo = src[i] | (src[i + 1] << 8); i += 2
        ml = (tok & 15) + 4
        if (tok & 15) == 15:
            while True:
                bb = src[i]; i += 1; ml += bb
                if bb != 255: break
        p = len(dst) - mo
        for k in range(ml): dst.append(dst[p + k])
    assert len(dst) == want, f'decoded {len(dst):,}, want {want:,}'
    return bytes(dst)


def lz4_compress(plain, level=12):
    names = [ctypes.util.find_library('lz4'), 'liblz4.so.1', 'liblz4.dylib', 'liblz4.dll']
    for name in filter(None, names):
        try:
            lib = ctypes.CDLL(name); break
        except OSError:
            continue
    else:
        raise SystemExit('need liblz4')
    lib.LZ4_compressBound.argtypes = [ctypes.c_int]
    lib.LZ4_compressBound.restype = ctypes.c_int
    lib.LZ4_compress_HC.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                    ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.LZ4_compress_HC.restype = ctypes.c_int
    bound = lib.LZ4_compressBound(len(plain))
    buf = ctypes.create_string_buffer(bound)
    n = lib.LZ4_compress_HC(plain, buf, len(plain), bound, level)
    assert n > 0, 'LZ4_compress_HC failed'
    return buf.raw[:n]


def find_mod(f):
    """Offset and name of a nav .mod this script knows, scanning 512-byte boundaries."""
    size = os.fstat(f.fileno()).st_size
    step, off = 1 << 20, 0
    while off < size:
        f.seek(off); blk = f.read(step + 0x200)
        if not blk: break
        for name in BUILDS:
            i = blk.find(name.encode())
            while i >= 0:
                at = off + i - 0x16
                if at >= 0 and at % 512 == 0: return at, name
                i = blk.find(name.encode(), i + 1)
        off += step
    raise SystemExit('no nav image this script knows is in that file')


def loader_sum(m, seg_off, cnt):
    """What the NK2 loader checks the segment-0 header sum against."""
    t = ssum(bytes(m[seg_off:seg_off + 0x20 + cnt * 0x20]))
    p = seg_off + 512
    for k in range(cnt):
        cs = struct.unpack_from('<I', m, seg_off + 0x20 + k * 0x20)[0]
        t = (t + ssum(bytes(m[p:p + cs * 512]))) & 0xffffffff
        p += cs * 512
    return t


def patch_mod(m, b):
    if MODE not in b:
        raise SystemExit(f'{MODE} mode is not mapped for this build yet')
    p = b[MODE]
    make = skip_body if MODE == 'skip' else timer_body
    body_len = len(p['stock']) * 4

    hdr_before = bytes(m[:0x200])
    seg_off = 0x200
    seg_sz = struct.unpack_from('<4I', m, 0x90)[2]
    seg_before = ssum(bytes(m[seg_off:seg_off + seg_sz]))

    cnt = struct.unpack_from('<I', m, seg_off + 4)[0]
    q, uo, ents = seg_off + 512, 0, []
    for k in range(cnt):
        cs, usz, csz, _ = struct.unpack_from('<4I', m, seg_off + 0x20 + k * 0x20)
        ents.append((k, q, cs, usz, csz, uo)); uo += usz; q += cs * 512

    rom_off = b['sec0_file'] + (p['va'] - b['sec0_va'])
    k, cp, cs, usz, csz, ubase = next(e for e in ents if e[5] <= rom_off < e[5] + e[3])
    lo = rom_off - ubase
    print(f'  {MODE} patch at {p["va"]:#x} -> ROM {rom_off:#x} -> chunk {k} +{lo:#x}')

    plain = lz4_decode(bytes(m[cp:cp + csz]), usz)
    want = b''.join(struct.pack('<I', w) for w in p['stock'])
    assert plain[lo:lo + body_len] == want, \
        f'stock body mismatch\n{plain[lo:lo + body_len].hex()}\n{want.hex()}'

    new = bytearray(plain)
    new[lo:lo + body_len] = make(b, p)
    assert len(new) == len(plain), 'patch changed the section size'
    new = bytes(new)
    nsrc = lz4_compress(new)
    assert lz4_decode(nsrc, usz) == new, 'recompressed chunk does not round-trip'
    print(f'  chunk {k}: {csz:,} -> {len(nsrc):,} B  (sectors hold {cs * 512:,}, '
          f'{cs * 512 - len(nsrc):,} spare)')
    assert len(nsrc) <= cs * 512, 'patched chunk no longer fits its sectors'

    m[cp:cp + cs * 512] = nsrc + bytes(cs * 512 - len(nsrc))
    e = seg_off + 0x20 + k * 0x20
    struct.pack_into('<I', m, e + 8, len(nsrc))
    struct.pack_into('<I', m, e + 12, ssum(bytes(m[cp:cp + cs * 512])))

    pad = seg_off + 0x1c                    # reserved zeros between MTCP's fields
    assert not any(m[pad:pad + 4]), 'the MTCP reserved word is not zero'
    drift = (seg_before - ssum(bytes(m[seg_off:seg_off + seg_sz]))) & 0xffffffff
    struct.pack_into('<I', m, pad, drift)
    assert ssum(bytes(m[seg_off:seg_off + seg_sz])) == seg_before, 'segment sum drifted'
    assert loader_sum(m, seg_off, cnt) == seg_before, "loader's sum drifted"
    assert ssum(bytes(m[0x200:])) == struct.unpack_from('<I', m, 0x34)[0], 'SUM4 mismatch'
    assert bytes(m[:0x200]) == hdr_before, 'the 512-byte header changed'
    print(f'  compensating word {drift:#010x} at .mod +{pad:#x}')


def main(src, dst):
    if os.path.exists(dst) and os.path.samefile(src, dst):
        raise SystemExit('refusing to patch the original in place')
    if not os.path.exists(dst):
        # clone where the filesystem can, so a 16 GB card image costs nothing
        if sys.platform != 'darwin' or subprocess.call(
                ['cp', '-c', src, dst], stderr=subprocess.DEVNULL) != 0:
            shutil.copyfile(src, dst)
    print(f'{dst}  ({os.path.getsize(dst):,} B)')

    with open(dst, 'r+b') as f:
        at, name = find_mod(f)
        b = BUILDS[name]
        f.seek(at); m = bytearray(f.read(b['mod_len']))
        assert struct.unpack_from('<I', m, 0)[0] + 0x200 == b['mod_len'], 'unexpected .mod size'
        print(f'  {name} at {at:#x}, {b["mod_len"]:,} B')
        before = bytes(m)
        patch_mod(m, b)
        f.seek(at); f.write(bytes(m))
    pages = sorted({(at + i) // 4096 for i in range(len(m)) if m[i] != before[i]})
    print(f'  {len(pages)} 4 KiB pages changed, '
          f'{pages[0] * 4096:#x}..{pages[-1] * 4096:#x}')
    print(f'  sha256 of the .mod: {hashlib.sha256(bytes(m)).hexdigest()}')


if __name__ == '__main__':
    a = sys.argv[1:]
    if not 2 <= len(a) <= 4: raise SystemExit(__doc__.strip().splitlines()[-1])
    if len(a) > 2:
        if a[2] not in ('skip', 'timer'): raise SystemExit('mode is skip or timer')
        MODE = a[2]
    if len(a) > 3: DELAY_MS = int(a[3])
    main(a[0], a[1])
