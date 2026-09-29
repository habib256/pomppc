#!/usr/bin/env bash
# GPL3 - Copyleft VERHILLE Arnaud
# tssh.sh — ssh vers le Tiger quotidien (sshd OpenSSH 3.x : algorithmes anciens).
#   tools/guest/tssh.sh "commande"        clé privée (hors dépôt)
#   tools/guest/tssh.sh -p "commande"     mot de passe, par expect (tssh.exp)
# Clé : $TSSH_KEY, sinon <dépôt principal>/.run/cmr/id_rsa (jamais versionnée).
# Port : $TSSH_PORT, sinon celui que la VM quotidienne a publié dans
# .run/tiger.sshport (run_tiger.sh glisse à 2223… si 2222 est pris), sinon 2222.
# Déplacé de .run/cmr/ (A5 scripts, 26/09/2026) ; .run/cmr/tssh.sh (hors dépôt)
# code 2222 en dur : il vise une VM SNAPSHOT=1 si elle a pris ce port.
D="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(git -C "$D" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
KEY="${TSSH_KEY:-${MAIN:-$D/../..}/.run/cmr/id_rsa}"
PORT="${TSSH_PORT:-$(cat "${MAIN:-$D/../..}/.run/tiger.sshport" 2>/dev/null)}"
case "$PORT" in ''|0) PORT=2222 ;; esac
OPTS=(-p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
      -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa
      -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1
      -o Ciphers=+aes128-cbc,3des-cbc -o MACs=+hmac-sha1 -o ConnectTimeout=10)
if [ "${1:-}" = "-p" ]; then shift
  export TSSH_CMD="$*"; export TSSH_OPTS="${OPTS[*]}"
  exec expect -f "$D/tssh.exp"
else
  exec ssh "${OPTS[@]}" -i "$KEY" -o BatchMode=yes tiger@127.0.0.1 "$@"
fi
