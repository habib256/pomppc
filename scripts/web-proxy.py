#!/usr/bin/env python3
"""web-proxy.py — relais web pour les vieux navigateurs de l'invité (Tiger, OS 9).

Tiger n'a qu'OpenSSL 0.9.7 et des certificats racines de 2006 : les sites
actuels (HTTPS, TLS 1.2+) lui sont inaccessibles. Ce relais tourne sur l'hôte
et parle HTTP simple à l'invité ; lui va chercher les pages en HTTPS moderne,
avec la vérification de certificats de l'hôte, et réécrit « https:// » en
« http:// » dans les en-têtes et les pages pour que la navigation continue à
passer par lui.

    python3 scripts/web-proxy.py [--port 8080] [--parent-pid PID]

Dans l'invité (QEMU en réseau utilisateur), l'hôte est 10.0.2.2 :
    Préférences Système → Réseau → Proxys → Proxy web (HTTP) : 10.0.2.2, port 8080
    ou : curl -x 10.0.2.2:8080 http://www.example.com/

Le relais n'écoute que sur 127.0.0.1 : il n'est joignable que depuis l'hôte et
ses invités QEMU, jamais depuis le réseau local. CONNECT est relayé tel quel
(tunnel) pour les rares sites qui acceptent encore le TLS de Tiger.

Bibliothèque standard seulement. Les pages sont transmises entières (pas de
flux) : suffisant pour naviguer, pas pour de la vidéo.
"""
import argparse
import gzip
import http.client
import os
import re
import select
import socket
import ssl
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

TEXT_TYPES = ("text/", "application/javascript", "application/x-javascript",
              "application/json", "application/xhtml", "application/xml",
              "application/rss", "application/atom", "image/svg")
DROP_HEADERS = {"strict-transport-security", "content-security-policy",
                "content-security-policy-report-only", "alt-svc",
                "transfer-encoding", "content-encoding", "content-length",
                "connection", "keep-alive", "public-key-pins", "expect-ct"}
HOP_HEADERS = {"proxy-connection", "connection", "keep-alive", "te", "trailer",
               "upgrade", "proxy-authorization", "accept-encoding"}
MAX_BODY = 64 * 1024 * 1024
# Hôtes qui ne parlent qu'HTTP (appris au vol) : on ne retente pas HTTPS.
PLAIN_ONLY = set()
LOCK = threading.Lock()
CTX = ssl.create_default_context()


def log(msg):
    sys.stderr.write(time.strftime("%H:%M:%S ") + msg + "\n")
    sys.stderr.flush()


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

    def fetch(self, scheme, host, port, path, body, headers):
        cls = http.client.HTTPSConnection if scheme == "https" else http.client.HTTPConnection
        kw = {"context": CTX} if scheme == "https" else {}
        # connexion courte (un port 443 filtré ne doit pas bloquer le repli
        # HTTP), lecture patiente
        conn = cls(host, port, timeout=8, **kw)
        conn.connect()
        conn.sock.settimeout(40)
        conn.request(self.command, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read(MAX_BODY) if self.command != "HEAD" else b""
        conn.close()
        return resp, data

    def relay(self):
        host, port, path = self.target()
        if not host:
            self.send_error(400, "requête sans hôte")
            return
        if host in ("10.0.2.2", "localhost", "127.0.0.1"):
            self.send_error(403, "le relais ne se relaie pas lui-même")
            return
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP_HEADERS}
        headers["Host"] = host if not port or port in (80, 443) else "%s:%d" % (host, port)
        headers["Accept-Encoding"] = "gzip, deflate"
        headers["Connection"] = "close"
        for k in ("Origin", "Referer"):
            if k in headers:
                headers[k] = headers[k].replace("http://", "https://", 1)

        # HTTPS d'abord (quasi tous les sites), HTTP en repli.
        tries = []
        with LOCK:
            plain = host in PLAIN_ONLY
        if port not in (None, 80, 443):
            tries = [("http", port)]
        elif not plain:
            tries = [("https", 443), ("http", 80)]
        else:
            tries = [("http", 80)]
        last = None
        used = None
        for scheme, p in tries:
            try:
                resp, data = self.fetch(scheme, host, p, path, body, headers)
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

        ctype = resp.getheader("Content-Type", "")
        try:
            data = decode_body(data, resp.getheader("Content-Encoding"))
        except (OSError, zlib.error, EOFError):
            pass
        if data and ctype.startswith(TEXT_TYPES):
            data = downgrade(data)

        self.send_response(resp.status, resp.reason)
        for k, v in resp.getheaders():
            kl = k.lower()
            if kl in DROP_HEADERS:
                continue
            if kl == "location":
                v = v.replace("https://", "http://", 1)
            elif kl == "set-cookie":
                v = fix_cookie(v)
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(data)
            except OSError:
                pass
        log("%s %s://%s%s → %d (%d o)" % (self.command, used, host, path[:80],
                                          resp.status, len(data)))


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
    log("relais web sur 127.0.0.1:%d (invité : 10.0.2.2:%d)" % (a.port, a.port))
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
