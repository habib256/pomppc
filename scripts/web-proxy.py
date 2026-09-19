#!/usr/bin/env python3
"""web-proxy.py — relais web pour les vieux navigateurs de l'invité (Tiger, OS 9).

Tiger n'a qu'OpenSSL 0.9.7, des certificats racines de 2006, Safari 2 et son
moteur JavaScript d'avant ES5 : le web actuel lui est fermé trois fois — TLS,
JavaScript, formats. Ce relais tourne sur l'hôte, parle HTTP simple à
l'invité, et lève les trois verrous :

  TLS         il va chercher les pages en HTTPS moderne (certificats de
              l'hôte) et réécrit « https:// » en « http:// » pour que la
              navigation continue à passer par lui ;
  JavaScript  selon le MODE du site (réglable de la barre en tête de page,
              ou de http://pomppc/) :
                sansjs  les scripts sont retirés, <noscript> déplié : le site
                        tel qu'il se lit sans JavaScript (défaut) ;
                rendu   Chrome, sans affichage, sur l'hôte, exécute la page ;
                        l'invité reçoit le document qui en résulte, sans
                        scripts ;
                brut    la page telle quelle (scripts compris) ;
                auto    sansjs, et rendu si la page sort vide ou demande
                        JavaScript (appris par site) ;
  formats     images WebP, AVIF et SVG converties en JPEG ou PNG, grandes
              images réduites ; ce que Safari 2 afficherait à tort (attribut
              hidden, <template>, SVG en ligne, bannières de cookies) retiré.

    python3 scripts/web-proxy.py [--port 8080] [--parent-pid PID]

Dans l'invité (QEMU en réseau utilisateur), l'hôte est 10.0.2.2 :
    Préférences Système → Réseau → Proxys → Proxy web (HTTP) : 10.0.2.2, port 8080
    ou : curl -x 10.0.2.2:8080 http://www.example.com/
Page d'accueil et réglages : http://pomppc/

Le relais n'écoute que sur 127.0.0.1 : il n'est joignable que depuis l'hôte et
ses invités QEMU, jamais depuis le réseau local. CONNECT est relayé tel quel
(tunnel) pour les rares sites qui acceptent encore le TLS de Tiger.

Dépendances FACULTATIVES, détectées au démarrage : BeautifulSoup + lxml
(retouche du HTML, modes sansjs et rendu), Pillow et ImageMagick (images),
Google Chrome ou Chromium (mode rendu). Sans elles : bibliothèque standard
seulement, et le relais se contente de TLS et de la réécriture des liens.
Réglages persistants : ~/.config/pomppc/web-proxy.json. Mode par défaut :
POMPPC_WEB_MODE (auto).
"""
import argparse
import gzip
import html
import http.client
import io
import json
import os
import re
import select
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
import zlib
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlencode, urljoin, urlsplit

try:
    from bs4 import BeautifulSoup, Comment
    import lxml  # noqa: F401  (analyseur de BeautifulSoup)
    HAVE_BS4 = True
except ImportError:
    HAVE_BS4 = False
try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

CHROME = next((p for p in (shutil.which(n) for n in (
    "google-chrome", "google-chrome-stable", "chromium", "chromium-browser")) if p), None)
CONVERT = shutil.which("convert") or shutil.which("magick")

TEXT_TYPES = ("text/", "application/javascript", "application/x-javascript",
              "application/json", "application/xhtml", "application/xml",
              "application/rss", "application/atom", "image/svg")
DROP_HEADERS = {"strict-transport-security", "content-security-policy",
                "content-security-policy-report-only", "alt-svc",
                "transfer-encoding", "content-encoding", "content-length",
                "connection", "keep-alive", "public-key-pins", "expect-ct",
                "cross-origin-embedder-policy", "cross-origin-opener-policy",
                "cross-origin-resource-policy", "permissions-policy", "nel",
                "report-to", "origin-agent-cluster"}
HOP_HEADERS = {"proxy-connection", "connection", "keep-alive", "te", "trailer",
               "upgrade", "proxy-authorization", "accept-encoding"}
MAX_BODY = 64 * 1024 * 1024
STREAM_OVER = 8 * 1024 * 1024       # au-delà, un fichier non retouché part en flux
# Ce que voient les serveurs : un navigateur moderne (sinon « navigateur non
# pris en charge », ou des murs anti-robots), qui n'accepte NI WebP NI AVIF —
# les serveurs qui négocient rendent alors du JPEG ou du PNG.
UA_MODERNE = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36")
ACCEPT = ("text/html,application/xhtml+xml,application/xml;q=0.9,"
          "image/jpeg,image/png,image/gif,*/*;q=0.8")
MODES = ("auto", "sansjs", "rendu", "brut")
MODE_NOMS = {"auto": "auto", "sansjs": "sans JS", "rendu": "rendu", "brut": "brut"}
SEUIL_TEXTE = 400                   # en dessous, « auto » demande le rendu
LARGEUR_MAX = int(os.environ.get("POMPPC_WEB_WIDTH", "1024"))
CONF_PATH = os.path.expanduser("~/.config/pomppc/web-proxy.json")
ICI = "pomppc"                      # hôte virtuel des pages du relais

# Hôtes qui ne parlent qu'HTTP (appris au vol) : on ne retente pas HTTPS.
PLAIN_ONLY = set()
LOCK = threading.Lock()
CTX = ssl.create_default_context()
APPRIS = {}                         # mode « auto » tranché par hôte (mémoire)
IMG_CACHE = OrderedDict()           # url → (type, données), images converties
IMG_CACHE_MAX = 64 * 1024 * 1024
RENDU_SEM = threading.Semaphore(2)  # Chrome : deux rendus à la fois au plus

