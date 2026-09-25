#!/usr/bin/env python3
"""ppcmix.py FICHIER [T0 T1] [--top N] — lit la sortie du greffon ppcmix.

Prend la différence des instantanés les plus proches de T0 et T1 (secondes
depuis le démarrage de QEMU ; par défaut le premier et le dernier), nomme les
clés d'opcode et classe chaque instruction selon la façon dont QEMU 9.2 la
traduit (relevé statique de docs/tcg-g4.md) :

  inline   ops TCG / gvec en ligne dans le code généré
  helper   un appel de fonction C par instruction exécutée
  fp       flottant scalaire (helpers softfloat, x-fast-fp)
  sortie   termine le bloc (branchement indirect → helper_lookup_tb_ptr,
           rfi, sc, isync, mtmsr…)

Imprime : total, instructions/s, part AltiVec, part helper, par classe, et les
N clés les plus exécutées (banque « commpage » à part)."""
import sys, collections

# ---------------------------------------------------------------------------
# Noms (G4 = PowerPC 7400/7410 : 32 bits + AltiVec) et classe de traduction.
# clé = primaire << 11 | sous-code (voir key_of dans ppcmix.c)
H, I, F, X = "helper", "inline", "fp", "sortie"
NAMES = {}
def k(op, sub=0):
    return op << 11 | sub
def add(op, sub, name, cls):
    NAMES[k(op, sub)] = (name, cls)

PRIM = {3: ("twi", I), 7: ("mulli", I), 8: ("subfic", I), 10: ("cmpli", I), 11: ("cmpi", I),
        12: ("addic", I), 13: ("addic.", I), 14: ("addi", I), 15: ("addis", I),
        16: ("bc", I), 17: ("sc", X), 18: ("b", I), 20: ("rlwimi", I), 21: ("rlwinm", I),
        23: ("rlwnm", I), 24: ("ori", I), 25: ("oris", I), 26: ("xori", I), 27: ("xoris", I),
        28: ("andi.", I), 29: ("andis.", I), 32: ("lwz", I), 33: ("lwzu", I), 34: ("lbz", I),
        35: ("lbzu", I), 36: ("stw", I), 37: ("stwu", I), 38: ("stb", I), 39: ("stbu", I),
        40: ("lhz", I), 41: ("lhzu", I), 42: ("lha", I), 43: ("lhau", I), 44: ("sth", I),
        45: ("sthu", I), 46: ("lmw", H), 47: ("stmw", H),
        48: ("lfs", H), 49: ("lfsu", H), 50: ("lfd", I), 51: ("lfdu", I),
        52: ("stfs", H), 53: ("stfsu", H), 54: ("stfd", I), 55: ("stfdu", I)}
for op, (n, c) in PRIM.items():
    add(op, 0, n, c)

OP19 = {0: ("mcrf", I), 16: ("bclr", X), 528: ("bcctr", X), 33: ("crnor", I), 50: ("rfi", X),
        129: ("crandc", I), 150: ("isync", X), 193: ("crxor", I), 225: ("crnand", I),
        257: ("crand", I), 289: ("creqv", I), 417: ("crorc", I), 449: ("cror", I)}
for sub, (n, c) in OP19.items():
    add(19, sub, n, c)

# XO-form (bit OE = 0x200 du xo sur 10 bits) : même traduction avec ou sans OE
XO = {8: "subfc", 10: "addc", 11: "mulhwu", 40: "subf", 75: "mulhw", 104: "neg", 136: "subfe",
      138: "adde", 200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw",
      266: "add", 459: "divwu", 491: "divw"}
