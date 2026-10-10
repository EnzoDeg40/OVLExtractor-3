#!/usr/bin/env python3
"""Re-encode an RCT3 language OVL pair (gui/<Language>.common.ovl + .unique.ovl)
with new strings — any length — from a JSON {"Symbol": "text", …}.

    python3 scripts/ovl_text_pack.py TEMPLATE.common.ovl strings.json OUT.common.ovl
    python3 scripts/ovl_text_pack.py --roundtrip TEMPLATE.common.ovl      # self-check

The template gives the symbol set (names, hashes, loader order); the JSON
overrides the text of any of them (symbols it does not name keep the
template's text, symbols it adds are rejected). The output keeps the OVL v5
layout of the template — string table, text blocks of ~16 KB, symbol table,
loader references, relocations, extra-data records — and drops the trailing
"FilesAndLines" source-map, which the game ignores (docs/RCT3_OVL_FORMAT.md
§18.1). The .unique.ovl is rewritten next to it, tail-less too.

Standard library only.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

BLOCK_TARGET = 16384  # a text block is closed once it reaches this many bytes


class Reader:
    def __init__(self, data: bytes):
        self.b, self.o = data, 0

    def u8(self):  v = self.b[self.o]; self.o += 1; return v
    def u16(self): v = struct.unpack_from('<H', self.b, self.o)[0]; self.o += 2; return v
    def u32(self): v = struct.unpack_from('<I', self.b, self.o)[0]; self.o += 4; return v
    def bytes(self, n): v = self.b[self.o:self.o + n]; self.o += n; return v


class LanguageOvl:
    """The parts of a v5 text OVL side that the writer regenerates or copies."""

    def __init__(self, data: bytes):
        r = Reader(data)
        if r.u32() != 0x4B524746: raise SystemExit('not an OVL')
        if r.u32() != 2 or r.u32() != 5: raise SystemExit('only header v2 / OVL v5 files are handled')
        if r.u32() != 16: raise SystemExit('unexpected header')
        self.subversion = r.u32()
        if self.subversion != 0: raise SystemExit('subversion 1 files are not handled')
        if r.u32() != 0: raise SystemExit('files with references are not handled')
        if r.u32() != 0: raise SystemExit('unexpected loader prelude')
        nloaders = r.u32()
        self.loaders = []
        for _ in range(nloaders):
            name = r.bytes(r.u16()); desc = r.bytes(r.u16()); typ = r.u32(); tag = r.bytes(r.u16())
            self.loaders.append((name, desc, typ, tag))
        if [l[3] for l in self.loaders] != [b'txt']: raise SystemExit('not a text-only OVL')
        self.loader_extra = [(r.u32(), r.u32()) for _ in range(nloaders)]
        self.chunks = []
        for _ in range(9):
            n = r.u32(); n2 = r.u32()
            if n2 != n: raise SystemExit('chunk header mismatch')
            self.chunks.append([r.u32() for _ in range(n)])
        # `temp` bytes follow: a record count, then the loader references that
        # own extra data ({u32 index, u16 0xFFFF, u16 1, u32 1, u16 1} each)
        temp = r.u32()
        self.extrastruct = r.bytes(temp) if temp >= 4 else b''
        after = r.u32()
        self.after_blocks = [r.u32() for _ in range(after)]
        # block data, in chunk order; internal offsets accumulate over the pair
        self.blocks = []
        off = 0
        for ci, sizes in enumerate(self.chunks):
            for sz in sizes:
                self.blocks.append((ci, off, r.bytes(sz)))
                off += sz
        self.total = off
        nrel = r.u32()
        self.relocations = [r.u32() for _ in range(nrel)]
        # loader references: chunk 3 (index 2) block 1
        c3 = [blk for blk in self.blocks if blk[0] == 2]
        self.symbol_table_off, sym = c3[0][1], c3[0][2]
        self.loader_refs_off, lrs = c3[1][1], c3[1][2]
        self.symbols = [struct.unpack_from('<IIHHI', sym, i) for i in range(0, len(sym), 16)]
        self.loader_refs = [struct.unpack_from('<IIIII', lrs, i) for i in range(0, len(lrs), 20)]
        self.extra = []
        for lr in self.loader_refs:
            count = lr[2] & 0xFFFF
            self.extra.append([r.bytes(r.u32()) for _ in range(count)])
        self.extradata_end = r.o
        # chunk 1 (index 0): block 0 = symbol-name string table, the rest = UTF-16 texts
        c1 = [blk for blk in self.blocks if blk[0] == 0]
        self.names = c1[0][2]
        self.text_blocks = c1[1:]

    # -- decoded view ---------------------------------------------------------
    def name_at(self, off: int) -> str:
        end = self.names.index(b'\0', off)
        return self.names[off:end].decode('latin1')

    def text_at(self, off: int) -> str:
        for _ci, boff, data in self.text_blocks:
            if boff <= off < boff + len(data):
                i = off - boff
                j = i
                while data[j:j + 2] != b'\0\0': j += 2
                return data[i:j].decode('utf-16-le')
        raise KeyError(hex(off))

    def strings(self) -> dict[str, str]:
        out = {}
        for sp, dp, *_ in self.symbols:
            out[self.name_at(sp).rsplit(':', 1)[0]] = self.text_at(dp)
        return out


def pack(t: LanguageOvl, texts: dict[str, str]) -> bytes:
    """Serialise a copy of template `t` whose texts are replaced by `texts`."""
    # 1. text blocks: the template's data order, strings re-encoded
    order = sorted(range(len(t.symbols)), key=lambda i: t.symbols[i][1])
    blocks, cur, new_dp = [], bytearray(), {}
    names_size = len(t.names)
    for i in order:
        sp, dp, *_ = t.symbols[i]
        sym = t.name_at(sp).rsplit(':', 1)[0]
        s = texts.get(sym, t.text_at(dp))
        enc = s.encode('utf-16-le') + b'\0\0'
        if len(cur) >= BLOCK_TARGET:
            blocks.append(bytes(cur)); cur = bytearray()
        new_dp[i] = names_size + sum(len(b) for b in blocks) + len(cur)
        cur += enc
    if cur: blocks.append(bytes(cur))
    c1_sizes = [names_size] + [len(b) for b in blocks]
    c1_total = sum(c1_sizes)

    # 2. symbol table and loader references with shifted pointers
    delta = c1_total - (t.symbol_table_off)  # chunk 3 starts right after chunk 1 (chunk 2 is empty)
    sym_off = c1_total
    lr_off = sym_off + 16 * len(t.symbols)
    sym = bytearray()
    for i, (sp, dp, isptr, lptr, h) in enumerate(t.symbols):
        sym += struct.pack('<IIHHI', sp, new_dp[i], isptr, lptr, h)
    old_sym_index = {t.symbol_table_off + 16 * i: i for i in range(len(t.symbols))}
    lrs = bytearray()
    for ln, dp, extra, ssp, nres in t.loader_refs:
        i = old_sym_index[ssp]
        lrs += struct.pack('<IIIII', ln, new_dp[i], extra, sym_off + 16 * i, nres)
    relocs = []
    for i in range(len(t.symbols)):
        relocs += [sym_off + 16 * i, sym_off + 16 * i + 4]
    for j in range(len(t.loader_refs)):
        relocs += [lr_off + 20 * j + 4, lr_off + 20 * j + 12]
    relocs.sort()

    # 3. serialise
    out = bytearray()
    out += struct.pack('<IIIIIII', 0x4B524746, 2, 5, 16, 0, 0, 0)
    out += struct.pack('<I', len(t.loaders))
    for name, desc, typ, tag in t.loaders:
        out += struct.pack('<H', len(name)) + name + struct.pack('<H', len(desc)) + desc
        out += struct.pack('<I', typ) + struct.pack('<H', len(tag)) + tag
    for a, b in t.loader_extra:
        out += struct.pack('<II', a, b)
    chunk_sizes = [c1_sizes, [], [len(sym), len(lrs)]] + [[] for _ in range(6)]
    for sizes in chunk_sizes:
        out += struct.pack('<II', len(sizes), len(sizes))
        for sz in sizes: out += struct.pack('<I', sz)
    out += struct.pack('<I', len(t.extrastruct)) + t.extrastruct
    out += struct.pack('<I', len(t.after_blocks))
    for v in t.after_blocks: out += struct.pack('<I', v)
    out += t.names
    for b in blocks: out += b
    out += sym + lrs
    out += struct.pack('<I', len(relocs))
    for v in relocs: out += struct.pack('<I', v)
    for chunks in t.extra:
        for ch in chunks: out += struct.pack('<I', len(ch)) + ch
    return bytes(out)


def unique_side(path_common: str) -> bytes:
    """The template's .unique.ovl without its FilesAndLines tail."""
    p = path_common.replace('.common.ovl', '.unique.ovl')
    data = open(p, 'rb').read()
    r = Reader(data)
    r.o = 0x10
    if r.u32() != 0 or r.u32() != 0 or r.u32() != 0: raise SystemExit('unexpected unique header')
    n = r.u32()
    for _ in range(n):
        r.bytes(r.u16()); r.bytes(r.u16()); r.u32(); r.bytes(r.u16())
    for _ in range(n): r.u32(); r.u32()
    for _ in range(9):
        k = r.u32(); r.u32()
        for _ in range(k): r.u32()          # sizes (all 0 in a language file)
    temp = r.u32(); r.bytes(temp)
    after = r.u32(); r.bytes(4 * after)
    nrel = r.u32(); r.bytes(4 * nrel)
    return data[:r.o]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('template', help='<Language>.common.ovl to copy the structure from')
    ap.add_argument('strings', nargs='?', help='JSON {"Symbol": "text"} (what --types text writes)')
    ap.add_argument('out', nargs='?', help='output <Name>.common.ovl (the .unique.ovl goes next to it)')
    ap.add_argument('--roundtrip', action='store_true', help='re-pack the template unchanged and compare')
    a = ap.parse_args()

    data = open(a.template, 'rb').read()
    t = LanguageOvl(data)
    if a.roundtrip:
        packed = pack(t, {})
        ref = data[:t.extradata_end]
        again = LanguageOvl(packed)
        same_strings = again.strings() == t.strings() and again.symbols[0][4] == t.symbols[0][4]
        same_bytes = packed == ref
        print(f'{len(t.symbols)} symbols, {len(t.text_blocks)} text blocks -> {len(again.text_blocks)}, '
              f'packed {len(packed)} B, template without tail {len(ref)} B: '
              f'{"byte-identical" if same_bytes else "same strings, blocks split differently" if same_strings else "DIFFERENT"}')
        return 0 if same_strings else 1
    if not a.strings or not a.out:
        ap.error('strings.json and OUT.common.ovl are required (or --roundtrip)')
    with open(a.strings, encoding='utf-8') as fh:
        texts = json.load(fh)
    known = t.strings()
    unknown = sorted(set(texts) - set(known))
    if unknown:
        raise SystemExit(f'{len(unknown)} symbol(s) not in the template (new symbols are not supported): '
                         + ', '.join(unknown[:10]))
    packed = pack(t, texts)
    if not a.out.endswith('.common.ovl'):
        ap.error('OUT must end with .common.ovl')
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, 'wb').write(packed)
    open(a.out.replace('.common.ovl', '.unique.ovl'), 'wb').write(unique_side(a.template))
    changed = sum(1 for k, v in texts.items() if known.get(k) != v)
    print(f'wrote {a.out} ({len(packed)} B, {len(t.symbols)} strings, {changed} changed) + .unique.ovl')
    return 0


if __name__ == '__main__':
    sys.exit(main())