MUR = re.compile(
    r"enable javascript|javascript is (disabled|required|not available)|"
    r"activ(er|ez) (le )?javascript|requires javascript|browser (is )?(not supported|"
    r"unsupported)|unsupported browser|navigateur (n'est pas|non) (pris en charge|"
    r"compatible)|update your browser|are you a robot|verify you are human|"
    r"you need to enable|please turn javascript on|javascript n'est pas disponible|"
    r"javascript est (désactivé|requis|nécessaire)|navigateur (obsolète|pas à jour)", re.I)
CONSENT = re.compile(r"didomi|onetrust|cookie-?(banner|consent|notice|law|wall)|"
                     r"consent-?(banner|manager|wall)|gdpr|qc-cmp|sp_message|fc-consent|"
                     r"cmp-?(container|banner)|truste|usercentrics|axeptio|tarteaucitron",
                     re.I)
# adresses de ressources (jamais un document à faire rendre par Chrome)
RESSOURCE = re.compile(r"\.(jpe?g|png|gif|webp|avif|svg|ico|bmp|css|js|mjs|json|xml|woff2?|"
                       r"ttf|otf|eot|mp4|webm|mov|m4v|mp3|m4a|ogg|zip|dmg|gz|pdf)$", re.I)
TRACKERS = re.compile(r"googletagmanager|doubleclick|google-analytics|facebook\.com/tr|"
                      r"scorecardresearch|chartbeat", re.I)


def log(msg):
    sys.stderr.write(time.strftime("%H:%M:%S ") + msg + "\n")
    sys.stderr.flush()


# ───────────────────────────── réglages ─────────────────────────────

class Conf:
    def __init__(self):
        self.modes = {}             # hôte → mode imposé par l'utilisateur
        self.barre = True           # barre de mode en tête des pages
        self.defaut = os.environ.get("POMPPC_WEB_MODE", "auto")
        try:
            d = json.load(open(CONF_PATH))
            self.modes = {h: m for h, m in d.get("modes", {}).items() if m in MODES}
            self.barre = bool(d.get("barre", True))
        except (OSError, ValueError):
            pass
        if self.defaut not in MODES:
            self.defaut = "auto"

    def save(self):
        try:
            os.makedirs(os.path.dirname(CONF_PATH), exist_ok=True)
            tmp = CONF_PATH + ".tmp"
            json.dump({"modes": self.modes, "barre": self.barre}, open(tmp, "w"),
                      indent=1, sort_keys=True)
            os.replace(tmp, CONF_PATH)
        except OSError as e:
            log("réglages non enregistrés : %s" % e)

    def mode_de(self, host):
        """(mode demandé, mode effectif) pour un hôte."""
        h = host.lower()
        for k in (h, h[4:] if h.startswith("www.") else "www." + h):
            if k in self.modes:
                return self.modes[k], self.modes[k]
        if self.defaut == "auto":
            return "auto", APPRIS.get(h, "auto")
        return self.defaut, self.defaut


CONF = Conf()


# ───────────────────────────── utilitaires ─────────────────────────────

def downgrade(data: bytes) -> bytes:
    return data.replace(b"https://", b"http://").replace(b"https:\\/\\/", b"http:\\/\\/")


def fix_cookie(value: str) -> str:
    # Sans « Secure », l'invité renverra le cookie en HTTP (vers le relais).
    v = re.sub(r";\s*secure\b", "", value, flags=re.I)
    v = re.sub(r";\s*samesite=none\b", "", v, flags=re.I)
    return re.sub(r";\s*partitioned\b", "", v, flags=re.I)


def decode_body(body: bytes, encoding: str) -> bytes:
    encoding = (encoding or "").lower()
    if encoding == "gzip":
        return gzip.decompress(body)
    if encoding == "deflate":
        try:
            return zlib.decompress(body)
        except zlib.error:
            return zlib.decompress(body, -zlib.MAX_WBITS)
    return body


def format_image(data):
    if data[:3] == b"\xff\xd8\xff":
        return "jpeg"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:4] == b"GIF8":
        return "gif"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    if data[4:12] in (b"ftypavif", b"ftypavis", b"ftypheic", b"ftypmif1"):
        return "avif"
    head = data[:1024].lstrip()
    if head.startswith(b"<svg") or (head.startswith(b"<?xml") and b"<svg" in head):
        return "svg"
    return None


# ───────────────────────────── images ─────────────────────────────

def _pil_out(im, force_png=False):
    """Image Pillow → (type, octets) décodable par Safari 2, réduite."""
    if im.width > LARGEUR_MAX:
        h = max(1, round(im.height * LARGEUR_MAX / im.width))
        im = im.resize((LARGEUR_MAX, h), Image.LANCZOS)
    alpha = im.mode in ("RGBA", "LA", "PA") or (im.mode == "P" and "transparency" in im.info)
    out = io.BytesIO()
    if alpha or force_png:
        im.save(out, "PNG")
        return "image/png", out.getvalue()
    im.convert("RGB").save(out, "JPEG", quality=82, optimize=True)
    return "image/jpeg", out.getvalue()