OP31 = {0: ("cmp", I), 4: ("tw", I), 19: ("mfcr", I), 20: ("lwarx", I), 23: ("lwzx", I),
        24: ("slw", I), 26: ("cntlzw", I), 28: ("and", I), 32: ("cmpl", I), 54: ("dcbst", I),
        55: ("lwzux", I), 60: ("andc", I), 83: ("mfmsr", I), 86: ("dcbf", I), 87: ("lbzx", I),
        119: ("lbzux", I), 124: ("nor", I), 144: ("mtcrf", I), 146: ("mtmsr", X),
        150: ("stwcx.", I), 151: ("stwx", I), 183: ("stwux", I), 210: ("mtsr", H),
        215: ("stbx", I), 242: ("mtsrin", H), 246: ("dcbtst", I), 247: ("stbux", I),
        278: ("dcbt", I), 279: ("lhzx", I), 284: ("eqv", I), 306: ("tlbie", H),
        310: ("eciwx", H), 311: ("lhzux", I), 316: ("xor", I), 343: ("lhax", I),
        371: ("mftb", H), 375: ("lhaux", I), 407: ("sthx", I), 412: ("orc", I),
        439: ("sthux", I), 444: ("or", I), 470: ("dcbi", I), 476: ("nand", I),
        512: ("mcrxr", I), 533: ("lswx", H), 534: ("lwbrx", I), 535: ("lfsx", H),
        536: ("srw", I), 566: ("tlbsync", I), 567: ("lfsux", H), 595: ("mfsr", H),
        597: ("lswi", H), 598: ("sync", I), 599: ("lfdx", I), 631: ("lfdux", I),
        659: ("mfsrin", H), 661: ("stswx", H), 662: ("stwbrx", I), 663: ("stfsx", H),
        695: ("stfsux", H), 725: ("stswi", H), 727: ("stfdx", I), 759: ("stfdux", I),
        790: ("lhbrx", I), 792: ("sraw", H), 824: ("srawi", I), 854: ("eieio", I),
        918: ("sthbrx", I), 922: ("extsh", I), 954: ("extsb", I), 982: ("icbi", H),
        983: ("stfiwx", I), 1014: ("dcbz", H),
        # AltiVec, mémoire
        6: ("lvsl", I), 38: ("lvsr", I), 7: ("lvebx", H), 39: ("lvehx", H), 71: ("lvewx", H),
        103: ("lvx", I), 359: ("lvxl", I), 135: ("stvebx", H), 167: ("stvehx", H),
        199: ("stvewx", H), 231: ("stvx", I), 487: ("stvxl", I), 342: ("dst", I),
        374: ("dstst", I), 822: ("dss", I)}
for sub, (n, c) in OP31.items():
    add(31, sub, n, c)
for sub, n in XO.items():
    add(31, sub, n, I)
    add(31, sub | 0x200, n + "o", I)

# mtspr (primaire 0) / mfspr (primaire 1) par SPR
SPR = {1: "xer", 8: "lr", 9: "ctr", 18: "dsisr", 19: "dar", 22: "dec", 25: "sdr1",
       26: "srr0", 27: "srr1", 256: "vrsave", 268: "tbl", 269: "tbu", 272: "sprg0",
       273: "sprg1", 274: "sprg2", 275: "sprg3", 282: "ear", 284: "tbl(w)", 285: "tbu(w)",
       287: "pvr", 1008: "hid0", 1009: "hid1", 1013: "dabr", 1017: "l2cr", 1019: "ictc",
       1020: "thrm1", 1021: "thrm2", 1022: "thrm3", 1023: "pir", 952: "mmcr0", 953: "pmc1",
       954: "pmc2", 956: "mmcr1", 957: "pmc3", 958: "pmc4", 959: "sia", 1014: "msscr0"}
for i in range(4):
    SPR[528 + 2 * i] = "ibat%du" % i; SPR[529 + 2 * i] = "ibat%dl" % i
    SPR[536 + 2 * i] = "dbat%du" % i; SPR[537 + 2 * i] = "dbat%dl" % i
def spr_cls(write, spr):
    if spr in (1, 8, 9):
        return I
    if spr in (22, 268, 269, 284, 285) or 528 <= spr <= 543 or spr == 25:
        return H
    if spr == 1008 and write:
        return H
    return I
for spr in range(1024):
    n = SPR.get(spr, "spr%d" % spr)
    add(0, spr, "mt" + n, spr_cls(True, spr))
    add(1, spr, "mf" + n, spr_cls(False, spr))

# flottant scalaire
OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls",
        28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
for sub, n in OP59.items():
    add(59, sub, n, F)
OP63A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
         28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
for sub, n in OP63A.items():
    add(63, 0x400 | sub, n, F)
OP63X = {0: ("fcmpu", F), 12: ("frsp", F), 14: ("fctiw", F), 15: ("fctiwz", F), 32: ("fcmpo", F),
         38: ("mtfsb1", F), 40: ("fneg", I), 64: ("mcrfs", F), 70: ("mtfsb0", F), 72: ("fmr", I),
         134: ("mtfsfi", F), 136: ("fnabs", I), 264: ("fabs", I), 583: ("mffs", F), 711: ("mtfsf", F)}
for sub, (n, c) in OP63X.items():
    add(63, sub, n, c)

# AltiVec (primaire 4). Classes d'après target/ppc/translate/vmx-impl.c.inc (9.2).
V = {}
def v(sub, n, c): V[sub] = (n, c)
for n, s in (("vaddubm", 0), ("vadduhm", 64), ("vadduwm", 128), ("vsububm", 1024),
             ("vsubuhm", 1088), ("vsubuwm", 1152)):
    v(s, n, I)                                            # gvec add/sub
