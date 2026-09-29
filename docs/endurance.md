# Banc d'endurance de session (`tools/endurance/`)

Un outil qui enchaîne seul des démarrages de Tiger, et en option des cycles de lancement
et d'arrêt d'un jeu. Il détecte les paniques et les gels, range un dossier par incident
et calcule le taux de chaque type d'incident avec son intervalle de confiance.
Premier client : la « panique AppleUSBOHCI au démarrage SMP=2 » (§4).

## 1. Ce qu'il ne touche jamais

- **La VM quotidienne** (`disks/tiger.qcow2`, `.run/tiger.lock`, `.run/mon.sock`, port 2222).
  Chaque instance démarre sur un **recouvrement qcow2 neuf**
  (`.run/endurance/<campagne>-<i>/disque.qcow2`) posé sur `disks/tiger-endurance.qcow2`,
  laissé en lecture seule. Chaque démarrage à froid repart donc du même disque.
- Chaque instance a son répertoire d'exécution `POMPPC_SCRATCH=.run/endurance/<campagne>-<i>`,
  qui contient le verrou, le moniteur, `tiger.mon` et `tiger.sshport`. Son port ssh vaut
  2240 + 10·i ; `run_tiger.sh` le décale si le port est pris, et le banc lit le port
  réellement publié.
- Les instances tournent en `HEADLESS=1` (`-display none`, aucune fenêtre) avec
  `POMPPC_AUDIO_PROFILE=muet`. Ce nouveau profil de `scripts/tiger_audio.sh` passe par
  `-audiodev none` : le Screamer reste présent, la RAM reste à 768 Mo, et aucun carillon ne
  sonne. Réglages communs : `TABLET=1` (comme sous ImGuiDock), `WEBPROXY=0`, `GLISO=0`.
- `run_tiger.sh` est celui du worktree. Le QEMU par défaut est celui de `config.env`
  (`~/src/qemu`, qu'on lance sans le modifier) ; `--qemu` en désigne un autre, par exemple
  une copie d'essai dans `~/src/qemu-endurance`.

## 2. Préparation (une fois)

1. **Base** : `cp -c disks/tiger.qcow2 disks/tiger-endurance-brut.qcow2`, puis
   `qemu-img check` sur le clone. Le clone du 29/09 a été pris pendant que le QEMU quotidien
   tournait en écriture ; `qemu-img check` n'a trouvé aucune erreur. Un premier démarrage
   rejoue le journal HFS+, un `shutdown -h` propre suit, puis on renomme en
   `tiger-endurance.qcow2` et on fait `chmod a-w`.
2. **Symboles**, rangés dans `bench/endurance/invite/`, hors dépôt :
   - `mach_kernel` : `TSSH_PORT=<port> tools/guest/tssh.sh 'cat /mach_kernel' > …/mach_kernel`
     (10.4.6, xnu-792.6.70) ;
   - `endurance-syms/` : `tools/endurance/invite-syms.sh`, exécuté dans l'invité sous sudo.
     Il lance `kextload -n -s DIR -A` pour chaque kext chargé et produit un `.sym` par kext,
     lié aux adresses du démarrage courant, plus `kextstat.txt`, qui donne ces adresses de
     référence.

## 3. Lancer

    python3 tools/endurance/endurance.py demarrages -n 100 --instances 2 --mode reboot --nom ma-campagne
    python3 tools/endurance/endurance.py demarrages -n 100 --mode froid --qemu ~/src/qemu-endurance/build/qemu-system-ppc64
    python3 tools/endurance/endurance.py jeu --jeu mb -n 20 --duree 90      # cycles lancement/arrêt
    python3 tools/endurance/endurance.py jeu --jeu d3 -n 10                 # sans redémarrage : kCGLBadDisplay
    python3 tools/endurance/endurance.py rapport bench/endurance/ma-campagne
    python3 tools/endurance/endurance.py collecte <socket moniteur> <dossier>   # à la main

- `--mode froid` lance un QEMU neuf sur un disque neuf à chaque démarrage. `--mode reboot`
  fait `shutdown -r now` dans l'invité, dans le même QEMU, comme la matrice après DOOM 3.
  `--mode reset` passe par `system_reset` au moniteur.
- Un démarrage est **ok** quand le ssh répond, que `hw.ncpu` vaut `--smp` et que l'invité
  répond encore `--repos` secondes (20 par défaut) plus tard. C'est une **panique** dès que
  `panicstr` ≠ 0 dans la mémoire de l'invité. C'est un **gel** sans ssh au bout de
  `--delai` s (360 par défaut). Restent **qemu-mort** et **cpu** (nombre de processeurs
  faux).
- Résultats dans `bench/endurance/<campagne>/` : `cycles.jsonl` (une ligne par cycle),
  `rapport.md` (taux par type, intervalle de Wilson à 95 %), `incidents/<n°>-<type>/`.

## 4. Ce que contient un dossier d'incident

VM arrêtée (`stop`) pendant la collecte :

- `ecran.png` : screendump.
- `registres.txt` : `info registers -a`, tous les vCPU.
- `registres-2.txt` : même relevé 3 s après `cont`, pour voir si les vCPU avancent.
- `echantillons.json` : 20 relevés de NIP, LR, SRR0/1, DAR, DSISR et MSR, à 0,1 s
  d'intervalle.
- `qemu-int.log` : 0,3 s de `log int,mmu` de QEMU.
- `panique.txt` : le texte complet de la panique, lu dans `debug_buf`, c'est-à-dire ce
  que l'invité écrirait en NVRAM. `panicstr` figure dans `incident.json`.
- `kmods.txt` : la liste `kmod` du noyau lue en mémoire (nom, base, taille). Les kexts
  chargés au démarrage sont V=R, donc leur `kmod_info` se lit en physique.
- `vues.txt` : les registres de l'OHCI vus côté device (`xp` sur sa BAR) et, pour chaque
  vCPU, la page de son DAR traduite par sa table de pages (`gva2gpa`, `x`).
- `pci.txt`, `qemu.log`.
- `panic.log` : après la collecte, `system_reset`, puis lecture de
  `/Library/Logs/panic.log` au démarrage suivant (`--sans-panic-log` pour s'en passer).
- `incident.md` : le résumé. PC, LR, SRR0, CTR et la chaîne des cadres (`r1`) de chaque
  vCPU y sont symbolisés, ainsi que toutes les adresses du texte de panique.

Symbolisation (`tools/endurance/symbolise.py`) : une adresse du noyau se cherche avec
`nm -n` sur `mach_kernel`. Une adresse dans un kext se traduit en décalage depuis la base
du kext **lors de l'incident** (liste `kmod`), puis en symbole à la base de référence plus ce
décalage dans le `.sym`. Les bases varient d'un démarrage à l'autre, parce que l'ordre de
chargement suit l'appariement IOKit.

    python3 tools/endurance/symbolise.py 0x46cd7c --kmods bench/endurance/X/incidents/Y/kmods.txt

## 5. Campagnes et résultats

Voir §6. Les journaux restent hors dépôt, dans `bench/endurance/<campagne>/`.
