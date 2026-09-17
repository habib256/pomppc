#!/bin/sh
# proxy.sh — règle le proxy web de Tiger sur le relais de l'hôte (scripts/web-proxy.py).
#
#   sudo sh proxy.sh on [hôte] [port]   # défaut : 10.0.2.2 8080 (hôte QEMU)
#   sudo sh proxy.sh off
#   sh proxy.sh status
#
# Équivaut à Préférences Système → Réseau → (interface) → Proxys → « Proxy web
# (HTTP) », pour toutes les interfaces. Tiger n'a pas networksetup : le script
# édite /Library/Preferences/SystemConfiguration/preferences.plist avec le
# Python 2.3 du système, puis réapplique l'emplacement courant (scselect).
# Seul le proxy HTTP est réglé : le relais va chercher les pages en HTTPS
# moderne et les rend en HTTP. Taper les adresses en http:// dans Safari.
P=/Library/Preferences/SystemConfiguration/preferences.plist
cmd=${1:-status}
host=${2:-10.0.2.2}
port=${3:-8080}

if [ "$cmd" = status ]; then
  scutil --proxy
  exit 0
fi
if [ "$(id -u)" != 0 ]; then
  echo "à lancer avec sudo" >&2
  exit 1
fi
case "$cmd" in on|off) ;; *) echo "usage : proxy.sh on|off|status [hôte] [port]" >&2; exit 1 ;; esac

cp -p "$P" "$P.pomppc-avant-proxy" 2>/dev/null
/usr/bin/python - "$P" "$cmd" "$host" "$port" <<'PY'
# -*- coding: utf-8 -*-
# Python 2.3 (Tiger) : plistlib n'a que Plist.fromFile / Plist.write
import sys, plistlib
path, cmd, host, port = sys.argv[1:5]
if hasattr(plistlib, 'readPlist'):
    pl = plistlib.readPlist(path)
else:
    pl = plistlib.Plist.fromFile(path)
n = 0
def patch(svc):
    global n
    if not hasattr(svc, 'has_key') or not svc.has_key('Interface'):
        return
    px = svc.get('Proxies') or {}
    if cmd == 'on':
        px['HTTPEnable'] = 1
        px['HTTPProxy'] = host
        px['HTTPPort'] = int(port)
        px['ExceptionsList'] = ['*.local', '169.254/16', '10.0.2.0/24']
    else:
        px['HTTPEnable'] = 0
    svc['Proxies'] = px
    n = n + 1
for svc in pl.get('NetworkServices', {}).values():
    patch(svc)
# services décrits directement dans un emplacement (et non par lien)
for s in pl.get('Sets', {}).values():
    for svc in s.get('Network', {}).get('Service', {}).values():
        if not svc.has_key('__LINK__'):
            patch(svc)
if hasattr(plistlib, 'writePlist'):
    plistlib.writePlist(pl, path)
else:
    pl.write(path)
print "%d service(s) reseau modifie(s)" % n
PY
[ $? = 0 ] || exit 1
cur=$(/usr/bin/python -c "import plistlib;print plistlib.Plist.fromFile('$P')['CurrentSet'].split('/')[-1]")
scselect "$cur" >/dev/null 2>&1
sleep 2
scutil --proxy | grep -E "HTTPEnable|HTTPProxy|HTTPPort"
