#!/usr/bin/env python3
"""Extract Clarion debug-print entry points from Windows CE kernel XIP images."""
import argparse
import re
import struct
import sys

E32_EXP = 0x24
WANTED = {"NKDbgPrintfW", "NKvDbgPrintfW", "OutputDebugStringW"}


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def xip_regions(data):
    for match in re.finditer(b"ECEC", data):
        base = match.start() - 0x40
        if base < 0:
            continue
        romhdr_va = u32(data, base + 0x44)
        for physfirst in range(0x80000000, 0x90000000, 0x10000):
            romhdr = romhdr_va - physfirst + base
            if not (base <= romhdr < len(data) - 0x100):
                continue
            if u32(data, romhdr + 8) != physfirst:
                continue
            count = u32(data, romhdr + 0x10)
            if 0 < count < 2000 and physfirst < u32(data, romhdr + 0xc):
                yield data, base, physfirst, romhdr, count
                break


def file_offset(xip, va):
    data, base, physfirst, _, _ = xip
    off = va - physfirst + base
    return off if 0 <= off < len(data) else None


def module_exports(xip, module_name):
    data, _, _, romhdr, count = xip
    toc = romhdr + 0x54
    for i in range(count):
        ent = toc + i * 0x20
        name_off = file_offset(xip, u32(data, ent + 0x10))
        if name_off is None:
            continue
        end = data.find(b"\0", name_off)
        if end < 0:
            continue
        name = data[name_off:end].decode("ascii", "replace")
        if name.lower() != module_name.lower():
            continue
        e32_va = u32(data, ent + 0x14)
        e32 = file_offset(xip, e32_va)
        o32 = file_offset(xip, u32(data, ent + 0x18))
        if e32 is None or o32 is None:
            raise ValueError("invalid coredll XIP table")
        nsec = struct.unpack_from("<H", data, e32)[0]
        sections = []
        for j in range(nsec):
            vsize, rva, psize, dataptr, _, flags = struct.unpack_from(
                "<6I", data, o32 + j * 0x18)
            sections.append((rva, max(vsize, psize), psize, dataptr,
                             bool(flags & 0x2000)))
        vbase = u32(data, e32 + 8)
        exp_rva, _ = struct.unpack_from("<2I", data, e32 + E32_EXP)

        def rva_offset(rva, size=1):
            for sec_rva, span, psize, dataptr, compressed in sections:
                if sec_rva <= rva and rva + size <= sec_rva + psize:
                    if compressed:
                        return None
                    return file_offset(xip, dataptr) + rva - sec_rva
            return None

        exp = rva_offset(exp_rva, 40)
        if exp is None:
            raise ValueError("coredll export directory is not in raw XIP")
        (_, _, _, _, name_rva, ordinal_base, nfunc, nname,
         funcs_rva, names_rva, ords_rva) = struct.unpack_from(
             "<IIHHIIIIIII", data, exp)
        funcs = rva_offset(funcs_rva, nfunc * 4)
        names = rva_offset(names_rva, nname * 4)
        ords = rva_offset(ords_rva, nname * 2)
        if None in (funcs, names, ords):
            raise ValueError("coredll export arrays are not in raw XIP")
        named = {}
        for j in range(nname):
            nrva = u32(data, names + j * 4)
            noff = rva_offset(nrva)
            if noff is not None:
                end = data.find(b"\0", noff)
                named[struct.unpack_from("<H", data, ords + j * 2)[0]] = (
                    data[noff:end].decode("ascii", "replace"))
        found = {}
        for j in range(nfunc):
            rva = u32(data, funcs + j * 4)
            name = named.get(j)
            if name in WANTED and rva:
                found[name] = vbase + rva
        if found.keys() != WANTED:
            raise ValueError("coredll is missing one or more required exports")
        return found
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nor", help="QY7 or QY8 NAND/NOR image")
    parser.add_argument("sd", nargs="?", help="optional QY7 SD image")
    args = parser.parse_args()
    paths = ([args.sd] if args.sd else []) + [args.nor]
    symbols = set()
    for path in paths:
        with open(path, "rb") as image:
            data = image.read()
        for xip in xip_regions(data):
            try:
                exports = module_exports(xip, "coredll.dll")
            except ValueError as exc:
                print(f"{path}: {exc}", file=sys.stderr)
                continue
            if exports:
                symbols.update((address, name)
                               for name, address in exports.items())
    if not symbols:
        parser.error("no coredll.dll with required exports found in supplied images")
    for address, name in sorted(symbols):
        print(f"{address:08x} {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
