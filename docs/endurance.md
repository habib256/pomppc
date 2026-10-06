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
- `run_tiger.sh` est celui du worktree. Le QEMU par défaut est celui que choisit le
  lanceur : `~/src/qemu/build` (config.env) sur le M4, le binaire rapide
  `~/src/qemu/build-fast` sur le PC (scripts/qemu_fast.sh) ; on le lance sans le modifier.
  `--qemu` en désigne un autre, par exemple une copie d'essai dans `~/src/qemu-endurance`.

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

### 2 bis. Sur le PC Linux (porté le 06/10/2026)

Ce qui changeait sous Linux, et ce que le banc fait désormais :

- **Symboles.** Le `nm` de binutils ne lit pas le Mach-O (« file format not recognized ») et
  rendait une table **vide sans erreur** : `panicstr` introuvable, aucune panique vue, rien de
  symbolisé. `symbolise.py` et `tools/re/kpanic.py` lisent maintenant la table de symboles
  en Python (`tools/re/machonm.py -n`, même sortie que `nm -n` d'Apple) ; `nm` reste le repli.
- **Un dossier de symboles par noyau.** La VM du PC est en 10.4.11 (Darwin 8.11.0) ; le banc
  du M4 a pris le 10.4.6. `--invite DOSSIER` ou `ENDURANCE_INVITE` (défaut
  `bench/endurance/invite/`) ; au premier démarrage, le banc compare `uname -v` de l'invité
  au noyau des symboles et **arrête la campagne** s'ils diffèrent. `kpanic.py` compare de
  même la chaîne `version` en mémoire à celle du fichier et refuse un faux noyau (la copie
  `.run/mach_kernel` de la matrice du PC était restée celle du 10.4.6 :
  `tools/matrice/hote.py` la reprend désormais quand elle n'est plus celle de l'invité).
- **Rien n'est en dur dans le noyau** sauf ce que lit `kpanic.py` dans le cadre de `panic()`
  (arguments à `r1+0x9c`, format dans `r24` sur le 10.4.6) : ces deux valeurs sont maintenant
  **décodées dans le prologue de `_panic`** du noyau donné (`stw r4,d(r1)`, `mr rN,r3`), et
  la sortie dit si elles ont été lues ou supposées.
- **Panique vue même quand `panicstr` est retombé à 0.** `panic()` le remet à zéro au retour
  de `Debugger()`, avant sa boucle finale (`kpanic.py`) : le banc tient aussi pour panique un
  vCPU dont le PC est dans `panic()` (de `_panic` au symbole suivant). Sans cela, une panique
  passait pour un gel.
- **Texte de la panique.** Sans boot-arg de débogage, Tiger n'alloue pas `debug_buf` :
  `panique.txt` reste vide. Chaque collecte lance donc `kpanic.py` VM arrêtée
  (`kpanic.txt` : texte reconstitué, appelant, pile, fin du msgbuf) ; le texte va dans
  `incident.md` et dans le résumé du rapport.
- **Image** : `ecran.png` par Pillow, sinon ImageMagick (`sips` sous macOS).
- **Binaire.** Sur le PC, `run_tiger.sh` prend le binaire **rapide** `build-fast/` (PGO), pas
  `build/` : le banc lit la bannière du lanceur (`binaire : …`, ligne `▶ Tiger …` avec les
  leviers allumés, backend qgpu) et range binaire, empreinte et leviers dans chaque cycle.
- **ssh sans connexion maîtresse** (`TSSH_MUX=0`) : une maîtresse restée sur un invité qui
  redémarre ou qui gèle fait attendre chaque ssh, et `tssh.sh` la range dans `.run/` du dépôt
  principal.
- **Affichage.** `HEADLESS=1` (`-display none`) : le backend GL de qgpu ouvre son propre
  contexte EGL (pbuffer 1×1, `eglGetDisplay(EGL_DEFAULT_DISPLAY)`), indépendant de
  l'affichage de QEMU, et la présentation écrit dans l'écran VGA, que `screendump` lit. Le
  sondage du lanceur se fait déjà en `-display none`. Une campagne de jeu **s'arrête** si le
  backend annoncé n'est pas `gl` ; `--fenetre` est le repli (fenêtre QEMU native).
- **Verrous** : `flock` (scripts/hostcompat.sh), un par instance dans son
  `.run/endurance/<campagne>-<i>/`. Jamais de suppression d'un verrou.

**Base sur le PC** (ext4, pas de clone) : VM quotidienne **arrêtée**, puis
`cp --sparse=always disks/tiger.qcow2 disks/tiger-endurance-brut.qcow2` (~8,8 Go),
`~/src/qemu/build/qemu-img check`, un démarrage (`SNAPSHOT=` vide, `DISK=` la copie), vérifier
`pmset -g` (sleep, displaysleep, disksleep à 0 : l'invité 10.4.11 s'endort sinon), arrêt propre,
renommage en `tiger-endurance.qcow2`, `chmod a-w`. Symboles : `cat /mach_kernel` de cet invité
dans `bench/endurance/invite-10.4.11/mach_kernel`, `invite-syms.sh` dans
`…/invite-10.4.11/endurance-syms/`.

## 3. Lancer

    python3 tools/endurance/endurance.py demarrages -n 100 --instances 2 --mode reboot --nom ma-campagne
    python3 tools/endurance/endurance.py demarrages -n 100 --mode froid --qemu ~/src/qemu-endurance/build/qemu-system-ppc64
    python3 tools/endurance/endurance.py jeu --jeu mb -n 20 --duree 90      # cycles lancement/arrêt
    python3 tools/endurance/endurance.py jeu --jeu d3 -n 10 --redemarre jamais   # kCGLBadDisplay
    python3 tools/endurance/endurance.py jeu --jeu mb,zen,ut,d3 --instances 2 -n 999 --jusqua 07:00 \
        --invite bench/endurance/invite-10.4.11 --nom pc-jeux        # le PC, toute une nuit
    python3 tools/endurance/endurance.py rapport bench/endurance/ma-campagne
    python3 tools/endurance/endurance.py collecte <socket moniteur> <dossier>   # à la main

- `--jusqua HH:MM` : aucun cycle ne commence après cette heure ; `touch
  bench/endurance/<campagne>/ARRET` arrête proprement. Relancer avec le même `--nom` **ajoute**
  des cycles (tranches) ; le banc refuse d'y mêler un autre bras.
- `--anciens-defauts` : le bras témoin d'avant le 06/10 sur le PC (`QEMU_FAST=0` et les leviers
  TCG de ce jour-là à 0) ; `--env K=V` passe toute autre variable à `run_tiger.sh`.
