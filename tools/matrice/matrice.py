#!/usr/bin/env python3
"""matrice.py — matrice de jeux automatisée (chantier A3, docs/matrice-jeux.md).

Joue chaque jeu dans chaque mode (fenêtre, plein écran) sur la VM quotidienne
et range, pour chaque cellule, les trois preuves du TODO §1 :

  1. image juste : le vidage déclenché (POMPPC_GL_DUMP_TRIGGER) est rejoué en
     natif (tests/qgpu_replay.c, backend gl) ; l'une des images rejouées doit
     être identique, à la tolérance près, à la capture de l'écran de la VM
     prise VM arrêtée pendant le vidage ; et le rejeu de la RÉFÉRENCE rangée
     (bench/matrice/ref/, empreinte versionnée dans references.csv, validée à
     l'œil une fois) doit redonner l'image de référence ;
  2. zéro repli : `fallbacks` de frames.csv constant de la fenêtre de mesure à
     la fin du vidage ;
  3. vitesse : ms/image sur la fenêtre de mesure de la scène fixe, sous le
     plancher du jeu.

    tools/matrice/matrice.py                    tout (jeux automatisés, deux modes)
    tools/matrice/matrice.py -j mb,zen -m fen   un sous-ensemble
    tools/matrice/matrice.py --liste            jeux et modes connus
    tools/matrice/matrice.py --valider d3-fen   marque la référence comme validée
    tools/matrice/matrice.py --analyse DOSSIER  refait l'analyse d'un tour rangé
    tools/matrice/matrice.py --reprendre DOSSIER -j prey,ut -m pe   complète un tour

Sorties : bench/matrice/<horodatage>/ (tableau.md, resultats.csv, une
cellule par dossier) dans le dépôt principal ; bench/ n'est pas versionné.
"""
import argparse
import csv
import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hote import Hote, MAIN, WT, journal          # noqa: E402
from jeu import NOM_MODE, MODES                    # noqa: E402

ICI = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(MAIN, "bench", "matrice")
REF = os.path.join(BENCH, "ref")
BIN = os.path.join(BENCH, "bin")
MANIFESTE = os.path.join(ICI, "references.csv")
G = "/Users/tiger/matrice"                         # dossier de la matrice dans l'invité
TRIG = "/tmp/matrice-go"
ORDRE = ["mb", "zen", "d3", "prey", "ut", "wc3", "cmr", "rtcw"]

# tolérances de l'image (écart moyen par composante / % de pixels > 16)
TOL_CAPTURE = (0.3, 0.2)     # rejeu contre capture de la VM (curseur logiciel : ~0,1 %)
TOL_REF = (0.5, 0.1)         # rejeu de la référence contre l'image de référence


# ------------------------------------------------------------------ outils
def construit_outils():
    """qgpu_replay (cœur + backends du dépôt) et ppmcmp, dans bench/matrice/bin."""
    os.makedirs(BIN, exist_ok=True)
    q = os.path.join(WT, "patches", "qgpu")
    cibles = {
        "qgpu_replay": ([os.path.join(WT, "tests", "qgpu_replay.c")] +
                        [os.path.join(q, f) for f in ("qgpu-core.c", "qgpu-soft.c", "qgpu-gl.c",
                                                      "qgpu_proto.h", "qgpu-core.h")],
                        ["cc", "-std=gnu11", "-O1", "-pthread", "-w", "-I", q,
                         os.path.join(WT, "tests", "qgpu_replay.c"),
                         os.path.join(q, "qgpu-core.c"), os.path.join(q, "qgpu-soft.c"),
                         os.path.join(q, "qgpu-gl.c"), "-framework", "OpenGL", "-lm"]),
        "ppmcmp": ([os.path.join(ICI, "ppmcmp.c")],
                   ["cc", "-O2", "-Wall", os.path.join(ICI, "ppmcmp.c"), "-lm"]),
    }
    for nom, (srcs, cmd) in cibles.items():
        out = os.path.join(BIN, nom)
        if os.path.exists(out) and all(os.path.getmtime(s) <= os.path.getmtime(out) for s in srcs):
            continue
        r = subprocess.run(cmd + ["-o", out], capture_output=True, text=True)
        if r.returncode:
            sys.exit("compilation de %s : %s" % (nom, r.stderr[-2000:]))


def charge_jeux():
    jeux = {}
    d = os.path.join(ICI, "jeux")
    for f in sorted(os.listdir(d)):
        if not f.endswith(".py") or f.startswith("_"):
            continue
        spec = importlib.util.spec_from_file_location("jeux." + f[:-3], os.path.join(d, f))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        j = m.JEU
        jeux[j.cle] = j
    return jeux


