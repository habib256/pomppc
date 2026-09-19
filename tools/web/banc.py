#!/usr/bin/env python3
"""banc.py — banc de sites pour le relais web des vieux navigateurs.

Lance SA PROPRE instance de scripts/web-proxy.py (port libre, jamais celle de
la VM en cours), demande chaque page du banc à travers elle en se présentant
comme Safari 2 (Tiger), et mesure ce qu'un vieux navigateur en tirerait :

  texte   caractères visibles SANS JavaScript (hors script, style, template,
          noscript) : c'est ce que Safari 2 affiche quand les scripts modernes
          échouent — et ils échouent presque tous ;
  img     images échantillonnées (8 au plus) dans un format que Safari 2
          décode (JPEG, PNG, GIF) / images échantillonnées ;
  js      balises <script> restantes, et Ko de scripts en ligne ;
  https   liens https:// restants (le navigateur ne peut pas les suivre) ;
  mur     page « activez JavaScript », « navigateur non pris en charge »,
          captcha ou connexion obligatoire.

Verdict « lisible » : réponse 200, au moins SEUIL caractères de texte, pas de
mur. Résultat : tableau, et JSON dans bench/web/ (comparaison avec --avant).

    tools/web/banc.py                      # le banc entier
    tools/web/banc.py --sites wiki,hn      # quelques sites
    tools/web/banc.py --mode rendu         # impose un mode du relais
    tools/web/banc.py --avant bench/web/xxx.json   # colonne de comparaison
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.request

from bs4 import BeautifulSoup

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SAFARI2 = ("Mozilla/5.0 (Macintosh; U; PPC Mac OS X; fr-fr) AppleWebKit/418.9.1 "
           "(KHTML, like Gecko) Safari/419.3")
SEUIL = 400

SITES = [
    ("wiki", "https://fr.wikipedia.org/wiki/Power_Mac_G4"),
    ("lemonde", "https://www.lemonde.fr/"),
    ("franceinfo", "https://www.franceinfo.fr/"),
    ("bbc", "https://www.bbc.com/news"),
    ("hn", "https://news.ycombinator.com/"),
    ("github", "https://github.com/habib256/pomppc"),
    ("reddit", "https://www.reddit.com/r/VintageApple/"),
    ("youtube", "https://www.youtube.com/results?search_query=powerbook+g4"),
    ("x", "https://x.com/apple"),
    ("ddg", "https://html.duckduckgo.com/html/?q=mac+os+x+tiger"),
    ("google", "https://www.google.com/search?q=powerpc+g4"),
    ("apple", "https://www.apple.com/fr/"),
    ("stackoverflow", "https://stackoverflow.com/questions/11227809/"),
    ("mdn", "https://developer.mozilla.org/fr/docs/Web/HTML"),
    ("ars", "https://arstechnica.com/"),
    ("verge", "https://www.theverge.com/"),
    ("meteo", "https://meteofrance.com/"),
    ("lowendmac", "https://lowendmac.com/"),
    ("archive", "https://archive.org/"),
    ("macrumors", "https://www.macrumors.com/"),
    ("amazon", "https://www.amazon.fr/"),
    ("osm", "https://www.openstreetmap.org/"),
]

MUR = re.compile(
    r"enable javascript|javascript is (disabled|required|not available)|activ(er|ez) (le )?javascript|"
    r"requires javascript|browser (is )?(not supported|unsupported)|unsupported browser|"
    r"navigateur (n'est pas|non) (pris en charge|compatible)|update your browser|"
    r"mettre à jour votre navigateur|are you a robot|captcha|verify you are human|"
    r"sign in to continue|log in to continue|you need to enable",
    re.I)


def port_libre():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def ouvreur(port):
    return urllib.request.build_opener(
        urllib.request.ProxyHandler({"http": "http://127.0.0.1:%d" % port}))


def demande(op, url, timeout=60, referer=None):
    h = {"User-Agent": SAFARI2, "Accept": "*/*", "Accept-Language": "fr-fr"}
    if referer:
        h["Referer"] = referer                      # comme le vrai navigateur
    req = urllib.request.Request(url, headers=h)
    try:
        with op.open(req, timeout=timeout) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", ""), e.read()
    except Exception as e:                                  # noqa: BLE001
        return 0, "", str(e).encode()


def format_image(data):
    if data[:3] == b"\xff\xd8\xff":
        return "jpeg"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:4] == b"GIF8":
        return "gif"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    if data[4:12] in (b"ftypavif", b"ftypavis"):
        return "avif"
    if b"<svg" in data[:512]:
        return "svg"
    if data[:4] == b"\x00\x00\x01\x00":
        return "ico"
    return "?"


def mesure(op, nom, url):
    t0 = time.time()
    st, ct, body = demande(op, url.replace("https://", "http://", 1))
    dt = time.time() - t0
    r = {"site": nom, "url": url, "statut": st, "octets": len(body), "s": round(dt, 1)}
    if st == 0:
        r.update(texte=0, lisible=False, erreur=body[:120].decode("utf-8", "replace"))
        return r
    soup = BeautifulSoup(body, "lxml")
    scripts = soup.find_all("script")
    r["js"] = len(scripts)
    r["js_ko"] = round(sum(len(s.get_text()) for s in scripts) / 1024)
    for t in soup(["script", "style", "template", "noscript"]):
        t.decompose()
    texte = " ".join(soup.get_text(" ").split())
    r["texte"] = len(texte)
    r["mur"] = bool(MUR.search(texte[:4000])) and len(texte) < 3000
    r["https"] = len(re.findall(rb"https://", body))
    r["integrity"] = len(re.findall(rb"\bintegrity=", body))
    imgs = []
    for im in soup.find_all("img"):
        s = im.get("src") or ""
        if s and not s.startswith("data:"):
            imgs.append(urllib.parse.urljoin(url.replace("https://", "http://", 1), s))
    r["img_total"] = len(imgs)
    ok = vus = 0
    formats = {}
    for u in imgs[:8]:
        if not u.startswith("http://"):
            continue
        _, _, d = demande(op, u, timeout=30, referer=url.replace("https://", "http://", 1))
        f = format_image(d)
        formats[f] = formats.get(f, 0) + 1
        vus += 1
        ok += f in ("jpeg", "png", "gif", "ico")
    r["img_ok"], r["img_vus"], r["img_formats"] = ok, vus, formats
    r["lisible"] = st == 200 and r["texte"] >= SEUIL and not r["mur"]
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sites", help="noms séparés par des virgules")
    ap.add_argument("--mode", help="mode imposé au relais (POMPPC_WEB_MODE)")
    ap.add_argument("--avant", help="JSON d'un banc précédent, pour comparer")
    ap.add_argument("--port", type=int, help="relais déjà lancé sur ce port")
    ap.add_argument("--note", default="", help="étiquette du passage")
    a = ap.parse_args()
    sites = SITES
    if a.sites:
        want = a.sites.split(",")
        sites = [s for s in SITES if s[0] in want]

    proc = None
    port = a.port
    if not port:
        port = port_libre()
        env = dict(os.environ)
        if a.mode:
            env["POMPPC_WEB_MODE"] = a.mode
        proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "scripts", "web-proxy.py"),
                                 "--port", str(port)], env=env,
                                stderr=open(os.path.join(ROOT, "bench", "web-banc-proxy.log"), "w"))
        time.sleep(1.0)
    op = ouvreur(port)
    avant = {}
    if a.avant:
        avant = {r["site"]: r for r in json.load(open(a.avant))["sites"]}
    res = []
    try:
        print("%-13s %4s %8s %7s %6s %5s %6s %5s  %s" %
              ("site", "st", "octets", "texte", "img", "js", "https", "s", "verdict"))
        for nom, url in sites:
            r = mesure(op, nom, url)
            res.append(r)
            av = avant.get(nom)
            cmp = ""
            if av:
                cmp = "  (avant : %s, %d car.)" % ("lisible" if av.get("lisible") else "cassé",
                                                   av.get("texte", 0))
            print("%-13s %4s %8d %7d %6s %5s %6s %5.1f  %s%s%s" % (
                nom, r["statut"], r["octets"], r.get("texte", 0),
                "%d/%d" % (r.get("img_ok", 0), r.get("img_vus", 0)), r.get("js", "-"),
                r.get("https", "-"), r["s"], "LISIBLE" if r["lisible"] else "cassé",
                " [mur]" if r.get("mur") else "", cmp), flush=True)
    finally:
        if proc:
            proc.terminate()
    n = sum(r["lisible"] for r in res)
    imok = sum(r.get("img_ok", 0) for r in res)
    imvu = sum(r.get("img_vus", 0) for r in res)
    print("→ %d/%d lisibles ; images décodables %d/%d" % (n, len(res), imok, imvu))
    os.makedirs(os.path.join(ROOT, "bench", "web"), exist_ok=True)
    out = os.path.join(ROOT, "bench", "web", time.strftime("%Y%m%d-%H%M%S") +
                       ("-" + a.note if a.note else "") + ".json")
    json.dump({"note": a.note, "mode": a.mode, "sites": res}, open(out, "w"),
              ensure_ascii=False, indent=1)
    print("→", out)


if __name__ == "__main__":
    main()