- Jeux : `--jeu` prend une liste, jouée à tour de rôle ; réglages préparés comme la matrice
  (`preparer`, `lance.command`, `POMPPC_GL_STATS/NOTE/FRAMES`) ; durée par jeu (mb 150 s,
  zen 120, ut 300, d3 600). `--redemarre toujours` (défaut) fait `shutdown -r` entre deux
  cycles et compte ce redémarrage comme un démarrage ; un client du kext resté ouvert
  redémarre toujours. États d'un cycle : ok, panique, gel, `jeu-fige` (frames.csv immobile,
  l'invité répond), `sortie-du-jeu` (état de sortie, stdout et rapport de CrashReporter dans
  `jeu.txt`), `pas-lance`, qemu-mort. Après panique, gel ou jeu figé : QEMU neuf.
- La charge de l'hôte (1 min) et le nombre de QEMU mac99 sont relevés toutes les 10 s
  (`charge.csv`) et rangés avec chaque cycle (début, max, fin).

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
  que l'invité écrirait en NVRAM (vide sans boot-arg de débogage). `panicstr` figure dans
  `incident.json`.
- `kpanic.txt` : `tools/re/kpanic.py`, VM arrêtée — texte reconstitué depuis la pile de
  `panic()`, appelant, pile symbolisée, fin du msgbuf.
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

## 5. Campagnes du 29/09/2026

SMP=2, base du 29/09, hôte partagé (deux autres VM). Journaux hors dépôt, dans
`bench/endurance/<campagne>/` (`rapport.md`, `cycles.jsonl`, `incidents/`).