def convertit_image(data, fmt):
    """(type, octets) ou None si l'image peut partir telle quelle."""
    try:
        if fmt == "webp" and HAVE_PIL:
            im = Image.open(io.BytesIO(data))
            if getattr(im, "is_animated", False):
                im.seek(0)
            return _pil_out(im)
        if fmt in ("avif", "svg") and CONVERT:
            src = "svg:-" if fmt == "svg" else "-"
            args = [CONVERT, "-density", "96", "-background", "none", src]
            if fmt == "svg":
                args += ["-resize", "%dx%d>" % (LARGEUR_MAX, LARGEUR_MAX), "png:-"]
            else:
                args += ["-resize", "%dx>" % LARGEUR_MAX, "-quality", "82", "jpeg:-"]
            r = subprocess.run(args, input=data, capture_output=True, timeout=20)
            if r.returncode == 0 and r.stdout:
                return ("image/png" if fmt == "svg" else "image/jpeg"), r.stdout
            return None
        if fmt in ("jpeg", "png") and HAVE_PIL and len(data) > 60000:
            im = Image.open(io.BytesIO(data))
            if im.width > LARGEUR_MAX:              # grande image : réduite
                return _pil_out(im, force_png=(fmt == "png"))
    except Exception as e:                          # noqa: BLE001
        log("image non convertie (%s) : %s" % (fmt, e))
    return None


def cache_image(url, val=None):
    with LOCK:
        if val is None:
            v = IMG_CACHE.get(url)
            if v:
                IMG_CACHE.move_to_end(url)
            return v
        IMG_CACHE[url] = val
        total = sum(len(d) for _, d in IMG_CACHE.values())
        while total > IMG_CACHE_MAX and IMG_CACHE:
            _, (_, d) = IMG_CACHE.popitem(last=False)
            total -= len(d)
        return val


# ───────────────────────────── HTML ─────────────────────────────

def _srcset_best(srcset):
    """Le candidat d'un srcset le plus proche de LARGEUR_MAX sans le dépasser."""
    best, best_w = None, -1
    for part in srcset.split(","):
        bits = part.strip().split()
        if not bits:
            continue
        url, w = bits[0], 0
        if len(bits) > 1:
            d = bits[1]
            try:
                w = int(float(d[:-1]) * (1 if d.endswith("w") else 400))
            except ValueError:
                w = 0
        if best is None or (best_w > LARGEUR_MAX and w < best_w) or \
                (w <= LARGEUR_MAX and w > best_w):
            best, best_w = url, w
    return best


LAZY = ("data-src", "data-lazy-src", "data-original", "data-lazy", "data-url",
        "data-hi-res-src", "data-full-src")


def retouche(soup, mode, url):
    """Retouche le document pour un vieux navigateur ; rend le texte visible."""
    # 1. scripts et assimilés
    if mode in ("sansjs", "rendu"):
        for t in soup.find_all("script"):
            t.decompose()
        for t in soup.find_all("noscript"):
            txt = t.get_text(" ", strip=True)
            if mode == "rendu" or MUR.search(txt) or t.find("iframe", src=TRACKERS):
                t.decompose()
            else:
                t.unwrap()
        for t in soup.find_all("link", rel=True):
            rel = " ".join(t.get("rel")).lower()
            if any(k in rel for k in ("preload", "modulepreload", "prefetch", "preconnect",
                                      "dns-prefetch", "manifest")):
                t.decompose()
        for t in soup.find_all(True):
            for a in [a for a in t.attrs if a.lower().startswith("on")]:
                del t[a]
    for t in soup.find_all("meta", attrs={"http-equiv": re.compile("security-policy", re.I)}):
        t.decompose()
    # 2. ce que Safari 2 afficherait à tort
    for t in soup.find_all(["template", "svg", "math"]):
        t.decompose()
    for t in soup.find_all(attrs={"hidden": True}):
        t.decompose()
    for t in soup.find_all("dialog"):
        if not t.has_attr("open"):
            t.decompose()
    for t in soup.find_all("iframe", src=TRACKERS):
        t.decompose()
    # bannières de consentement — jamais un bloc qui porte du vrai contenu
    for t in soup.find_all(True, id=CONSENT) + soup.find_all(True, class_=CONSENT):
        if t.decomposed or t.name in ("html", "body", "main", "article"):
            continue
        if len(t.get_text(" ", strip=True)) < 2500:
            t.decompose()
    for t in soup.find_all(attrs={"integrity": True}):
        del t["integrity"]
    # 3. images : la vraie adresse, pas de srcset
    for t in soup.find_all("source"):
        par = t.parent
        if par is not None and par.name == "picture":
            img = par.find("img")
            if img is not None and not img.get("src") and t.get("srcset"):
                img["src"] = _srcset_best(t["srcset"])
            t.decompose()
    for im in soup.find_all("img"):
        src = im.get("src", "")
        vide = not src or src.startswith("data:") or "blank" in src or "spacer" in src
        for a in LAZY:
            if im.get(a) and vide:
                im["src"] = im[a]
                vide = False
        for a in ("srcset", "data-srcset", "data-lazy-srcset"):
            if im.get(a):
                if vide:
                    im["src"] = _srcset_best(im[a])
                    vide = False
                del im[a]
        for a in ("sizes", "loading", "decoding", "fetchpriority"):
            if im.has_attr(a):
                del im[a]
    # 4. vidéo et audio HTML5 : un lien vers le fichier
    for t in soup.find_all(["video", "audio"]):
        src = t.get("src") or (t.find("source") or {}).get("src") if t else None
        a = soup.new_tag("a", href=src or url)
        a.string = "[%s]" % ("vidéo" if t.name == "video" else "son")
        t.replace_with(a)
    for c in soup.find_all(string=lambda s: isinstance(s, Comment)):
        c.extract()
    # 5. texte visible (pour « auto »)
    body = soup.body or soup
    texte = " ".join(body.get_text(" ").split())
    return texte