def lit_frames(txt):
    """frames.csv -> {image: (elapsed_ms, fallbacks, readbacks, dessins)} (compteurs cumulés)."""
    rows = {}
    for l in txt.splitlines():
        p = l.split(",")
        if len(p) >= 7 and p[0].isdigit():
            try:
                rows[int(p[0])] = (float(p[2]), int(p[5]), int(p[6]), int(p[4]))
            except ValueError:
                pass
    return rows


def entete_vidage(chemin):
    with open(chemin, "rb") as f:
        b = f.read(8)
    if len(b) < 8:
        return None
    magic, image = struct.unpack(">II", b)
    return image if magic == 0x50514431 else None


def sha256(chemin):
    h = hashlib.sha256()
    with open(chemin, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def en_png(ppm):
    png = ppm[:-4] + ".png"
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png], capture_output=True)
    return png if os.path.exists(png) else None


# ------------------------------------------------------------ manifeste
def lit_manifeste():
    m = {}
    if os.path.exists(MANIFESTE):
        with open(MANIFESTE) as f:
            for r in csv.DictReader(l for l in f if not l.startswith("#")):
                m[r["cellule"]] = r
    return m


CHAMPS_MANIFESTE = ["cellule", "image", "sha256", "fichiers", "octets", "date", "validee", "note"]


def ecrit_manifeste(m):
    with open(MANIFESTE, "w") as f:
        f.write("# Références d'image de la matrice (docs/matrice-jeux.md §3). Les fichiers\n"
                "# (vidage + image PPM) sont hors dépôt dans bench/matrice/ref/<cellule>/ ;\n"
                "# ici l'empreinte et la validation à l'œil (validee=oui).\n")
        w = csv.DictWriter(f, CHAMPS_MANIFESTE)
        w.writeheader()
        for k in sorted(m):
            w.writerow({c: m[k].get(c, "") for c in CHAMPS_MANIFESTE})


# ------------------------------------------------------------ analyse image
def rejoue(dump, prefixe, presents):
    env = dict(os.environ, QGPU_REPLAY_PRESENTS=presents)
    r = subprocess.run([os.path.join(BIN, "qgpu_replay"), dump, prefixe, "gl"],
                       capture_output=True, text=True, env=env, timeout=1800)
    err = r.stderr
    nerr = None
    for l in err.splitlines():
        if "présentations écrites" in l:
            try:
                nerr = int(l.split(",")[-1].split()[0])
            except (ValueError, IndexError):
                pass
    pres = []
    if os.path.exists(presents):
        for l in open(presents):
            p = l.split()
            if len(p) == 6:
                n, img, off, pas, w, hh = map(int, p)
                pres.append({"n": n, "image": img, "x": (off % pas) // 4 if pas else 0,
                             "y": off // pas if pas else 0, "w": w, "h": hh,
                             "ppm": "%s-%04d-f%d.ppm" % (prefixe, n, img)})
    return nerr, pres, err


def ppmcmp(*args):
    r = subprocess.run([os.path.join(BIN, "ppmcmp")] + list(args), capture_output=True, text=True)
    return r.stdout.strip()


def analyse_image(cel, cle):
    """Rejeu du vidage, comparaison à la capture figée, à la référence."""
    res = {"rejeu_erreurs": "", "capture_ecart": "", "image_rejeu": "", "ref": "", "image_ok": False,
           "image_motif": []}
    dump = os.path.join(cel, "invite", "dump")
    cap = os.path.join(cel, "capture.ppm")
    if not os.path.isdir(dump) or not os.listdir(dump):
        res["image_motif"].append("pas de vidage")
        return res
    rj = os.path.join(cel, "rejeu")
    shutil.rmtree(rj, ignore_errors=True)
    os.makedirs(rj)
    nerr, pres, err = rejoue(dump, os.path.join(rj, "r"), os.path.join(rj, "presents.txt"))
    open(os.path.join(rj, "rejeu.log"), "w").write(err)
    res["rejeu_erreurs"] = "?" if nerr is None else str(nerr)
    res["presentations"] = len(pres)
    if nerr is None or nerr:
        res["image_motif"].append("rejeu : %s soumission(s) en erreur" % res["rejeu_erreurs"])
    if not pres:
        res["image_motif"].append("rejeu sans présentation")
        return res
    meilleur = None
    if os.path.exists(cap):
        for p in pres:
            o = ppmcmp(cap, str(p["x"]), str(p["y"]), p["ppm"]).split()
            if len(o) == 4:
                moy, mx, pct = float(o[1]), int(o[2]), float(o[3])
                if meilleur is None or moy < meilleur[1]:
                    meilleur = (p, moy, mx, pct)
    else:
        res["image_motif"].append("pas de capture figée")
    if meilleur:
        p, moy, mx, pct = meilleur
        res["capture_ecart"] = "%.2f/%.2f%%" % (moy, pct)
        res["image_rejeu"] = os.path.relpath(p["ppm"], cel)
        res["image_numero"] = p["image"]
        ok_cap = moy <= TOL_CAPTURE[0] and pct <= TOL_CAPTURE[1]
        if not ok_cap:
            res["image_motif"].append("rejeu ≠ VM (écart %.2f, %.2f %% des pixels, capture hors du vidage ?)"
                                      % (moy, pct))
        # capture découpée, rangée à côté (PNG pour l'œil)
        crop = os.path.join(cel, "capture-zone.ppm")
        ppmcmp("-c", cap, str(p["x"]), str(p["y"]), str(p["w"]), str(p["h"]), crop)
        en_png(crop)
        en_png(p["ppm"])
        st = ppmcmp("-s", p["ppm"]).split()
        if len(st) == 5 and (float(st[3]) < 4 or int(st[4]) < 8):
            res["image_motif"].append("image vide ou unie (écart-type %s, %s teintes)" % (st[3], st[4]))
        # références
        res.update(verifie_reference(cle, cel, p, dump, creer=ok_cap and nerr == 0))
        if res.get("ref_motif"):
            res["image_motif"].append(res["ref_motif"])
    else:
        ok_cap = False
    # on ne garde que l'image retenue (les autres PPM pèsent 2-3 Mio chacune)
    garde = {res.get("image_rejeu")}
    for p in pres:
        if os.path.relpath(p["ppm"], cel) not in garde and os.path.exists(p["ppm"]):
            os.remove(p["ppm"])
    res["image_ok"] = not res["image_motif"]
    return res