| Campagne | QEMU | mode | n | incidents | taux (IC 95 %) |
|---|---|---|---|---|---|
| `ref-smp2-a` | référence 15:10 (avant OpenPIC) | froid | 100 | 0 | 0 % (0–3,7) |
| `ref-smp2-reboot` | référence 15:10 | reboot | 100 | 2 gels OHCI (#44, #74) | 2,0 % (0,6–7,0) |
| `ancien-smp2-reboot` | patch SMP d'avant M1 (888f45d^) | reboot | 42 | 0 | 0 % (0–8,4) |
| `trace-openpic-reboot` | 15:10 + trace OpenPIC | reboot | 85 | 1 gel OHCI (#78) | 1,2 % (0,2–6,4) |
| `avant-trace-reset` | 15:10 + trace OpenPIC | reset | 32 | 0 | 0 % (0–10,7) |
| `apres-openpic-reboot` | référence 17:35:50 (correctif OpenPIC, sha256 b9ccbfa7…) | reboot | 150 | 0 gel OHCI ; 1 « gel après ssh » (#40), faux positif probable ¹ | 0 % (0–2,5) |
| `apres-trace-reboot` | correctif OpenPIC + trace | reboot | 103 | 0 (2 déclencheurs « IRQ 28 pending au reset » traversés) | 0 % (0–3,6) |

¹ #40 : le bureau est intact (capture) et les deux vCPU sont au repos, avec des appels
système qui passent (`qemu-int.log`). Aucune tempête d'interruptions : un seul ssh de 20 s
s'est perdu sur un hôte chargé. Depuis a87b764, un gel après le bureau exige trois ssh ratés.

Cycles de jeu (`jeu --jeu mb`, 2 cycles de 60 s) : lancement, jeu vu, arrêt, Terminal
fermé, sans incident. C'est la vérification de l'outil, pas encore une campagne.

## 6. Le « panique AppleUSBOHCI au démarrage » : un gel, et sa cause

**Ce qu'on voit.** Le démarrage à froid ne le montre jamais (0/100). Le redémarrage
dans le même QEMU (`shutdown -r`, ce que fait la matrice) le montre environ une fois sur
50 à 80. Aucune panique (`panicstr` = 0) ; l'invité se fige au démarrage :

- CPU 0, EE coupé (MSR 0x1030), tourne entre `AppleUSBOHCI::FilterInterrupt` (+0x24 à
  +0x3c) et `AppleMPICInterruptController::handleInterrupt`. La pile passe par
  `IOFilterInterruptEventSource::disableInterruptOccurred`, `IOCPUInterruptController::handleInterrupt`,
  `interrupt`, puis `IOInterruptController::enableInterrupt` ← `IOWorkLoop::enableAllInterrupts` :
  l'interruption arrive au moment même où le pilote OHCI s'active.
- CPU 1 au repos (`machine_idle`).
- Registres lus par le filtre : `HcInterruptEnable` = MIE seul, `HcInterruptStatus` = RHSC.
  L'OHCI ne demande donc rien, le filtre rend « pas pour moi », et l'IRQ 28 revient
  aussitôt, sans fin.

**La cause, dans QEMU (`hw/intc/openpic.c`).** `openpic_reset()` ne remet pas
`src->pending` à zéro et repasse chaque source en front (`ivpr_reset` de KeyLargo). Au
reset machine, le sous-arbre de macio, donc l'OpenPIC, est remis à zéro **avant** le
bus PCI. `pcibus_reset_hold()` baisse ensuite la ligne INTx de l'OHCI, mais sur une source
désormais en front la baisse est ignorée et `pending` reste à 1. Si la ligne OHCI était
haute à l'instant du `shutdown -r`, le Tiger suivant repasse la source 28 au niveau, la
démasque, et elle reste active pour toujours.

**Preuve.** Campagne tracée (`trace-openpic-reboot`, `fprintf` dans `openpic_reset` et
`openpic_set_irq`), 85 redémarrages. La séquence « `IRQ 28 pending=1 level=1` au reset,
puis `IRQ 28 baissée, source en front, pending reste 1` » apparaît **une seule fois**,
dans l'instance 1, juste avant son unique gel (#78, même signature). L'instance 0 n'a
ni cette séquence ni gel. Le même mécanisme, après une panique suivie d'un `system_reset`
(« using 1966 buffer headers »), a été établi en parallèle par l'agent DOOM 3.

**Correctif.** `patches/openpic/0001-openpic-reset-niveau.patch` (branche de l'agent
DOOM 3, étape « 1 bis » de `scripts/build_qemu_qfb.sh`) : `pending` repasse à 0 au reset,
et une source de niveau reprend l'état réel de sa ligne (`input`) quand l'invité la
repasse au niveau. Le banc a d'abord écrit le même correctif (la seule remise à zéro),
puis l'a retiré au profit de celui-là.

**Contrôle après correction.** `apres-openpic-reboot` : 0 gel OHCI sur 150 redémarrages
(IC 0–2,5 %) ; `apres-trace-reboot` (correctif + la même trace) : 0 gel sur 103 redémarrages, alors que la condition de déclenchement, « IRQ 28 pending=1 level=1 » au reset, s'y est produite **deux fois**. Les deux redémarrages suivants ont atteint le bureau. Avant correction, cette condition avait donné un gel dans le seul cas observé (1 sur 1). Au total, avant correction : 3 gels sur 185 `shutdown -r` ; après : 0 sur 253 (IC 0–1,5 %).

**Et la « panique » d'origine ?** Le 24/09, elle était notée « panique au boot
(AppleUSBOHCI, cpu 1) ». Hypothèse, non reproduite ici : même tempête sur le CPU 0, et le
CPU 1 qui attend un verrou simple tenu par le CPU 0 finit en `simple lock deadlock` /
`Lock timeout` au nom d'AppleUSBOHCI. Les 42 redémarrages sous l'ancien patch SMP (d'avant
M1) n'ont donné ni panique ni gel : M1 n'explique pas le « 1 sur 10 » d'alors. La
fréquence de l'époque (un sur dix au lieu d'un sur 50 à 80) peut venir de la VM quotidienne
(ImGuiDock, son, tablette, redémarrage juste après un jeu qui laisse l'OHCI occupé) ;
non mesuré.

**À savoir.** `info pic` n'est pas disponible pour l'OpenPIC de mac99 dans ce QEMU
(« Interrupt controller information not available »). L'état de la source 28 se lit donc
par la trace ci-dessus, ou dans `vues.txt` (registres de l'OHCI côté device).