def barre(mode_dem, mode_eff, host, url):
    liens = []
    for m in ("sansjs", "rendu", "brut", "auto"):
        nom = MODE_NOMS[m]
        if m == mode_dem:
            nom = "<b>%s</b>" % nom
        else:
            nom = '<a href="http://%s/mode?%s">%s</a>' % (
                ICI, html.escape(urlencode({"h": host, "m": m, "u": url})), nom)
        liens.append(nom)
    eff = "" if mode_dem == mode_eff else " (%s)" % MODE_NOMS[mode_eff]
    return ('<table width="100%%" cellspacing="0" cellpadding="2" border="0" '
            'bgcolor="#ffffcc"><tr><td><font face="Geneva,Helvetica,Arial" size="1" '
            'color="#333333">POMPPC%s &middot; %s &middot; <a href="http://%s/">accueil'
            '</a></font></td></tr></table>' % (eff, " | ".join(liens), ICI))


def serialise(soup):
    """Document → octets UTF-8, charset déclaré, https rabattu."""
    for t in soup.find_all("meta"):
        if t.get("charset") or (t.get("http-equiv", "").lower() == "content-type"):
            t.decompose()
    head = soup.head
    if head is not None:
        m = soup.new_tag("meta")
        m["http-equiv"] = "Content-Type"
        m["content"] = "text/html; charset=utf-8"
        head.insert(0, m)
    return downgrade(str(soup).encode("utf-8", "replace"))


# ───────────────────────────── rendu par Chrome ─────────────────────────────

_PROFILS = []


def rendu_chrome(url):
    """DOM de la page après exécution par Chrome sans affichage, ou None."""
    if not CHROME:
        return None
    with RENDU_SEM:
        with LOCK:
            if not _PROFILS:
                base = os.path.expanduser("~/.cache/pomppc/chrome")
                _PROFILS.extend(os.path.join(base, "p%d" % i) for i in range(2))
            prof = _PROFILS.pop()
        try:
            os.makedirs(prof, exist_ok=True)
            cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run",
                   "--no-default-browser-check", "--hide-scrollbars", "--mute-audio",
                   "--disable-extensions", "--window-size=1024,768",
                   "--virtual-time-budget=8000", "--user-agent=" + UA_MODERNE,
                   "--user-data-dir=" + prof, "--dump-dom", url]
            t0 = time.time()
            r = subprocess.run(cmd, capture_output=True, timeout=25)
            log("  rendu Chrome %s : %d o en %.1f s" % (url[:80], len(r.stdout), time.time() - t0))
            return r.stdout if r.returncode == 0 and len(r.stdout) > 200 else None
        except (OSError, subprocess.TimeoutExpired) as e:
            log("  rendu Chrome impossible : %s" % e)
            return None
        finally:
            with LOCK:
                _PROFILS.append(prof)


# ───────────────────────────── adaptateurs de site ─────────────────────────────
#
# Quelques sites ferment la porte à tout ce qui n'est pas un navigateur récent
# — Chrome sans affichage compris — mais gardent une entrée de service : on
# fabrique alors la page nous-mêmes, en HTML simple.

ATOM = "{http://www.w3.org/2005/Atom}"
SE_SITES = {"stackoverflow.com": "stackoverflow", "superuser.com": "superuser",
            "serverfault.com": "serverfault", "askubuntu.com": "askubuntu",
            "mathoverflow.net": "mathoverflow.net", "stackapps.com": "stackapps"}


def get_https(url, accept="*/*", timeout=20):
    """(statut, octets décompressés) d'un GET HTTPS fait par le relais."""
    u = urlsplit(url)
    path = (u.path or "/") + ("?" + u.query if u.query else "")
    conn = http.client.HTTPSConnection(u.hostname, u.port or 443, timeout=timeout, context=CTX)
    try:
        conn.request("GET", path, headers={
            "User-Agent": UA_MODERNE, "Accept": accept, "Accept-Encoding": "gzip, deflate",
            "Accept-Language": "fr-FR,fr;q=0.9,en;q=0.8"})
        r = conn.getresponse()
        data = r.read(MAX_BODY)
        try:
            data = decode_body(data, r.getheader("Content-Encoding"))
        except (OSError, zlib.error, EOFError):
            pass
        return r.status, data
    finally:
        conn.close()


def page_simple(titre, corps):
    return ("<html><head><title>%s</title></head><body bgcolor=\"#ffffff\">"
            "<font face=\"Geneva,Helvetica,Arial\">%s</font></body></html>" % (titre, corps))


def ad_reddit(host, path):
    """Reddit n'ouvre plus ses pages (ni l'ancien Reddit) qu'aux navigateurs
    récents qui passent son défi ; ses flux Atom restent ouverts."""
    p = urlsplit(path)
    base = p.path.rstrip("/")
    if base.endswith("/.rss") or not (base == "" or base.startswith(("/r/", "/user/", "/u/"))):
        return None
    st, data = get_https("https://www.reddit.com%s/.rss%s" % (
        base, "?" + p.query if p.query else ""), accept="application/atom+xml")
    if st != 200:
        return None
    root = ET.fromstring(data)
    titre = html.escape(root.findtext(ATOM + "title") or "Reddit")
    out = ["<h2>%s</h2>" % titre,
           '<p><font size="1">Reddit, lu par son flux (lecture seule). '
           '<a href="http://www.reddit.com/r/popular/">populaire</a></font></p>']
    for e in root.findall(ATOM + "entry"):
        lien = e.find(ATOM + "link")
        href = lien.get("href") if lien is not None else "#"
        au = e.findtext(ATOM + "author/" + ATOM + "name") or ""
        quand = (e.findtext(ATOM + "updated") or "")[:16].replace("T", " ")
        out.append('<h3><a href="%s">%s</a></h3><font size="1" color="#666666">%s &middot; %s'
                   '</font><div>%s</div><hr>' % (
                       html.escape(href), html.escape(e.findtext(ATOM + "title") or ""),
                       html.escape(au), quand, e.findtext(ATOM + "content") or ""))
    return page_simple(titre, "".join(out))