for n, s in (("vaddubs", 512), ("vadduhs", 576), ("vadduws", 640), ("vaddsbs", 768),
             ("vaddshs", 832), ("vaddsws", 896), ("vsububs", 1536), ("vsubuhs", 1600),
             ("vsubuws", 1664), ("vsubsbs", 1792), ("vsubshs", 1856), ("vsubsws", 1920)):
    v(s, n, I)                                            # gvec saturé + VSCR[SAT]
v(384, "vaddcuw", I); v(1408, "vsubcuw", I)
for n, s in (("vmaxub", 2), ("vmaxuh", 66), ("vmaxuw", 130), ("vmaxsb", 258), ("vmaxsh", 322),
             ("vmaxsw", 386), ("vminub", 514), ("vminuh", 578), ("vminuw", 642),
             ("vminsb", 770), ("vminsh", 834), ("vminsw", 898)):
    v(s, n, I)
for n, s in (("vavgub", 1026), ("vavguh", 1090), ("vavguw", 1154), ("vavgsb", 1282),
             ("vavgsh", 1346), ("vavgsw", 1410)):
    v(s, n, I)
for n, s in (("vrlb", 4), ("vrlh", 68), ("vrlw", 132), ("vslb", 260), ("vslh", 324),
             ("vslw", 388), ("vsrb", 516), ("vsrh", 580), ("vsrw", 644), ("vsrab", 772),
             ("vsrah", 836), ("vsraw", 900)):
    v(s, n, I)
v(452, "vsl", I); v(708, "vsr", I)                        # trans_vsl/vsr : TCG en ligne
v(1036, "vslo", H); v(1100, "vsro", H)
for n, s in (("vand", 1028), ("vandc", 1092), ("vor", 1156), ("vxor", 1220), ("vnor", 1284)):
    v(s, n, I)
v(1540, "mfvscr", H); v(1604, "mtvscr", H)
for n, s in (("vmuloub", 8), ("vmulouh", 72), ("vmulosb", 264), ("vmulosh", 328),
             ("vmuleub", 520), ("vmuleuh", 584), ("vmulesb", 776), ("vmulesh", 840)):
    v(s, n, H)
for n, s in (("vsum4ubs", 1544), ("vsum4sbs", 1800), ("vsum4shs", 1608), ("vsum2sws", 1672),
             ("vsumsws", 1928)):
    v(s, n, H)
for n, s in (("vaddfp", 10), ("vsubfp", 74), ("vrefp", 266), ("vrsqrtefp", 330),
             ("vexptefp", 394), ("vlogefp", 458), ("vrfin", 522), ("vrfiz", 586),
             ("vrfip", 650), ("vrfim", 714), ("vcfux", 778), ("vcfsx", 842), ("vctuxs", 906),
             ("vctsxs", 970), ("vmaxfp", 1034), ("vminfp", 1098)):
    v(s, n, H)                                            # env + softfloat, 4 éléments
for n, s in (("vmrghb", 12), ("vmrghh", 76), ("vmrghw", 140), ("vmrglb", 268),
             ("vmrglh", 332), ("vmrglw", 396)):
    v(s, n, H)
for n, s in (("vspltb", 524), ("vsplth", 588), ("vspltw", 652), ("vspltisb", 780),
             ("vspltish", 844), ("vspltisw", 908)):
    v(s, n, I)                                            # gvec dup
for n, s in (("vpkuhum", 14), ("vpkuwum", 78), ("vpkuhus", 142), ("vpkuwus", 206),
             ("vpkshus", 270), ("vpkswus", 334), ("vpkshss", 398), ("vpkswss", 462),
             ("vpkpx", 782)):
    v(s, n, H)
for n, s in (("vupkhsb", 526), ("vupkhsh", 590), ("vupklsb", 654), ("vupklsh", 718),
             ("vupkhpx", 846), ("vupklpx", 974)):
    v(s, n, H)
for n, s in (("vcmpequb", 6), ("vcmpequh", 70), ("vcmpequw", 134), ("vcmpgtub", 518),
             ("vcmpgtuh", 582), ("vcmpgtuw", 646), ("vcmpgtsb", 774), ("vcmpgtsh", 838),
             ("vcmpgtsw", 902)):
    v(s, n, I)                                            # gvec cmp (+ CR6 en ligne)