def verifie_reference(cle, cel, p, dump, creer=True):
    """Rejoue la référence rangée et la compare à son image ; sans référence,
    en crée une (à valider à l'œil : --valider) si la preuve du tour est
    complète (rejeu = VM, rejeu sans erreur)."""
    m = lit_manifeste()
    d = os.path.join(REF, cle)
    r = {}
    if cle in m and os.path.exists(os.path.join(d, "image.ppm")):
        e = m[cle]
        if sha256(os.path.join(d, "image.ppm")) != e["sha256"]:
            r["ref"] = "empreinte ≠ manifeste"
            r["ref_motif"] = "référence altérée (empreinte)"
            return r
        tmp = os.path.join(cel, "rejeu-ref")
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        nerr, pres, err = rejoue(os.path.join(d, "dump"), os.path.join(tmp, "r"),
                                 os.path.join(tmp, "presents.txt"))
        cible = [q for q in pres if q["image"] == int(e["image"])]
        if not cible:
            r["ref"] = "image %s absente du rejeu" % e["image"]
            r["ref_motif"] = "rejeu de la référence : image %s absente" % e["image"]
        else:
            o = ppmcmp("-r", os.path.join(d, "image.ppm"), cible[-1]["ppm"]).split()
            if len(o) == 3:
                moy, pct = float(o[0]), float(o[2])
                r["ref"] = "%.2f/%.2f%%" % (moy, pct)
                if moy > TOL_REF[0] or pct > TOL_REF[1]:
                    r["ref_motif"] = "rejeu de la référence ≠ référence (%.2f, %.2f %%)" % (moy, pct)
            else:
                r["ref"] = " ".join(o)
                r["ref_motif"] = "rejeu de la référence : %s" % " ".join(o)
        shutil.rmtree(tmp, ignore_errors=True)
        if e.get("validee") != "oui" and not r.get("ref_motif"):
            r["ref_motif"] = "référence non validée (--valider %s)" % cle
        r["ref"] += " (validée)" if e.get("validee") == "oui" else " (à valider)"
        return r
    if not creer:
        r["ref"] = "aucune"
        r["ref_motif"] = "pas de référence (non créée : preuve du tour incomplète)"
        return r
    # création : fichiers de vidage jusqu'à l'image retenue (le rejeu n'a pas
    # besoin de la suite), l'image rejouée
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "dump"))
    n = octets = 0
    for f in sorted(os.listdir(dump)):
        im = entete_vidage(os.path.join(dump, f))
        if im is not None and im <= p["image"]:
            shutil.copy2(os.path.join(dump, f), os.path.join(d, "dump", f))
            n += 1
            octets += os.path.getsize(os.path.join(dump, f))
    shutil.copy2(p["ppm"], os.path.join(d, "image.ppm"))
    en_png(os.path.join(d, "image.ppm"))
    m[cle] = {"cellule": cle, "image": str(p["image"]), "sha256": sha256(os.path.join(d, "image.ppm")),
              "fichiers": str(n), "octets": str(octets), "date": time.strftime("%Y-%m-%d"),
              "validee": "non", "note": "créée par le tour %s" % os.path.basename(os.path.dirname(cel))}
    ecrit_manifeste(m)
    r["ref"] = "créée (à valider)"
    r["ref_motif"] = "référence créée ce tour, à valider à l'œil (--valider %s)" % cle
    return r