def ad_stackexchange(host, path):
    """Stack Overflow et consorts : un défi Cloudflare arrête tout le monde, sauf
    l'API publique (300 requêtes par jour et par adresse, sans clé)."""
    h = host[4:] if host.startswith("www.") else host
    site = SE_SITES.get(h) or (h[:-len(".stackexchange.com")] if h.endswith(".stackexchange.com")
                               else None)
    m = re.match(r"/(?:questions|q)/(\d+)", path)
    if not site or not m:
        return None
    qid = m.group(1)
    api = "https://api.stackexchange.com/2.3/questions/%s%s?site=%s&filter=withbody"
    st, qd = get_https(api % (qid, "", site))
    if st != 200:
        return None
    items = json.loads(qd).get("items") or []
    if not items:
        return None
    q = items[0]
    _, ad = get_https(api % (qid, "/answers", site) + "&sort=votes&order=desc&pagesize=30")
    reps = json.loads(ad).get("items") or []
    out = ["<h2>%s</h2>" % q.get("title", ""),
           '<p><font size="1">%s &middot; score %s &middot; %s</font></p>' % (
               html.escape(q.get("owner", {}).get("display_name", "")), q.get("score", 0),
               " ".join("[%s]" % html.escape(t) for t in q.get("tags", []))),
           "<div>%s</div><hr><h3>%d réponse(s)</h3>" % (q.get("body", ""), len(reps))]
    for r in reps:
        out.append('<p><font size="1"><b>%s</b> &middot; score %s &middot; %s</font></p>'
                   '<div>%s</div><hr>' % (
                       "&#10003; acceptée" if r.get("is_accepted") else "réponse",
                       r.get("score", 0), html.escape(r.get("owner", {}).get("display_name", "")),
                       r.get("body", "")))
    return page_simple(q.get("title", "Stack Exchange"), "".join(out))


def ad_google(host, path):
    """Recherche Google : elle exige JavaScript depuis 2025, et répond « mettez à
    jour votre navigateur » à tout le reste. Chrome sans affichage l'obtient ;
    on en tire une page de résultats en HTML simple."""
    qs = parse_qs(urlsplit(path).query)
    q = qs.get("q", [""])[0]
    if not q:
        return None
    if "tbm" in qs or "udm" in qs:          # images, actualités… : le rendu brut
        return None
    params = {"q": q, "hl": qs.get("hl", ["fr"])[0]}
    if qs.get("start"):
        params["start"] = qs["start"][0]
    dom = rendu_chrome("https://www.google.com/search?" + urlencode(params))
    if not dom or not HAVE_BS4:
        return None
    soup = BeautifulSoup(dom, "lxml")
    res, vus = [], set()
    for h3 in soup.find_all("h3"):
        a = h3.find_parent("a", href=True)
        if a is None:
            continue
        href = a["href"]
        if href.startswith("/url?") and parse_qs(urlsplit(href).query).get("q"):
            href = parse_qs(urlsplit(href).query)["q"][0]
        if href.startswith(("/goto?", "/url?")):
            # lien opaque de Google : il renvoie (302) vers la destination, et le
            # relais suit la redirection
            href = "https://www.google.com" + href
        elif not href.startswith("http") or "google." in (urlsplit(href).hostname or ""):
            continue
        if href in vus:
            continue
        vus.add(href)
        titre = h3.get_text(" ", strip=True)
        # le bloc du résultat : le premier ancêtre marqué data-hveid, qui porte
        # l'adresse lisible (<cite>) et la description (la plus longue feuille
        # de texte qui n'est ni le titre ni l'adresse)
        bloc, extrait, vu = a, "", href
        for _ in range(10):
            bloc = bloc.parent
            if bloc is None or bloc.has_attr("data-hveid"):
                break
        if bloc is not None:
            cite = bloc.find("cite")
            if cite is not None:
                vu = cite.get_text(" ", strip=True)
            for el in bloc.find_all(["div", "span"]):
                if el.find("div") is not None or el.find("h3") is not None:
                    continue
                t = " ".join(el.get_text(" ").split())
                if len(t) > len(extrait) and titre not in t and vu not in t:
                    extrait = t
        res.append((titre, href, vu, extrait[:320]))
    if not res:
        return None
    start = int(params.get("start", "0") or 0)
    lignes = ['<form action="http://www.google.com/search" method="get">'
              '<input type="text" name="q" size="50" value="%s"> '
              '<input type="submit" value="Recherche Google"></form>' % html.escape(q)]
    for titre, href, vu, ext in res:
        lignes.append('<p><a href="%s"><font size="+1">%s</font></a><br>'
                      '<font size="1" color="#006600">%s</font><br>%s</p>' % (
                          html.escape(href), html.escape(titre),
                          html.escape(vu[:90]), html.escape(ext)))
    nav = []
    if start >= 10:
        nav.append('<a href="http://www.google.com/search?%s">&laquo; précédents</a>' %
                   html.escape(urlencode({"q": q, "start": start - 10})))
    nav.append('<a href="http://www.google.com/search?%s">suivants &raquo;</a>' %
               html.escape(urlencode({"q": q, "start": start + 10})))
    lignes.append("<p>%s &middot; <a href=\"http://html.duckduckgo.com/html/?%s\">même "
                  "recherche sur DuckDuckGo</a></p>" % (" &middot; ".join(nav),
                                                       html.escape(urlencode({"q": q}))))
    return page_simple("%s - Recherche Google" % html.escape(q), "".join(lignes))