for n, s in (("vcmpeqfp", 198), ("vcmpgefp", 454), ("vcmpgtfp", 710), ("vcmpbfp", 966)):
    v(s, n, H)
VA = {32: ("vmhaddshs", H), 33: ("vmhraddshs", H), 34: ("vmladduhm", H), 36: ("vmsumubm", H),
      37: ("vmsummbm", H), 38: ("vmsumuhm", H), 39: ("vmsumuhs", H), 40: ("vmsumshm", H),
      41: ("vmsumshs", H), 42: ("vsel", I), 43: ("vperm", H), 44: ("vsldoi", H),
      46: ("vmaddfp", H), 47: ("vnmsubfp", H)}
for sub, (n, c) in V.items():
    add(4, sub, n, c)
for sub, (n, c) in VA.items():
    add(4, 0x400 | sub, n, c)

ALTIVEC_MEM = {"lvsl", "lvsr", "lvebx", "lvehx", "lvewx", "lvx", "lvxl", "stvebx", "stvehx",
               "stvewx", "stvx", "stvxl", "dst", "dstst", "dss", "mtvrsave", "mfvrsave"}

def name_of(key):
    if key in NAMES:
        return NAMES[key]
    return ("op%d/%d" % (key >> 11, key & 0x7ff), "?")

def is_altivec(key, name):
    return (key >> 11) == 4 or name in ALTIVEC_MEM

# ---------------------------------------------------------------------------
def load(path):
    snaps, cur = [], None
    for l in open(path):
        p = l.split()
        if not p:
            continue
        if p[0] == "T":
            cur = [float(p[1]), int(p[2]), {}]
            snaps.append(cur)
        elif p[0] == "END":
            continue
        elif cur is not None:
            cur[2][(int(p[0]), int(p[1]))] = int(p[2])
    return snaps

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 40
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args.remove(str(top))
    snaps = load(args[0])
    if len(snaps) < 2:
        sys.exit("moins de deux instantanés")
    if len(args) >= 3:
        t0, t1 = float(args[1]), float(args[2])
        a = min(snaps, key=lambda s: abs(s[0] - t0))
        b = min(snaps, key=lambda s: abs(s[0] - t1))
    else:
        a, b = snaps[0], snaps[-1]
    dt = b[0] - a[0]
    d = collections.Counter()
    for kk, vv in b[2].items():
        x = vv - a[2].get(kk, 0)
        if x:
            d[kk] = x
    tot = sum(d.values())
    print("fenêtre %.1f → %.1f s (%.1f s) ; %d instructions (%.1f M/s) ; %d traductions (%.0f/s)"
          % (a[0], b[0], dt, tot, tot / dt / 1e6, b[1] - a[1], (b[1] - a[1]) / dt))
    cls = collections.Counter(); alt = collections.Counter(); banks = collections.Counter()
    per = collections.Counter()
    for (bank, key), x in d.items():
        name, c = name_of(key)
        cls[c] += x
        banks[bank] += x
        per[(name, c, bank)] += x
        if is_altivec(key, name):
            alt[c] += x
    print("commpage : %.2f %%" % (100.0 * banks[1] / tot))
    print("par classe : " + ", ".join("%s %.2f %%" % (c, 100.0 * x / tot)
                                      for c, x in cls.most_common()))
    at = sum(alt.values())
    print("AltiVec : %.3f %% des instructions (%s)" % (100.0 * at / tot, ", ".join(
        "%s %.3f %%" % (c, 100.0 * x / tot) for c, x in alt.most_common())))
    print("\n%-14s %-7s %-4s %14s %8s %10s" % ("instr.", "classe", "bnq", "compte", "%", "M/s"))
    for (name, c, bank), x in per.most_common(top):
        print("%-14s %-7s %-4s %14d %7.3f%% %10.3f" % (name, c, "cp" if bank else "", x,
                                                     100.0 * x / tot, x / dt / 1e6))
    print("\nhelpers, par instruction :")
    hl = collections.Counter()
    for (name, c, bank), x in per.items():
        if c == H:
            hl[name] += x
    for name, x in hl.most_common(25):
        print("  %-12s %7.3f %%  %8.0f /s" % (name, 100.0 * x / tot, x / dt))
    print("\nAltiVec, par instruction :")
    al = collections.Counter()
    for (bank, key), x in d.items():
        name, c = name_of(key)
        if is_altivec(key, name):
            al[(name, c)] += x
    for (name, c), x in al.most_common(30):
        print("  %-12s %-7s %7.4f %%  %10.0f /s" % (name, c, 100.0 * x / tot, x / dt))

main()
