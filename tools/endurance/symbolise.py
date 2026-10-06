# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""symbolise.py — adresse du noyau de Tiger → symbole, contre `mach_kernel` et
les kexts chargés.

Les fichiers viennent de l'invité (voir docs/endurance.md) et vivent hors
dépôt, dans bench/endurance/invite/ :
  mach_kernel                    copie de /mach_kernel (tssh.sh 'cat /mach_kernel')
  endurance-syms/*.sym           kextload -n -s -A (tools/endurance/invite-syms.sh)
  endurance-syms/kextstat.txt    adresses du démarrage où les .sym ont été liés

Un kext n'est pas chargé à la même adresse d'un démarrage à l'autre (l'ordre
de chargement dépend de l'appariement IOKit, qui court en SMP). On traduit
donc par DÉCALAGE : décalage = adresse − base du kext au démarrage de
l'incident (liste `kmod` lue dans la mémoire de l'invité, ou kextstat du
démarrage), puis symbole à base_de_référence + décalage dans le .sym.

    python3 tools/endurance/symbolise.py 0x468abc 0x2c1f0 [--kmods kmods.txt]
"""
import bisect
import os
import re
import subprocess
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(os.path.dirname(ICI))
sys.path.insert(0, os.path.join(WT, "tools", "re"))
import machonm  # noqa: E402


def depot_principal():
    try:
        d = subprocess.run(["git", "-C", WT, "rev-parse", "--path-format=absolute",
                            "--git-common-dir"], capture_output=True, text=True).stdout.strip()
        if d.endswith("/.git"):
            return d[:-5]
    except OSError:
        pass
    return WT


MAIN = depot_principal()
# Symboles de l'invité : un dossier PAR NOYAU (le banc du M4 a pris le 10.4.6,
# la VM du PC est en 10.4.11 depuis le 02/10). ENDURANCE_INVITE le désigne ;
# le banc vérifie au premier démarrage que le noyau de l'invité est bien celui-là.
INVITE = os.environ.get("ENDURANCE_INVITE") or os.path.join(MAIN, "bench", "endurance", "invite")


def _table(chemin):
    """[(adresse, lettre, nom)] : lecteur Mach-O en Python (tools/re/machonm.py),
    `nm -n` en repli. Le `nm` de binutils du PC Linux ne lit pas le Mach-O et
    rendait une table VIDE sans erreur (06/10/2026) : panicstr introuvable, plus
    aucune panique vue, rien de symbolisé."""
    t = machonm.nm(chemin)
    if t:
        return t
    out = subprocess.run(["nm", "-n", chemin], capture_output=True, text=True).stdout
    res = []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 3:
            res.append((int(p[0], 16), p[1], p[2]))
    return res


def _nm(chemin):
    return sorted((a, n) for a, l, n in _table(chemin) if l in "tT")


def _nm_tout(chemin):
    """Tous les symboles définis (données comprises) : nom → adresse."""
    d = {}
    for a, l, n in _table(chemin):
        if l not in "UuA":
            d.setdefault(n, a)
    return d


def version_noyau(chemin):
    """« Darwin Kernel Version 8.11.0: … » lu dans le fichier, ou ""."""
    try:
        b = open(chemin, "rb").read()
    except OSError:
        return ""
    m = re.search(rb"Darwin Kernel Version [^\0]*", b)
    return m.group(0).decode("latin-1").strip() if m else ""


def lit_kextstat(texte):
    """[(nom, adresse, taille)] des kexts à adresse non nulle d'une sortie kextstat."""
    res = []
    for l in texte.splitlines():
        p = l.split()
        if len(p) >= 6 and p[0].isdigit() and p[2].startswith("0x"):
            a, t = int(p[2], 16), int(p[3], 16)
            if a:
                res.append((p[5], a, t))
    return res


def lit_kmods(chemin):
    """Fichier kmods.txt écrit par le banc : « nom adresse taille » par ligne."""
    res = []
    with open(chemin) as f:
        for l in f:
            p = l.split()
            if len(p) >= 3 and p[1].startswith("0x"):
                res.append((p[0], int(p[1], 16), int(p[2], 16)))
    return res


class Symboliseur:
    def __init__(self, invite=INVITE):
        self.invite = invite
        self.noyau = _nm(os.path.join(invite, "mach_kernel"))
        self.adr_noyau = [a for a, _ in self.noyau]
        self.donnees = _nm_tout(os.path.join(invite, "mach_kernel"))
        if not self.noyau or "_panicstr" not in self.donnees:
            raise SystemExit("aucun symbole lisible dans %s/mach_kernel (panicstr absent) : "
                             "le banc ne verrait aucune panique" % invite)
        self.version = version_noyau(os.path.join(invite, "mach_kernel"))
        try:
            self.fin_texte = machonm.MachO(os.path.join(invite, "mach_kernel")).fin_texte()
        except (OSError, ValueError):
            self.fin_texte = None
        self.fin_texte = self.fin_texte or 0x400000
        # panic() : de _panic au symbole de texte suivant ; un vCPU qui y tourne
        # est en panique même quand panicstr est déjà retombé à 0 (panic() le
        # remet à zéro au retour de Debugger(), avant sa boucle finale :
        # tools/re/kpanic.py)
        p = self.symbole_noyau("panic")
        self.panic = None
        if p is not None:
            i = bisect.bisect_right(self.adr_noyau, p)
            self.panic = (p, self.adr_noyau[i] if i < len(self.adr_noyau) else p + 0x400)
        # fin du texte du noyau : premier kext de référence au-dessus, ou 4 Mo
        sd = os.path.join(invite, "endurance-syms")
        self.ref = {}
        try:
            with open(os.path.join(sd, "kextstat.txt")) as f:
                for nom, a, t in lit_kextstat(f.read()):
                    self.ref[nom] = (a, t)
        except OSError:
            pass
        self.kexts = {}
        self._sd = sd

    def symbole_noyau(self, nom):
        return self.donnees.get(nom) or self.donnees.get("_" + nom)

    def _kext(self, nom):
        if nom not in self.kexts:
            p = os.path.join(self._sd, nom + ".sym")
            self.kexts[nom] = _nm(p) if os.path.exists(p) else []
        return self.kexts[nom]

    def resout(self, adresse, kmods=None):
        """Chaîne « module:symbole+0xdécalage » pour `adresse`."""
        kmods = kmods if kmods is not None else [(n, a, t) for n, (a, t) in self.ref.items()]
        for nom, base, taille in kmods:
            if base <= adresse < base + taille:
                dec = adresse - base
                syms = self._kext(nom)
                if nom in self.ref and syms:
                    cible = self.ref[nom][0] + dec
                    i = bisect.bisect_right([a for a, _ in syms], cible) - 1
                    if i >= 0:
                        return "%s:%s+0x%x" % (court(nom), syms[i][1], cible - syms[i][0])
                return "%s+0x%x" % (court(nom), dec)
        if self.noyau and adresse < self.fin_texte:
            i = bisect.bisect_right(self.adr_noyau, adresse) - 1
            if i >= 0 and adresse - self.adr_noyau[i] < 0x4000:
                return "mach_kernel:%s+0x%x" % (self.noyau[i][1], adresse - self.adr_noyau[i])
        return "?"


def court(nom):
    return nom.replace("com.apple.driver.", "").replace("com.apple.iokit.", "")


def adresses_du_texte(texte):
    """Adresses 0x######## d'un texte de panique (retour arrière, PC=, LR=)."""
    return [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{6,8})\b", texte)]


if __name__ == "__main__":
    args = sys.argv[1:]
    kmods = None
    if "--kmods" in args:
        i = args.index("--kmods")
        kmods = lit_kmods(args[i + 1])
        del args[i:i + 2]
    s = Symboliseur()
    for a in args:
        v = int(a, 16)
        print("%08x -> %s" % (v, s.resout(v, kmods)))