def adaptateur(host, path):
    """(statut, en-têtes, html) si un adaptateur prend la requête, sinon None."""
    if re.search(r"(^|\.)google\.[a-z.]+$", host) and path.startswith("/search"):
        try:
            doc = ad_google(host, path)
        except (OSError, ValueError) as e:
            log("adaptateur Google : %s" % e)
            doc = None
        if doc:
            return 200, [], doc
        # secours : la même recherche sur DuckDuckGo (HTML sans JavaScript)
        q = parse_qs(urlsplit(path).query).get("q", [""])[0]
        return 302, [("Location", "http://html.duckduckgo.com/html/?" + urlencode({"q": q}))], None
    try:
        if host in ("reddit.com", "www.reddit.com", "old.reddit.com", "np.reddit.com"):
            doc = ad_reddit(host, path)
        elif host.endswith(".stackexchange.com") or \
                (host[4:] if host.startswith("www.") else host) in SE_SITES:
            doc = ad_stackexchange(host, path)
        else:
            return None
    except (OSError, ValueError, ET.ParseError, http.client.HTTPException) as e:
        log("adaptateur %s%s : %s" % (host, path[:60], e))
        return None
    return (200, [], doc) if doc else None


# ───────────────────────────── pages du relais ─────────────────────────────

LEGERS = [
    ("DuckDuckGo (HTML)", "http://html.duckduckgo.com/html/"),
    ("Wikipédia", "http://fr.m.wikipedia.org/"),
    ("Hacker News", "http://news.ycombinator.com/"),
    ("Reddit (par ses flux)", "http://www.reddit.com/r/popular/"),
    ("CNN lite", "http://lite.cnn.com/"),
    ("NPR texte", "http://text.npr.org/"),
    ("Low End Mac", "http://lowendmac.com/"),
    ("MacRumors", "http://www.macrumors.com/"),
    ("Macintosh Garden", "http://macintoshgarden.org/"),
]


def page_accueil():
    rows = "".join('<li><a href="%s">%s</a></li>' % (u, html.escape(n)) for n, u in LEGERS)
    with LOCK:
        modes = sorted(CONF.modes.items())
        appris = sorted(APPRIS.items())
    reg = "".join('<tr><td>%s</td><td>%s</td><td><a href="http://%s/mode?%s">oublier</a>'
                  '</td></tr>' % (html.escape(h), MODE_NOMS[m], ICI,
                                   html.escape(urlencode({"h": h, "m": "", "u": "/"})))
                  for h, m in modes) or '<tr><td colspan="3"><i>aucun</i></td></tr>'
    app = ", ".join("%s → %s" % (html.escape(h), MODE_NOMS[m]) for h, m in appris) or "rien encore"
    outils = []
    outils.append("HTML retouché" if HAVE_BS4 else "<b>pas de BeautifulSoup</b> : liens seuls")
    outils.append("images converties" if (HAVE_PIL or CONVERT) else "<b>pas d'images converties</b>")
    outils.append("rendu par Chrome" if CHROME else "<b>pas de Chrome</b> : pas de mode rendu")
    return ("""<html><head><title>POMPPC — relais web</title></head>
<body bgcolor="#ffffff"><font face="Geneva,Helvetica,Arial">
<h2>Relais web POMPPC</h2>
<form action="http://www.google.com/search" method="get">
<input type="text" name="q" size="50"> <input type="submit" value="Recherche Google">
</form>
<form action="http://html.duckduckgo.com/html/" method="get">
<input type="text" name="q" size="50"> <input type="submit" value="DuckDuckGo">
</form>
<h3>Sites légers</h3><ul>%s</ul>
<h3>Modes</h3>
<p>Mode par défaut : <b>%s</b>. <i>sans JS</i> : la page sans ses scripts ;
<i>rendu</i> : Chrome exécute la page sur l'hôte ; <i>brut</i> : telle quelle ;
<i>auto</i> : sans JS, et rendu si la page sort vide. La barre jaune en tête de
chaque page change le mode du site.</p>
<table border="1" cellpadding="3"><tr><th>site</th><th>mode imposé</th><th></th></tr>%s</table>
<p>Appris par « auto » : %s</p>
<p><font size="1">Outils de l'hôte : %s.</font></p>
</font></body></html>""" % (rows, MODE_NOMS[CONF.defaut], reg, app, " ; ".join(outils))
    ).encode("utf-8")


