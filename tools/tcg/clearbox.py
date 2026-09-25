#!/usr/bin/env python3
"""clearbox.py IMAGE — efface l'en-tête de la boîte d'entrée de devloop.

Le StartupItem POMPPCAgent rejoue au démarrage le dernier job resté dans la
boîte (souvent un `shutdown`) : à effacer avant chaque `devloop.py start`.
Secteurs lus dans bench/devloop/mailbox.json."""
import json, os, sys

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
box = json.load(open(os.path.join(root, "bench", "devloop", "mailbox.json")))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512)
    f.write(b"\0" * 512)
print("boîte d'entrée effacée (secteur %d)" % box["IN"])
