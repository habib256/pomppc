#!/usr/bin/env python3
"""ppcanno.py — désassembleur annoté pour les Mach-O PowerPC 32 bits (Tiger).

Outil de rétro-ingénierie du plugin OpenGL (docs/gpu-3d-tiger.md, couche C) :
llvm-objdump désassemble le PPC mais ne résout pas l'adressage PIC
(`bcl 20,31` + `mflr` + `addis`/`addi`), ni les stubs d'import. Ce script le
fait, fonction par fonction, et annote chaque référence avec la chaîne, le
symbole ou l'import visé.

    ppcanno.py BINAIRE                      # tout le binaire
    ppcanno.py BINAIRE _gldCreateContext    # une fonction (ou un préfixe: 'gld*')
    ppcanno.py BINAIRE --xref "IOGLBundleName"   # fonctions qui référencent une chaîne
    ppcanno.py BINAIRE --callers _gldInitDispatch

Dépendance : capstone (pip install capstone).
"""
import struct
import sys

import capstone
from capstone import ppc as cppc

LC_SEGMENT, LC_SYMTAB, LC_DYSYMTAB = 0x1, 0x2, 0xB
S_TYPE = 0xFF
S_NON_LAZY, S_LAZY, S_STUBS = 0x6, 0x7, 0x8
INDIRECT_LOCAL, INDIRECT_ABS = 0x80000000, 0x40000000