# ───────────────────────────── le relais ─────────────────────────────

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "POMPPC-web-proxy"

    def log_message(self, fmt, *args):
        pass

    # ── tunnel pour CONNECT ──
    def do_CONNECT(self):
        host, _, port = self.path.partition(":")
        try:
            up = socket.create_connection((host, int(port or 443)), timeout=20)
        except OSError as e:
            self.send_error(502, "connexion impossible : %s" % e)
            return
        self.send_response(200, "Connection established")
        self.end_headers()
        conns = [self.connection, up]
        try:
            while True:
                r, _, x = select.select(conns, [], conns, 60)
                if x or not r:
                    break
                for s in r:
                    data = s.recv(65536)
                    if not data:
                        return
                    (up if s is self.connection else self.connection).sendall(data)
        except OSError:
            pass
        finally:
            up.close()

    def do_GET(self):
        self.relay()

    def do_POST(self):
        self.relay()

    def do_HEAD(self):
        self.relay()

    def do_PUT(self):
        self.relay()

    def do_DELETE(self):
        self.relay()

    def target(self):
        """(hôte, port, chemin) de la requête, forme proxy ou forme directe."""
        if self.path.startswith(("http://", "https://")):
            u = urlsplit(self.path)
            path = u.path or "/"
            if u.query:
                path += "?" + u.query
            return u.hostname, u.port, path
        host = self.headers.get("Host", "")
        h, _, p = host.partition(":")
        return h, int(p) if p else None, self.path

    def reply(self, status, ctype, data, extra=()):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(data)
            except OSError:
                pass

    # ── pages du relais : http://pomppc/ ──
    def ici(self, path):
        u = urlsplit(path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/mode":
            h, m, back = q.get("h", "").lower(), q.get("m", ""), q.get("u", "/")
            with LOCK:
                if m in MODES:
                    CONF.modes[h] = m
                else:
                    CONF.modes.pop(h, None)
                APPRIS.pop(h, None)
                CONF.save()
            log("mode de %s : %s" % (h, m or "(oublié)"))
            if back == "/":
                back = "http://%s/" % ICI
            self.reply(302, "text/html", b"", [("Location", back)])
            return
        self.reply(200, "text/html; charset=utf-8", page_accueil())

    def fetch(self, scheme, host, port, path, body, headers):
        cls = http.client.HTTPSConnection if scheme == "https" else http.client.HTTPConnection
        kw = {"context": CTX} if scheme == "https" else {}
        # connexion courte (un port 443 filtré ne doit pas bloquer le repli
        # HTTP), lecture patiente
        conn = cls(host, port, timeout=8, **kw)
        conn.connect()
        conn.sock.settimeout(40)
        conn.request(self.command, path, body=body, headers=headers)
        return conn, conn.getresponse()

    def relay(self):
        t0 = time.time()
        host, port, path = self.target()
        if not host:
            self.send_error(400, "requête sans hôte")
            return
        host = host.lower()
        if host in (ICI, "www." + ICI):
            self.ici(path)
            return
        if host in ("10.0.2.2", "localhost", "127.0.0.1"):
            self.send_error(403, "le relais ne se relaie pas lui-même")
            return
        # adaptateurs de site : une page fabriquée, ou un renvoi
        if self.command in ("GET", "HEAD"):
            ad = adaptateur(host, path)
            if ad:
                st, extra, doc = ad
                if doc is None:
                    self.reply(st, "text/html", b"", extra)
                else:
                    self.reply(st, "text/html; charset=utf-8", self.fabrique(doc, host, path))
                log("%s %s%s → %d (adaptateur, %.1f s)" % (self.command, host, path[:80], st,
                                                          time.time() - t0))
                return
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP_HEADERS}
        headers["Host"] = host if not port or port in (80, 443) else "%s:%d" % (host, port)
        headers["Accept-Encoding"] = "gzip, deflate"
        headers["Connection"] = "close"
        headers["User-Agent"] = UA_MODERNE
        headers["Accept"] = ACCEPT
        for k in ("Origin", "Referer"):
            if k in headers:
                headers[k] = headers[k].replace("http://", "https://", 1)
        mode_dem, mode = CONF.mode_de(host)

        # HTTPS d'abord (quasi tous les sites), HTTP en repli.
        with LOCK:
            plain = host in PLAIN_ONLY
        if port not in (None, 80, 443):
            tries = [("http", port)]
        elif not plain:
            tries = [("https", 443), ("http", 80)]
        else:
            tries = [("http", 80)]
        last = conn = resp = used = None
        for scheme, p in tries:
            try:
                conn, resp = self.fetch(scheme, host, p, path, body, headers)
                used = scheme
                if scheme == "http" and len(tries) > 1:
                    with LOCK:
                        PLAIN_ONLY.add(host)
                break
            except (OSError, http.client.HTTPException, ssl.SSLError) as e:
                last = e
                resp = None
        if resp is None:
            log("échec %s %s%s : %s" % (self.command, host, path, last))
            self.send_error(502, "hôte injoignable : %s" % last)
            return
        url = "%s://%s%s" % (used, headers["Host"], path)
        try:
            self.answer(conn, resp, url, host, mode_dem, mode, t0)
        finally:
            conn.close()

    def fabrique(self, doc, host, path):
        """Page d'un adaptateur : retouchée, barre de mode, UTF-8."""
        if not HAVE_BS4:
            return downgrade(doc.encode("utf-8"))
        soup = BeautifulSoup(doc, "lxml")
        retouche(soup, "sansjs", "http://%s%s" % (host, path))
        if CONF.barre and soup.body is not None:
            bar = BeautifulSoup(barre("auto", "auto", host, "http://%s%s" % (host, path))
                                .replace("POMPPC ", "POMPPC (adaptateur) ", 1), "lxml")
            tbl = bar.find("table")
            if tbl is not None:
                soup.body.insert(0, tbl)
        return serialise(soup)

    def headers_out(self, resp, drop=()):
        for k, v in resp.getheaders():
            kl = k.lower()
            if kl in DROP_HEADERS or kl in drop:
                continue
            if kl == "location":
                v = v.replace("https://", "http://", 1)
            elif kl == "set-cookie":
                v = fix_cookie(v)
            self.send_header(k, v)

    def answer(self, conn, resp, url, host, mode_dem, mode, t0):
        ctype = resp.getheader("Content-Type", "") or ""
        clen = int(resp.getheader("Content-Length") or -1)
        ct = ctype.split(";")[0].strip().lower()
        # « octet-stream » ou sans type : peut-être une image mal étiquetée, mais
        # seulement si la taille est connue et raisonnable (un gros
        # téléchargement ne doit jamais être lu en mémoire, ni tronqué)
        retouchable = ct.startswith(TEXT_TYPES) or ct.startswith("image/") or \
            (ct in ("", "application/octet-stream") and 0 <= clen <= STREAM_OVER)
        # ── gros fichiers, ou rien à retoucher : en flux ──
        if self.command == "HEAD" or (not retouchable) or clen > STREAM_OVER:
            self.send_response(resp.status, resp.reason)
            self.headers_out(resp, drop=())
            enc = resp.getheader("Content-Encoding")
            if enc:
                self.send_header("Content-Encoding", enc)
            if clen >= 0:
                self.send_header("Content-Length", str(clen))
            self.send_header("Connection", "close")
            self.end_headers()
            n = 0
            if self.command != "HEAD":
                try:
                    while True:
                        chunk = resp.read(65536)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        n += len(chunk)
                except OSError:
                    pass
            log("%s %s → %d (%d o, flux)" % (self.command, url[:90], resp.status, n))
            return
        data = resp.read(MAX_BODY)
        try:
            data = decode_body(data, resp.getheader("Content-Encoding"))
        except (OSError, zlib.error, EOFError):
            pass
        note = ""
        status = resp.status
        # ── images ──
        fmt = format_image(data) if (ct.startswith("image/") or not ct or
                                     ct == "application/octet-stream") else None
        if fmt and resp.status == 200:
            got = cache_image(url)
            if got is None:
                conv = convertit_image(data, fmt)
                got = cache_image(url, conv) if conv else None
            if got:
                ctype, data = got
                note = " %s→%s" % (fmt, ctype.split("/")[1])
        # ── documents ── (un mur anti-robots — 202, 403, 429, 503 — laisse
        # souvent passer Chrome : on tente le rendu)
        elif ct == "text/html" and HAVE_BS4 and (
                (resp.status == 200 and data) or
                (resp.status in (202, 403, 429, 503) and mode != "brut" and CHROME and
                 not RESSOURCE.search(urlsplit(url).path))):
            force = resp.status != 200
            data, ctype, note, eff = self.document(data, ctype, url, host, mode_dem,
                                                   "rendu" if force else mode)
            if force and eff == "rendu":
                status = 200
        elif data and ct.startswith(TEXT_TYPES):
            data = downgrade(data)
        self.send_response(status)
        self.headers_out(resp, drop=("content-type",))
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(data)
            except OSError:
                pass
        log("%s %s → %d (%d o%s, %.1f s)" % (self.command, url[:90], status, len(data),
                                             note, time.time() - t0))

    def document(self, data, ctype, url, host, mode_dem, mode):
        """HTML retouché selon le mode ; (données, type, note)."""
        page = "http" + url[url.index("://"):] if url.startswith("https") else url
        if mode == "brut":
            soup = BeautifulSoup(data, "lxml")
            retouche(soup, "brut", page)
            eff = "brut"
        else:
            soup = None
            eff = mode
            if mode in ("auto", "sansjs"):
                soup = BeautifulSoup(data, "lxml")
                texte = retouche(soup, "sansjs", page)
                eff = "sansjs"
                if mode == "auto" and (len(texte) < SEUIL_TEXTE or
                                       (MUR.search(texte[:4000]) and len(texte) < 3000)):
                    soup = None
            if soup is None:                        # rendu (demandé, ou « auto » déçu)
                dom = rendu_chrome("https" + url[url.index("://"):] if url.startswith("http:")
                                   and host not in PLAIN_ONLY else url)
                if dom:
                    soup = BeautifulSoup(dom, "lxml")
                    retouche(soup, "rendu", page)
                    eff = "rendu"
                    if mode == "auto":
                        with LOCK:
                            APPRIS[host] = "rendu"
                else:
                    soup = BeautifulSoup(data, "lxml")
                    retouche(soup, "sansjs", page)
                    eff = "sansjs"
        if CONF.barre and soup.body is not None:
            bar = BeautifulSoup(barre(mode_dem, eff, host, page), "lxml")
            tbl = bar.find("table")
            if tbl is not None:
                soup.body.insert(0, tbl)
        return serialise(soup), "text/html; charset=utf-8", " " + MODE_NOMS[eff], eff


def watch_parent(pid):
    """Quitte quand le processus parent (QEMU, après exec du lanceur) disparaît."""
    while True:
        time.sleep(3)
        try:
            os.kill(pid, 0)
        except OSError:
            os._exit(0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=int(os.environ.get("WEB_PROXY_PORT", 8080)))
    ap.add_argument("--parent-pid", type=int, default=0)
    a = ap.parse_args()
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    except OSError as e:
        log("port %d indisponible (%s) : relais déjà lancé ?" % (a.port, e))
        return 1
    srv.daemon_threads = True
    if a.parent_pid:
        threading.Thread(target=watch_parent, args=(a.parent_pid,), daemon=True).start()
    log("relais web sur 127.0.0.1:%d (invité : 10.0.2.2:%d) ; mode par défaut %s ; "
        "HTML %s, images %s, rendu %s" % (
            a.port, a.port, CONF.defaut, "oui" if HAVE_BS4 else "non",
            "oui" if (HAVE_PIL or CONVERT) else "non", "Chrome" if CHROME else "non"))
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
