#!/usr/bin/env bash
# tssh.sh — ssh vers le Tiger quotidien (sshd OpenSSH 3.x : algorithmes anciens).
#   tools/guest/tssh.sh "commande"        clé privée (hors dépôt)
#   tools/guest/tssh.sh -p "commande"     mot de passe, par expect (tssh.exp)
# Clé : $TSSH_KEY, sinon <dépôt principal>/.run/cmr/id_rsa (jamais versionnée).
# Port : $TSSH_PORT (défaut 2222, redirection hostfwd de run_tiger.sh).
# Déplacé de .run/cmr/ (A5 scripts, 26/09/2026) ; .run/cmr/tssh.sh reste valable.
D="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(git -C "$D" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
KEY="${TSSH_KEY:-${MAIN:-$D/../..}/.run/cmr/id_rsa}"
PORT=${TSSH_PORT:-2222}
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