class MachO:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        magic = struct.unpack_from(">I", d, 0)[0]
        if magic == 0xCAFEBABE:                       # fat : prendre la tranche ppc
            n = struct.unpack_from(">I", d, 4)[0]
            for i in range(n):
                cpu, sub, off, size, al = struct.unpack_from(">5I", d, 8 + 20 * i)
                if cpu == 18:
                    self.data = d = d[off:off + size]
                    break
            magic = struct.unpack_from(">I", d, 0)[0]
        if magic != 0xFEEDFACE:
            raise SystemExit("pas un Mach-O ppc 32 bits big-endian")
        _, cpu, sub, ftype, ncmds, sizeofcmds, flags = struct.unpack_from(">7I", d, 0)
        self.sections = []          # (seg, sect, addr, size, off, flags, r1, r2)
        self.syms = {}              # addr -> nom
        self.symaddr = {}           # nom -> addr
        self.imports = {}           # adresse (stub ou pointeur) -> nom importé
        symoff = nsyms = stroff = 0
        indoff = nind = 0
        p = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from(">II", d, p)
            if cmd == LC_SEGMENT:
                nsects = struct.unpack_from(">I", d, p + 48)[0]
                q = p + 56
                for _ in range(nsects):
                    sect = d[q:q + 16].rstrip(b"\0").decode()
                    seg = d[q + 16:q + 32].rstrip(b"\0").decode()
                    addr, sz, off, al, rel, nrel, fl, r1, r2 = struct.unpack_from(">9I", d, q + 32)
                    self.sections.append((seg, sect, addr, sz, off, fl, r1, r2))
                    q += 68
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from(">4I", d, p + 8)
            elif cmd == LC_DYSYMTAB:
                vals = struct.unpack_from(">18I", d, p + 8)
                indoff, nind = vals[12], vals[13]
            p += size
        self.symnames = []
        for i in range(nsyms):
            strx, typ, sect, desc, val = struct.unpack_from(">IBBhI", d, symoff + 12 * i)
            e = d.index(b"\0", stroff + strx)
            name = d[stroff + strx:e].decode("latin-1")
            self.symnames.append(name)
            if (typ & 0x0E) == 0x0E and not (typ & 0xE0):   # N_SECT, pas stab
                if val not in self.syms or name.startswith("_"):
                    self.syms[val] = name
                self.symaddr[name] = val
        ind = [struct.unpack_from(">I", d, indoff + 4 * i)[0] for i in range(nind)]
        for seg, sect, addr, sz, off, fl, r1, r2 in self.sections:
            t = fl & S_TYPE
            if t in (S_NON_LAZY, S_LAZY):
                step = 4
            elif t == S_STUBS:
                step = r2
            else:
                continue
            for k in range(sz // step):
                idx = ind[r1 + k] if r1 + k < len(ind) else INDIRECT_LOCAL
                if idx & (INDIRECT_LOCAL | INDIRECT_ABS):
                    continue
                self.imports[addr + k * step] = self.symnames[idx]

    def section_of(self, a):
        for s in self.sections:
            if s[2] <= a < s[2] + s[3]:
                return s
        return None

    def read(self, a, n):
        s = self.section_of(a)
        if not s or s[4] == 0:
            return None
        o = s[4] + (a - s[2])
        return self.data[o:o + n]

    def u32(self, a):
        b = self.read(a, 4)
        return struct.unpack(">I", b)[0] if b and len(b) == 4 else None

    def cstr(self, a):
        s = self.section_of(a)
        if not s or s[4] == 0:
            return None
        o = s[4] + (a - s[2])
        e = self.data.find(b"\0", o, o + 200)
        if e < 0:
            return None
        raw = self.data[o:e]
        if len(raw) < 2 or any(c < 9 or c > 126 for c in raw):
            return None
        return raw.decode("latin-1")

    def describe(self, a):
        """Ce qu'il y a à l'adresse a, en clair."""
        if a in self.imports:
            return "&" + self.imports[a]
        if a in self.syms:
            return self.syms[a]
        s = self.section_of(a)
        if not s:
            return None
        seg, sect = s[0], s[1]
        if sect in ("__cstring", "__const") or "string" in sect:
            t = self.cstr(a)
            if t is not None:
                return repr(t)
        if sect in ("__cfstring",):
            ptr = self.u32(a + 8)
            t = self.cstr(ptr) if ptr else None
            return "@" + repr(t) if t else None
        # plus proche symbole précédent
        best = max((x for x in self.syms if x <= a and self.section_of(x) == s), default=None)
        if best is not None:
            return "%s+0x%x" % (self.syms[best], a - best)
        return "%s,%s+0x%x" % (seg, sect, a - s[2])

    def functions(self):
        text = [s for s in self.sections if s[1] == "__text"][0]
        addrs = sorted(a for a in self.syms if text[2] <= a < text[2] + text[3])
        out = []
        for i, a in enumerate(addrs):
            end = addrs[i + 1] if i + 1 < len(addrs) else text[2] + text[3]
            out.append((self.syms[a], a, end))
        return out


def disasm(m, name, start, end):
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.detail = True
    code = m.read(start, end - start) or b""
    reg = {}                  # registre -> valeur connue (base PIC et dérivés)
    lines = []
    pending_bcl = None
    for ins in md.disasm(code, start):
        note = ""
        mn, ops = ins.mnemonic, ins.op_str
        o = ins.operands
        rn = lambda r: ins.reg_name(r)
        if bytes(ins.bytes) == b"\x42\x9f\x00\x05":   # bcl 20,31,$+4 (capstone : « bdnzl »)
            pending_bcl = ins.address + 4
            mn, ops = "bcl", "20, 31, $+4"
        elif mn == "mflr" and o:
            if pending_bcl is not None:
                reg[rn(o[0].reg)] = pending_bcl
                pending_bcl = None
            else:
                reg.pop(rn(o[0].reg), None)
        elif mn in ("addis", "lis") and len(o) >= 2:
            dst = rn(o[0].reg)
            if mn == "lis":
                reg[dst] = (o[1].imm << 16) & 0xFFFFFFFF
            else:
                src = rn(o[1].reg)
                if src in reg and o[2].type == cppc.PPC_OP_IMM:
                    reg[dst] = (reg[src] + (o[2].imm << 16)) & 0xFFFFFFFF
                else:
                    reg.pop(dst, None)
        elif mn in ("addi", "la", "li", "ori") and len(o) >= 2:
            dst = rn(o[0].reg)
            val = None
            if mn == "li":
                reg.pop(dst, None)
            elif mn == "ori" and len(o) == 3 and rn(o[1].reg) in reg:
                val = reg[rn(o[1].reg)] | (o[2].imm & 0xFFFF)
            elif mn in ("addi", "la"):
                if o[1].type == cppc.PPC_OP_MEM:
                    b = rn(o[1].mem.base)
                    if b in reg:
                        val = (reg[b] + o[1].mem.disp) & 0xFFFFFFFF
                elif len(o) == 3 and rn(o[1].reg) in reg:
                    val = (reg[rn(o[1].reg)] + o[2].imm) & 0xFFFFFFFF
            if val is not None:
                d = m.describe(val)
                note = "= 0x%x %s" % (val, d or "")
                reg[dst] = val
            else:
                reg.pop(dst, None)
        elif mn.startswith(("lwz", "lfs", "lfd", "stw", "lhz", "lbz")) and len(o) == 2 \
                and o[1].type == cppc.PPC_OP_MEM:
            b = rn(o[1].mem.base)
            if b in reg:
                a = (reg[b] + o[1].mem.disp) & 0xFFFFFFFF
                d = m.describe(a)
                note = "[0x%x] %s" % (a, d or "")
                if mn.startswith("lwz"):
                    v = m.u32(a)
                    if v is not None and m.section_of(v):
                        dv = m.describe(v)
                        if dv:
                            note += " -> %s" % dv
            if mn.startswith(("lwz", "lhz", "lbz")):
                reg.pop(rn(o[0].reg), None)
        elif mn in ("bl", "b", "bla") and o and o[0].type == cppc.PPC_OP_IMM:
            t = o[0].imm & 0xFFFFFFFF
            note = m.imports.get(t) or m.syms.get(t) or ""
            if mn == "bl":
                for r in ("r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r0"):
                    reg.pop(r, None)
        elif o and o[0].type == cppc.PPC_OP_REG and mn not in ("mtctr", "mtlr", "cmpw", "cmplw", "cmpwi", "cmplwi"):
            reg.pop(rn(o[0].reg), None)
        lines.append("  %08x  %-8s %-28s %s" % (ins.address, mn, ops, ("; " + note) if note else ""))
    return lines


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    m = MachO(sys.argv[1])
    args = sys.argv[2:]
    funcs = m.functions()
    if args and args[0] in ("--xref", "--callers"):
        needle = args[1]
        for name, a, e in funcs:
            body = disasm(m, name, a, e)
            hits = [l for l in body if needle in l.split(";", 1)[-1]] if True else []
            if args[0] == "--callers":
                hits = [l for l in body if (" bl " in l or l.split()[1] in ("bl", "b")) and needle in l]
            for h in hits:
                print("%-40s %s" % (name, h.strip()))
        return 0
    want = args[0] if args else None
    for name, a, e in funcs:
        if want:
            if want.endswith("*"):
                if not name.startswith(want[:-1]):
                    continue
            elif name != want:
                continue
        print("\n%s:  ; 0x%x-0x%x" % (name, a, e))
        print("\n".join(disasm(m, name, a, e)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