# ------------------------------------------------------------ une cellule
class Cellule:
    def __init__(self, h, jeu, mode, tour, vidage=True):
        """vidage=False : lancement de MESURE (ni déclencheur ni vidage, rangé dans
        <cellule>/mesure/) ; vidage=True : lancement de PREUVE (<cellule>/invite/)."""
        self.h, self.j, self.mode = h, jeu, mode
        self.vidage = vidage
        self.gele = False
        self.cle = "%s-%s" % (jeu.cle, mode)
        self.dir = os.path.join(tour, self.cle)
        self.sous = "invite" if vidage else "mesure"
        self.gd = "%s/%s%s" % (G, self.cle, "" if vidage else "-mesure")   # dans l'invité
        self.res = {"jeu": jeu.titre, "cle": jeu.cle, "mode": NOM_MODE[mode], "verdict": "",
                    "motifs": [], "dossier": os.path.relpath(self.dir, MAIN)}

    def cellule_sh(self):
        env = {"POMPPC_GL_STATS": "1", "POMPPC_GL_NOTE": self.gd + "/note.txt",
               "POMPPC_GL_FRAMES": self.gd + "/frames.csv"}
        if self.vidage:
            env.update({"POMPPC_GL_DUMP": self.gd + "/dump", "POMPPC_GL_DUMP_TRIGGER": TRIG,
                        "POMPPC_GL_DUMP_FRAMES": str(self.j.dump_images)})
        env.update(self.j.env)
        l = ["# écrit par tools/matrice/matrice.py (%s)" % self.cle, "D='%s'" % self.gd]
        for k, v in env.items():
            l.append("%s='%s'; export %s" % (k, v, k))
        l.append("jeu() {\n%s\n}" % self.j.commande(self.mode).strip("\n"))
        return "\n".join(l) + "\n"

    def sauvegarde(self):
        for f in self.j.fichiers_reglages(self.mode):
            self.h.ssh("f='%s'; if [ -e \"$f\" ]; then cp -p \"$f\" \"$f.matrice-sauve\"; "
                       "else touch \"$f.matrice-absent\"; fi" % f)

    def restaure(self):
        for f in self.j.fichiers_reglages(self.mode):
            self.h.ssh("f='%s'; if [ -e \"$f.matrice-sauve\" ]; then mv -f \"$f.matrice-sauve\" \"$f\"; "
                       "elif [ -e \"$f.matrice-absent\" ]; then rm -f \"$f\" \"$f.matrice-absent\"; fi" % f)

    def jouer(self):
        h, j = self.h, self.j
        t0 = time.time()
        os.makedirs(self.dir, exist_ok=True)
        self.res["charge_hote"] = h.charge_hote()
        journal("== %s (%s) — %s" % (j.titre, NOM_MODE[self.mode], self.res["charge_hote"]))
        h.ssh("mkdir -p %s; rm -rf %s; rm -f %s; killall ScreenSaverEngine 2>/dev/null; true"
              % (G, self.gd, TRIG))
        self.sauvegarde()
        rows = {}
        a = b = None
        try:
            j.preparer(h, self.mode)
            h.depose(self.cellule_sh(), G + "/cellule.sh")
            h.ssh("mkdir -p %s && open %s/lance.command" % (self.gd, G))
            lance = time.time()
            premier = muet = 0
            brut, decal = "", 0
            while True:
                time.sleep(10)
                t = time.time() - lance
                # relevé INCRÉMENTAL de frames.csv (tail -c +N) : tout relire à
                # chaque fois coûtait jusqu'à 35 % du processeur invité à sshd
                # et faussait la mesure (DOOM 3, 26/09)
                code, out = h.ssh("tail -c +%d %s/frames.csv 2>/dev/null; echo @@LOG; tail -3 %s/log.txt 2>/dev/null"
                                  % (decal + 1, self.gd, self.gd), delai=60)
                if code in (124, 255) or "@@LOG" not in out:
                    muet += 1
                    if muet >= 4:       # ~5 min sans ssh : invité gelé (26/09, DOOM 3)
                        self.gele = True
                        raise Echec("l'invité ne répond plus (gelé ? image %s)" % (max(rows) if rows else "aucune"))
                    continue
                muet = 0
                fr, _, log = out.partition("@@LOG")
                fin = fr.rfind("\n") + 1           # on ne garde que des lignes entières
                brut += fr[:fin]
                decal += len(fr[:fin].encode())
                rows = lit_frames(brut)
                if "\nexit " in "\n" + log:
                    raise Echec("le jeu a quitté avant la scène (%s)" %
                                log.strip().splitlines()[-1] if log.strip() else "?")
                if rows and premier < 2 and t > 10:
                    h.premier_plan(j.nom_ui())       # lancé par ssh : sinon repli Swap60
                    premier += 1
                elif rows:
                    n = max(rows)
                    if n > 40 and n - 20 in rows and rows[n][1] - rows[n - 20][1] > 5:
                        h.premier_plan(j.nom_ui())
                j.pendant(h, rows, t)
                w = j.fenetre(rows)
                if w:
                    a, b = w
                    break
                if t > j.delai_scene:
                    raise Echec("scène non atteinte en %d s (image %s)" % (t, max(rows) if rows else "aucune"))
            journal("fenêtre de mesure %d..%d atteinte (%.0f s)" % (a, b, time.time() - lance))
            # vidage déclenché puis capture figée pendant le vidage
            if not self.vidage:
                raise Fini()
            # la capture doit tomber sur une image VIDÉE et PRÉSENTÉE. frames.csv
            # n'est vidé par le plugin que toutes les 5 s : c'est le vidage qui
            # sert d'horloge. Chaque fichier porte dans son en-tête (mot 1,
            # grand-boutiste = ordre de l'invité, lu par od) l'image en cours ;
            # on attend que le dernier fichier soit à dump_attente images du
            # premier, puis VM arrêtée tout de suite.
            h.ssh("touch %s; V=%s/dump; i=0; while [ $i -lt 3000 ]; do "
                  "set -- $(ls $V 2>/dev/null | sed -n '1p;$p'); "
                  "if [ $# -ge 2 ]; then a=$(od -An -tu4 -j4 -N4 $V/$1); b=$(od -An -tu4 -j4 -N4 $V/$2); "
                  "[ $((b - a)) -ge %d ] && break; fi; sleep 0.2; i=$((i+1)); done"
                  % (TRIG, self.gd, j.dump_attente), delai=700)
            h.capture_figee(os.path.join(self.dir, "capture.ppm"))
            en_png(os.path.join(self.dir, "capture.ppm"))
            # fin du vidage : le nombre de fichiers ne bouge plus
            avant, stable = -1, 0
            for _ in range(120):
                time.sleep(3)
                n = h.sortie("ls %s/dump | wc -l" % self.gd).strip()
                stable = stable + 1 if n == avant else 0
                avant = n
                if stable >= 2:
                    break
            rows = lit_frames(h.sortie("cat %s/frames.csv" % self.gd))
        except Fini:
            time.sleep(10)
            rows = lit_frames(h.sortie("cat %s/frames.csv" % self.gd))
        except Echec as e:
            self.res["motifs"].append(str(e))
        finally:
            if self.gele:
                # rien ne passe par ssh : RESET, puis on rend les réglages
                h.redemarre()
            try:
                j.arreter(h)
            finally:
                # Terminal ne quitte sans dialogue que si lance.command est fini
                h.ssh("rm -f %s; i=0; while ! grep -q '^exit ' %s/log.txt 2>/dev/null && [ $i -lt 20 ]; "
                      "do sleep 1; i=$((i+1)); done; osascript -e 'tell application \"Terminal\" to quit' "
                      "2>/dev/null; true" % (TRIG, self.gd), delai=60)
                self.restaure()
                j.nettoyer(h, self.mode)
        h.rapatrie(self.gd, os.path.join(self.dir))
        nom = os.path.basename(self.gd)
        if os.path.isdir(os.path.join(self.dir, nom)):
            shutil.rmtree(os.path.join(self.dir, self.sous), ignore_errors=True)
            os.rename(os.path.join(self.dir, nom), os.path.join(self.dir, self.sous))
        h.ssh("rm -rf %s" % self.gd)               # le vidage pèse : on ne le laisse pas dans l'invité
        if j.redemarrer_apres and not self.gele:
            if not h.redemarre():
                self.res["motifs"].append("l'invité ne redémarre pas")
        self.res["duree_s"] = int(time.time() - t0)
        self.res["fenetre"] = "%s..%s" % (a, b) if a is not None else ""
        self.mesures(rows, a, b)
        return self.res

    def mesures(self, rows, a, b):
        r = self.res
        if a is None or a not in rows or b not in rows:
            r["replis"] = r["ms_image"] = ""
            return
        r["ms_image"] = "%.1f" % ((rows[b][0] - rows[a][0]) / (b - a))
        fin = max(rows)
        r["replis"] = str(rows[fin][1] - rows[a][1])
        # en fenêtre, le plugin rend volontairement un échange sur
        # DIRECT_REFRESH (90) à Apple pour rafraîchir la mémoire de la fenêtre
        # côté WindowServer (pomppc_accel.c, direct_target) : Swap60, et Swap58
        # si le jeu fait un glFlush/glFinish par image. Ces replis-là sont admis.
        r["replis_admis"] = str(2 * ((fin - a) // 90 + 1) if self.mode == "fen" else 0)
        r["replis_fenetre"] = "%d..%d" % (a, fin)
        r["relectures"] = str(rows[fin][2] - rows[a][2])


class Echec(Exception):
    pass


class Fini(Exception):
    """Mesure faite, pas de vidage demandé."""


def verdict(res, jeu):
    """Vert / rouge et motifs, à partir des mesures et de l'analyse d'image."""
    res["motifs_execution"] = list(res.get("motifs_execution", res.get("motifs", [])))
    m = list(res["motifs_execution"])
    if res.get("replis", "") == "":
        m.append("pas de mesure")
    else:
        if int(res["replis"]) > int(res.get("replis_admis") or 0):
            m.append("%s repli(s)%s" % (res["replis"], " (rafraîchissement de fenêtre admis : %s)"
                                        % res["replis_admis"] if res.get("replis_admis", "0") != "0" else ""))
        if jeu.plancher_ms and float(res["ms_image"]) > jeu.plancher_ms:
            m.append("%s ms/image > plancher %d" % (res["ms_image"], jeu.plancher_ms))
    m += res.get("image_motif", [])
    if jeu.defaut_connu:
        m.append("défaut connu : " + jeu.defaut_connu)
    res["motifs"] = m
    res["verdict"] = "vert" if not m else "rouge"
    return res


# ------------------------------------------------------------ tableau
COLONNES = ["jeu", "mode", "verdict", "image", "replis", "replis_admis", "replis_detail", "ms_image",
            "ms_image_preuve", "plancher", "rejeu_erreurs",
            "capture_ecart", "ref", "fenetre", "duree_s", "charge_hote", "dossier", "motifs", "motifs_execution"]


def ecrit_tableau(tour, resultats, jeux):
    with open(os.path.join(tour, "resultats.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, COLONNES, extrasaction="ignore")
        w.writeheader()
        for r in resultats:
            w.writerow(dict(r, motifs=" ; ".join(r.get("motifs", [])),
                            motifs_execution=" ; ".join(r.get("motifs_execution", []))))
    L = ["# Matrice de jeux — tour %s" % os.path.basename(tour), "",
         "Commit : `%s`. Produit par `tools/matrice/matrice.py` (docs/matrice-jeux.md)." %
         subprocess.run(["git", "-C", WT, "rev-parse", "--short", "HEAD"], capture_output=True,
                        text=True).stdout.strip(), "",
         "| Jeu | Mode | Verdict | Image (rejeu/VM, réf.) | Replis | ms/image (plancher) | Preuves | Motifs |",
         "|---|---|---|---|---|---|---|---|"]
    for r in resultats:
        v = {"vert": "**VERT**", "rouge": "ROUGE"}.get(r["verdict"], r["verdict"])
        img = r.get("image", "")
        if r.get("capture_ecart"):
            img += " (%s ; réf. %s)" % (r["capture_ecart"], r.get("ref", "—"))
        ms = r.get("ms_image", "") or "—"
        if r.get("plancher"):
            ms += " (%s)" % r["plancher"]
        L.append("| %s | %s | %s | %s | %s | %s | `%s` | %s |" % (
            r["jeu"], r["mode"], v, img or "—", r.get("replis", "") or "—", ms,
            r.get("dossier", ""), " ; ".join(r.get("motifs", [])) or "—"))
    L += ["", "Écart image : moyenne par composante / %% de pixels à plus de 16 d'écart. Tolérances : "
          "rejeu contre VM ≤ %.1f / %.1f %%, rejeu de la référence ≤ %.1f / %.1f %%." % (TOL_CAPTURE + TOL_REF),
          "Replis en fenêtre : le plugin rend un échange sur 90 à Apple (rafraîchissement de la "
          "fenêtre, voulu) ; 2 par tranche de 90 images sont admis.",
          "Vitesse bruitée si « autre(s) QEMU » > 0 dans resultats.csv (colonne charge_hote)."]
    open(os.path.join(tour, "tableau.md"), "w").write("\n".join(L) + "\n")
    return "\n".join(L)


def complete(res, jeu, mode, analyse):
    res.update(analyse)
    mi = analyse.get("image_motif", [])
    if not analyse:
        res["image"] = "—"
    elif analyse.get("image_ok"):
        res["image"] = "juste"
    elif all(x.startswith("référence") for x in mi):
        res["image"] = "à valider"      # rejeu = VM ; seule manque la validation à l'œil
    elif any(x.startswith(("rejeu ≠ VM", "rejeu de la référence ≠", "image vide")) for x in mi):
        res["image"] = "fausse"
    else:
        res["image"] = "non prouvée"    # pas de vidage, de capture, rejeu en erreur…
    if jeu.plancher_ms:
        res["plancher"] = str(jeu.plancher_ms)
    return verdict(res, jeu)


def un_lancement(h, j, mode, tour, vidage):
    c = Cellule(h, j, mode, tour, vidage=vidage)
    try:
        return c.jouer()
    except Exception as e:      # une cellule en échec n'arrête pas le tour
        journal("cellule %s : %r" % (c.cle, e))
        c.res["motifs"].append("erreur du pilote : %r" % e)
        return c.res


def joue_cellule(h, j, mode, tour, a):
    """Deux lancements par défaut : MESURE sans déclencheur (vitesse, replis),
    puis PREUVE avec déclencheur (vidage, capture figée, replis). Le
    déclencheur coûte un access() par dessin texturé tant que le fichier
    n'existe pas (pomppc_accel.c, draw_probe/cube_probe) : DOOM 3 139 contre
    77 ms/image (26/09) ; la vitesse ne se mesure donc pas pendant la preuve.
    --une-passe : un seul lancement (vitesse prise avec le déclencheur,
    biaisée) ; --sans-vidage : la mesure seule."""
    if a.sans_vidage:
        r = un_lancement(h, j, mode, tour, False)
        r["motifs"].append("tour sans vidage (--sans-vidage) : vitesse et replis seulement")
        return r
    if a.une_passe:
        r = un_lancement(h, j, mode, tour, True)
        r["preuve"] = bool(r.get("fenetre"))
        if r.get("ms_image"):
            r["motifs"].append("vitesse prise avec le déclencheur de vidage (--une-passe) : biaisée")
        return r
    m = un_lancement(h, j, mode, tour, False)
    p = un_lancement(h, j, mode, tour, True)
    r = dict(p)
    r["preuve"] = bool(p.get("fenetre"))
    r["motifs"] = ["mesure : " + x for x in m.get("motifs", [])] + p.get("motifs", [])
    r["ms_image"] = m.get("ms_image", "")
    r["ms_image_preuve"] = p.get("ms_image", "")
    r["fenetre"] = "%s (preuve %s)" % (m.get("fenetre", ""), p.get("fenetre", ""))
    r["duree_s"] = (m.get("duree_s") or 0) + (p.get("duree_s") or 0)
    rp = [x.get("replis", "") for x in (m, p)]
    if all(x != "" for x in rp):
        r["replis"] = str(int(rp[0]) + int(rp[1]))
        r["replis_admis"] = str(int(m.get("replis_admis") or 0) + int(p.get("replis_admis") or 0))
        r["replis_detail"] = "mesure %s, preuve %s" % tuple(rp)
    else:
        r["replis"] = ""
    return r


# ------------------------------------------------------------ principal
def restaure_orphelins(h):
    """Un tour interrompu a pu laisser des réglages sauvegardés : on les rend."""
    out = h.sortie("find /Users/tiger/Library /Users/tiger/Desktop -name '*.matrice-sauve' -o "
                   "-name '*.matrice-absent' 2>/dev/null", delai=120)
    for f in out.split("\n"):
        f = f.strip()
        if f.endswith(".matrice-sauve"):
            journal("réglage rendu : %s" % f[:-14])
            h.ssh("mv -f '%s' '%s'" % (f, f[:-14]))
        elif f.endswith(".matrice-absent"):
            h.ssh("rm -f '%s' '%s'" % (f, f[:-15]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-j", "--jeux", default="", help="jeux (clés séparées par des virgules)")
    ap.add_argument("-m", "--modes", default="fen,pe", help="fen, pe ou fen,pe")
    ap.add_argument("--liste", action="store_true")
    ap.add_argument("--valider", metavar="CELLULE")
    ap.add_argument("--analyse", metavar="TOUR", help="refait l'analyse d'un tour déjà joué")
    ap.add_argument("--sortie", help="dossier du tour (défaut bench/matrice/<horodatage>)")
    ap.add_argument("--reprendre", metavar="TOUR",
                    help="compléter un tour interrompu : garde ses cellules, rejoue la sélection")
    ap.add_argument("--une-passe", action="store_true",
                    help="un seul lancement par cellule (vitesse biaisée par le déclencheur)")
    ap.add_argument("--sans-vidage", action="store_true",
                    help="ni vidage ni capture : vitesse et replis seuls (le déclencheur du vidage "
                         "coûte un access() par dessin texturé, docs/matrice-jeux.md §5)")
    a = ap.parse_args()
    jeux = charge_jeux()
    ordre = [k for k in ORDRE if k in jeux] + sorted(k for k in jeux if k not in ORDRE)
    if a.liste:
        for k in ordre:
            j = jeux[k]
            print("%-5s %-22s %s" % (k, j.titre, " ".join(
                "%s%s" % (m, "(non automatisé)" if m in j.non_automatise else "") for m in MODES)))
        return
    if a.valider:
        m = lit_manifeste()
        if a.valider not in m:
            sys.exit("pas de référence %s" % a.valider)
        m[a.valider]["validee"] = "oui"
        ecrit_manifeste(m)
        print("référence %s validée : pensez à committer tools/matrice/references.csv" % a.valider)
        return
    construit_outils()
    choisis = [k for k in (a.jeux.split(",") if a.jeux else ordre) if k]
    modes = [m for m in a.modes.split(",") if m in MODES]
    for k in choisis:
        if k not in jeux:
            sys.exit("jeu inconnu : %s (--liste)" % k)
    if a.analyse:
        tour = os.path.abspath(a.analyse)
        resultats = []
        for r in csv.DictReader(open(os.path.join(tour, "resultats.csv"))):
            if r["verdict"] == "non automatisé":
                r["motifs"] = [r["motifs"]]
                resultats.append(r)
                continue
            j = jeux[r["dossier"].split("/")[-1].rsplit("-", 1)[0]]
            mode = r["dossier"].rsplit("-", 1)[1]
            r["motifs_execution"] = [x for x in r.get("motifs_execution", "").split(" ; ") if x]
            an = analyse_image(os.path.join(MAIN, r["dossier"]), "%s-%s" % (j.cle, mode))
            resultats.append(complete(r, j, mode, an))
        print(ecrit_tableau(tour, resultats, jeux))
        return
    tour = a.reprendre or a.sortie or os.path.join(BENCH, time.strftime("%Y%m%d-%H%M"))
    tour = os.path.abspath(tour)
    os.makedirs(tour, exist_ok=True)
    # --reprendre : les cellules déjà jouées du tour sont gardées, celles de la
    # sélection (-j, -m) sont rejouées et remplacent les anciennes
    anciens = {}
    if a.reprendre and os.path.exists(os.path.join(tour, "resultats.csv")):
        par_titre = {j.titre: k for k, j in jeux.items()}
        par_mode = {v: k for k, v in NOM_MODE.items()}
        for r in csv.DictReader(open(os.path.join(tour, "resultats.csv"))):
            r["motifs"] = [x for x in r.get("motifs", "").split(" ; ") if x]
            r["motifs_execution"] = [x for x in r.get("motifs_execution", "").split(" ; ") if x]
            anciens[(par_titre.get(r["jeu"]), par_mode.get(r["mode"]))] = r
    h = Hote()
    if not h.attend_ssh(60):
        sys.exit("l'invité ne répond pas (VM lancée ? .run/cmr/tssh.sh uptime)")
    # un tour interrompu a pu laisser un jeu en marche : on l'arrête (et on
    # redémarre l'invité si c'était DOOM 3), puis on rend les réglages
    for j in jeux.values():
        if j.processus and h.processus(j.processus):
            journal("jeu resté en marche : %s, arrêté" % j.titre)
            j.arreter(h)
            h.ssh("osascript -e 'tell application \"Terminal\" to quit' 2>/dev/null; true")
            if j.redemarrer_apres:
                h.redemarre()
    restaure_orphelins(h)
    for j in jeux.values():
        for mode in MODES:
            if mode not in j.non_automatise:
                j.nettoyer(h, mode)
    h.ssh("mkdir -p %s" % G)
    h.depose(open(os.path.join(ICI, "guest", "lance.command")).read(), G + "/lance.command")
    h.ssh("chmod +x %s/lance.command" % G)
    resultats = []
    t0 = time.time()
    for k in ordre:
        j = jeux[k]
        for mode in MODES:
            if mode in j.non_automatise:     # toujours au tableau, avec sa raison
                if (k in choisis and mode in modes) or not a.jeux:
                    resultats.append({"jeu": j.titre, "mode": NOM_MODE[mode], "verdict": "non automatisé",
                                      "motifs": [j.non_automatise[mode]], "dossier": ""})
                    ecrit_tableau(tour, resultats, jeux)
                elif a.reprendre:
                    resultats.append(anciens.get((k, mode)) or {
                        "jeu": j.titre, "mode": NOM_MODE[mode], "verdict": "non automatisé",
                        "motifs": [j.non_automatise[mode]], "dossier": ""})
                continue
            if k not in choisis or mode not in modes:
                if (k, mode) in anciens:
                    resultats.append(anciens[(k, mode)])
                continue
            res = joue_cellule(h, j, mode, tour, a)
            c = Cellule(h, j, mode, tour)
            an = analyse_image(c.dir, c.cle) if res.get("preuve") else {}
            complete(res, j, mode, an)
            journal("%s : %s %s" % (c.cle, res["verdict"], " ; ".join(res["motifs"])))
            resultats.append(res)
            ecrit_tableau(tour, resultats, jeux)
    print(ecrit_tableau(tour, resultats, jeux))
    journal("tour complet en %d min : %s" % ((time.time() - t0) / 60, tour))
    lien = os.path.join(BENCH, "dernier")
    if os.path.islink(lien):
        os.remove(lien)
    if not os.path.exists(lien):
        os.symlink(os.path.basename(tour), lien)


if __name__ == "__main__":
    main()
