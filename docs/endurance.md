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

## 7. PC Linux, nuit du 06 au 07/10/2026 : la configuration par défaut du 06/10

**Cadre.** i7-10700F, 16 fils, 39 Go, RTX 4060 Ti (EGL/NVIDIA). Base
`disks/tiger-endurance.qcow2`, copiée le 07/10 à 02:33 de la VM quotidienne arrêtée
(`qemu-img check` sans erreur), **Tiger 10.4.11** (Darwin 8.11.0, xnu-792.24.17), `pmset` à 0,
symboles dans `bench/endurance/invite-10.4.11/` (`mach_kernel` et 54 `.sym` de kexts).
SMP=2, `HEADLESS=1` : le backend GL de qgpu a pris `gl` sur la RTX dans toutes les instances,
sans fenêtre (`--fenetre` n'a pas servi). Banc au commit 954c452, puis 108e339 (sonde ssh) à
partir de 03:21. Journaux dans `bench/endurance/{pc-reboot,pc-reboot-temoin,pc-jeux}/`, plus
`charge-etrangere-0707.txt` (tools/tcg/chargehote.py) et `thermique-0707.txt`.

**Les deux bras.**

- **Défaut** : binaire **rapide** `build-fast/` (PGO, 06/10 19:57, sha256 4dd53b13…) et tous
  les leviers du lanceur (TLB-PRÉCIS, LMW-EN-LIGNE, DCBZ-EN-LIGNE, JIT À 2 GIO, comparaisons
  natives scalaires et AltiVec, LMW-STMW-VECTEUR, CACHE DE SAUTS PPC, plus les plus anciens). Le
  binaire a dû être **imposé** (`--qemu`) : voir « Surprises ».
- **Témoin** (`--anciens-defauts`) : binaire de référence `build/` (06/10 15:58) et les leviers
  du 06/10 éteints (`QEMU_FAST=0 TLBPRECISE=0 LMWINLINE=0 DCBZINLINE=0 JITREL32=0
  FPNATIVECMP=0 VFPNATIVECMP=0 LMWVEC=0 JCWORD=0`).

**Déroulé.** Essai court à 02:35 (3 redémarrages, 2 parties mb et d3, sans incident), puis, de
02:51 à 06:52, en parallèle : 3 instances de redémarrages (`shutdown -r` dans l'invité) en
tranches alternées (défaut 02:51-03:21, témoin 03:21-04:15, défaut 04:15-05:10, témoin
05:11-06:00, défaut 06:00-06:45), et 2 instances de jeux (mb, zen, ut, d3 à tour de rôle,
`shutdown -r` entre deux parties, bras défaut seulement). Cinq VM ensemble : charge 1 min
médiane 9,4 à 9,9 (maximum 16,7), charge étrangère médiane 0,04 cœur (maximum 6,5 : un
`qgpu_replay` d'une autre session vers 04:49). Pas de tranche à froid : pas le temps.

**Durées réelles.** Démarrage jusqu'au ssh, médiane **35 s** (15 à 160 s) ; un cycle de
redémarrage complet prend environ 1 min ; environ 120 redémarrages par heure sur 3 instances en
défaut, environ 140 en témoin. Partie : mb 150 s, zen 120 s, ut 300 s, d3 600 s, plus environ
1 min de redémarrage, soit environ 16 parties par heure sur 2 instances.

### Résultats

| campagne | bras | cycles | incidents | taux | IC 95 % (Wilson) |
|---|---|---|---|---|---|
| `pc-reboot` | défaut | 241 démarrages (228 reboot, 13 à froid) | 4 « gel » (ssh refusé, §7.1) | 1,7 % | 0,6 – 4,2 % |
| `pc-jeux` | défaut | 64 démarrages (59 reboot, 5 à froid) | 1 « gel » (ssh refusé, §7.1) | 1,6 % | 0,3 – 8,3 % |
| `pc-jeux` | défaut | **62 parties** | 0 | 0 % | 0 – 5,8 % |
| `pc-reboot-temoin` | témoin | 238 démarrages (231 reboot, 7 à froid) | 1 panique (§7.2) | 0,4 % | 0,1 – 2,3 % |

Par jeu (défaut, 0 incident chacun) : DOOM 3 15 parties (IC 0 – 20,4 %), Marble Blast 16
(0 – 19,4 %), UT2004 15 (0 – 20,4 %), Zenerchi 16 (0 – 19,4 %). Aucune panique, aucun gel,
aucune sortie de jeu (`exit 139` compris) et aucun jeu figé en 62 parties.

Paniques, tous démarrages du bras défaut : **0 sur 305** (IC 0 – 1,2 %).

**Comparaison des bras, redémarrages seulement** (`endurance.py compare --depart reboot`) :
défaut 4/228 (1,8 %, IC 0,7 – 4,4 %) contre témoin 1/231 (0,4 %, IC 0,1 – 2,4 %). Fisher
exact bilatéral **p = 0,21** (0,37 tous départs confondus). Les intervalles se recouvrent :
**rien ne permet de dire que le taux a bougé avec les leviers du 06/10**. Ce n'est pas non plus
la preuve qu'il n'a pas bougé : avec environ 230 cycles par bras, on ne distinguait guère que
5 % contre 0 %. Et les incidents des deux bras ne sont pas de la même nature (ci-dessous).

### 7.1 Les cinq « gels » du bras défaut : invité vivant, sshd muet

Les cinq ont la même signature : `pc-reboot` #42, #49, #83 et #202, `pc-jeux` d0022, chacun
après un `shutdown -r`, sur trois instances différentes.

- Aucun ssh en 360 s, mais le **bureau est intact** (Finder, horloge à l'heure de la collecte).
- Les **deux vCPU sont au repos** (`_machine_idle+0x194`, EE=1, 20 relevés sur 20), sans panique
  (`panicstr` = 0, aucun vCPU dans `panic()`).
- Le msgbuf finit normalement (chargement de POMPPCFsqrt).
- **La sonde** (#83, #202, d0022, à partir du commit 108e339) : la connexion au port redirigé
  aboutit (NAT de QEMU), puis se ferme **sans bannière** ; un ssh de 90 s échoue aussi. Le port 22
  de l'invité ne répond donc pas, alors que sa pile IP vit (`info usernet` : trafic mDNS de
  10.0.2.15).
- `system.log` du démarrage figé, relu après le reset : rien d'anormal ; il s'arrête au
  chargement de POMPPCFsqrt, comme un démarrage normal (sshd, lancé à la demande par launchd,
  n'y écrit rien).
- Un `system_reset` rend le ssh en 45 à 85 s.
- Charge pendant ces cycles : 10,1 à 14,4.

**Ce que c'est, et ce que ce n'est pas.** Ce n'est pas une panique ni un gel du noyau. Ce n'est
pas non plus le gel OHCI du §6, dont la signature est un vCPU qui tourne, EE coupé, dans
`AppleUSBOHCI::FilterInterrupt`. C'est un démarrage **sans service ssh**, l'interface restant
vivante. La cause est **inconnue** : `launchd` qui n'arme pas son socket de sshd ? un
redémarrage à chaud dans le même QEMU ? Le banc ne voit l'invité que par ssh ; pour trancher, il
faudrait un second canal (console série, ou un service témoin sur un autre port). Ce type
d'incident compte bien pour un utilisateur (« le Mac démarre mais la session à distance ne
répond pas »), mais il ne relève pas des « plantages en jeu sans cause ». Ce sont 5 cas sur 305
démarrages du bras défaut, 0 sur 238 du témoin (Fisher p ≈ 0,07 sur ce seul type). C'est la
piste la plus nette de la nuit, et rien de plus qu'une piste.

### 7.2 La panique du bras témoin : appel par un pointeur nul pendant le démarrage

`pc-reboot-temoin` #160, binaire de référence `build/`, leviers du 06/10 éteints, 20 s après
le `shutdown -r`. Détectée alors que `panicstr` était déjà retombé à 0 : le vCPU 0 était dans
`panic()`. Texte, relu par `kpanic.py` et sur l'écran :

    panic: 0x400 - Inst access
    Exception state: PC=0x00000000 MSR=0x40008030 DAR=0x01672000 DSISR=0x40000000
                     LR=0x002DFED4 R1=0x11E3B9D0 XCP=0x00000010 (0x400 - Inst access)
    IOPlatformExpert::CheckSubTree+0x12c ← CheckSubTree+0x304 (×2)
      ← Core99PE::PMRegisterDevice+0x5c (AppleCore99PE)
      ← IOSCSIProtocolInterface::InitializePowerManagement+0x58
      ← IOSCSIProtocolServices::InitializePowerManagement+0x2c
      ← IOATAPIProtocolTransport::start+0x3ec ← IOService::startCandidate ← probeCandidates
      ← doServiceMatch ← _IOConfigThread::main
    panic: We are hanging here...

C'est un saut à l'adresse 0 depuis `CheckSubTree` pendant l'appariement IOKit du lecteur ATAPI
(enregistrement auprès de la gestion d'énergie). Une seule occurrence en 543 démarrages des deux
bras. Ce n'est **ni un LockTimeOut, ni le gel OHCI**, et ce n'est pas sur la configuration par
défaut. Dossier complet dans `incidents/0160-panique/` (`kpanic.txt`, `ecran.png`, pile
symbolisée avec les kexts).

### 7.3 Ce que ça dit, et pas plus

- Bras défaut (binaire rapide et leviers du 06/10) :
  - **0 panique sur 305 démarrages** (IC 0 – 1,2 %) ;
  - **0 incident sur 62 parties** (IC 0 – 5,8 % ; par jeu, 0 sur 15 ou 16, IC jusqu'à 20 %) ;
  - 5 démarrages sans ssh, soit 1,6 % (IC 0,7 – 3,8 %).
- Bras témoin : 1 panique sur 238 démarrages (0,4 %, IC 0,1 – 2,3 %), 0 démarrage sans ssh.
- Aucun LockTimeOut et aucun `exit 139` de DOOM 3 cette nuit : le banc ne les a pas reproduits
  en 15 parties de DOOM 3 et 543 démarrages. « Plantages en jeu sans cause » reste ouvert, sans
  nouveau témoin.
- La nuit ne dit pas si les leviers du 06/10 changent le taux d'incident : différence non
  significative (p = 0,21), incidents de natures différentes d'un bras à l'autre.

### 7.4 Surprises

- **Le lanceur ne prend plus le binaire rapide depuis le 06/10 vers 22:07.** `scripts/qemu_fast.sh`
  l'écarte parce que la série de patches a changé depuis sa construction (75bb094, tcg/0037-0039
  ajoutés après 19:57) : « construit sur une autre série de patches que ce dépôt ». Il retombe
  alors sur `build/`, qui date de 15:58, plus vieux encore et sans relevé de construction. Toute
  session lancée depuis `main` après 22:07 tourne donc sur la référence, malgré « build-fast par
  défaut ». Cette nuit, le bras défaut a imposé `build-fast` par `--qemu`. À faire :
  reconstruire `build-fast` (ou `build/`) sur la série courante.
- **Thermique.** Le paquet du processeur est resté à **97-100 °C** toute la nuit, avec une
  fréquence moyenne vers 3,9 GHz (relevé minute par minute). Les images par partie baissent au
  fil de la nuit (d3 : 7 517 images en 600 s à l'essai court, charge 3,5, puis une médiane de
  3 453 sous une charge d'environ 10 et jusqu'à 1 894), même sur un QEMU tout juste relancé.
  Charge et thermique se mêlent : ce n'est pas une mesure de vitesse.
- **Liste `kmod` sur le 10.4.11** : la tête de liste (POMPPCFsqrt, chargé en dernier) se lit en
  physique, le maillon suivant non. La lecture par la MMU (108e339) ne marche que si le vCPU
  choisi est dans le contexte du noyau. Pendant la panique #160, 28 kexts ont été lus et la pile
  est symbolisée ; pendant les « gels », un seul kext.
- Le banc a vu la panique #160 grâce au PC dans `panic()` : `panicstr` valait déjà 0. Le banc
  du M4 l'aurait comptée comme un gel, sans texte.

## 8. PC Linux, 07/10/2026 : console série et cause des « pas de ssh »

### 8.1 Le second canal : un UART 16550 sur PCI, un getty dans une base dédiée

- **Le port série de mac99 ne sert pas.** L'ESCC de macio est bien dans l'arbre de l'invité
  (`escc@13000/ch-a`), mais Tiger 10.4.11 n'y attache aucun nœud `/dev/tty.*`, et le noyau
  n'écrit rien sur `-serial` avec `serial=3` en boot-args (seul OpenBIOS y parle). Au passage :
  `EXTRA_ARGS` est découpé aux espaces, un `-prom-env 'boot-args=-v serial=3'` n'y passe pas.
- **Ce qui marche : `-device pci-serial`** (UART 16550 de QEMU, classe PCI 0x0700).
  `Apple16X50Serial.kext`, livré avec Tiger, l'apparie sur sa classe (`IOPCIClassMatch
  0x07000000&0xFFFF0000`) et publie `/dev/tty.pci-serialNN`, NN = numéro de fente.
  **Fente imposée `addr=0x12`** (IRQ 29, seule) : à la fente libre par défaut (0x11), l'UART
  partageait l'**IRQ 28 de l'OHCI** et ne recevait rien (l'émission marchait, la réception non).
  Côté hôte : `-chardev socket,id=serie0,path=…/serie.sock,server=on,wait=off`.
- **Base `disks/tiger-endurance-serie.qcow2`** (07/10 08:39, lecture seule) : copie de
  `tiger-endurance.qcow2` (`cp --sparse=always`, `qemu-img check` sans erreur), un démarrage
  avec l'UART, **une seule modification** : une ligne ajoutée à `/etc/ttys` (copie d'origine en
  `/etc/ttys.avant-serie`) —

      tty.pci-serial18 "/usr/libexec/getty std.115200" vt100 on secure

  puis `kill -HUP 1` (launchd relit `/etc/ttys` et lance le getty), arrêt propre par la console
  (`shutdown -h now`), `chmod a-w`. Même noyau (10.4.11), mêmes symboles
  (`bench/endurance/invite-10.4.11/`). L'ancienne base et la VM quotidienne n'ont pas bougé.
- **`tools/endurance/serie.py`** : ouvre une session sur le socket (`tiger` / `tiger974`),
  pose une invite neutre (`PS1='@> '`, `stty -echo`) et délimite chaque sortie par une marque
  de fin. Piège : `login` coupe l'écho par un `tcsetattr(TCSAFLUSH)` **après** avoir écrit
  « Password: » ; un mot de passe envoyé aussitôt est jeté (« Login incorrect ») — on attend
  1,5 s. Usage manuel :

      python3 tools/endurance/serie.py .run/endurance/<inst>/serie.sock 'netstat -an' 'ifconfig -a'
      python3 tools/endurance/serie.py .run/endurance/<inst>/serie.sock --releve DOSSIER

- **Dans le banc : `--serie`** (démarrages et jeux) ajoute l'UART à chaque instance
  (`.run/endurance/<campagne>-<i>/serie.sock`) et prend la base à getty (sauf
  `ENDURANCE_BASE`) ; c'est un autre bras (`campagne.json`). À chaque gel, **VM en marche,
  avant la collecte et tout reset** : relevé par la console dans `serie.txt` (`date`,
  `ifconfig -a`, `netstat -an`, `netstat -rn`, `arp -an`, `ipconfig getpacket en0`, le port 22
  essayé de l'intérieur par `nc`, `ps`, `launchctl list`, `lsof -i`, `dmesg`, `system.log`,
  `scutil`), trace brute dans `serie-brut.txt`, résumé dans `incident.md` (port 22 à
  l'écoute ?, sshd dans launchd ?, IPv4/IPv6 de lo0 et en0, erreurs `in6_` du msgbuf). Si la
  sonde n'a pas eu de bannière : `launchctl unload/load` de `ssh.plist` par la console, puis
  nouvelle sonde (`ssh_apres_rechargement`). Après le `system_reset`, le même relevé par ssh
  dans `apres-reset.txt`. `--releve-sain N` fait le relevé sur un démarrage sain sur N
  (référence, rangée dans `cycles/NNNN/`). Chaque démarrage sain note aussi l'échec éventuel
  d'`in6_ifattach` (`in6_lo0_errno` dans `cycles.jsonl`). `collecte --serie SOCKET` pour une
  collecte à la main.

### 8.2 Campagne `pc-reboot-serie` (07/10, 08:52-10:20)

Bras défaut **actuel** : binaire rapide `build-fast/` reconstruit le 07/10 à 07:03 (sha256
0bc504fa…, choisi par le lanceur lui-même, sans `--qemu`), leviers du lanceur, `--serie`, 3
instances, `--mode reboot`, `--releve-sain 20`. Charge 1 min de l'hôte : 3 à 10 (VM quotidienne
de l'utilisateur et un QEMU d'un autre agent en même temps). Arrêtée par `ARRET` au départ de
l'utilisateur, avant la borne (~3 h / 400) : **168 démarrages** (161 redémarrages, 7 à froid ; rapport :
`bench/endurance/pc-reboot-serie/rapport.md`).

- **1 « pas de ssh »** (#7, instance 0, 7ᵉ démarrage), même signature que les cinq de la nuit
  (bureau, vCPU au repos, sonde « connecté, fermé sans bannière »).
- **1 panique** (#76), d'une autre nature, §8.4.
- Démarrages sains : 0 échec d'`in6_ifattach` relevé (`in6_lo0_errno`) ; relevés par la
  console tous les 20 cycles : port 22 à l'écoute en IPv4 et IPv6 (tenu par `launchd`, `lsof`),
  `::1` sur lo0, aucun message `in6_`.

### 8.3 Ce que l'invité montrait au moment du cas (#7, par la console, avant tout reset)

`incidents/0007-gel/serie.txt` :

- **Rien n'écoute sur le port 22**, ni en IPv4 ni en IPv6 (`netstat -an`, `lsof -i`) ; `nc` vers
  `127.0.0.1:22` et `10.0.2.15:22` **depuis l'invité** : refusé. Le défaut n'est donc ni dans le
  NAT de QEMU ni dans le chemin réseau : il est dans l'invité, au-dessus de la pile IP.
- `launchctl list` contient bien **`com.openssh.sshd`**, mais `launchd` ne tient **aucun**
  socket pour lui (sur un démarrage sain : `launchd … TCP *:22 (LISTEN)` en IPv6 puis IPv4).
- **lo0 sans IPv6** : `inet 127.0.0.1` seulement, pas de `::1` ni de `fe80::1%lo0`. Dans le
  msgbuf : `in6_ifattach_loopback: failed to configure the loopback address on lo0 (errno=17)`
  (EEXIST ; les cinq cas de la nuit avaient le même message avec errno=55, ENOBUFS — relu dans
  leurs `kpanic.txt`). `mDNSResponder` n'a que ses sockets IPv4 (`bind error … errno 49`,
  `getsockname v6 error 9` dans `system.log`, comme dans les cas de la nuit).
- en0 normal : 10.0.2.15 par DHCP, passerelle 10.0.2.2 dans la table ARP, IPv6 de lien et
  `fec0::` présents (attachés plus tard), horloge juste.

**Mécanisme (lu dans les sources d'Apple, xnu-792.24.17 et launchd-106).**

1. Au démarrage, l'attachement IPv6 de lo0 (`in6_ifattach` → `in6_ifattach_loopback` →
   `in6_update_ifa`) échoue ; `in6_ifattach` rend alors la main sans rien poser sur lo0.
2. `/etc/rc` lance `launchctl load /Library/LaunchDaemons /System/Library/LaunchDaemons
   /etc/mach_init.d` tôt, avant la configuration d'en0 : à cet instant **aucune adresse IPv6
   n'existe** dans le système.
3. `launchctl` crée les sockets de `ssh.plist` (`SockServiceName ssh`, sans famille) par
   `getaddrinfo` : `::` d'abord, puis `0.0.0.0`. `in6_pcbbind()` commence par
   `if (!in6_ifaddrs) return (EADDRNOTAVAIL);` — le `bind()` IPv6 échoue, et
   `sock_dict_edit_entry()` fait **`return` au premier échec** : le socket IPv4 n'est jamais
   créé. Le job est chargé sans socket : sshd ne sera jamais lancé.
4. `system_reset` (ou un autre redémarrage) refait un démarrage où lo0 s'attache : ssh revient.

Pourquoi lo0 échoue n'est pas établi : deux acteurs peuvent l'attacher (le `timeout` noyau
`ip6_init2`, et `in6_if_up` sur ioctl `SIOCLL_START` depuis l'espace utilisateur), avec un
test « `::1` existe-t-il ? » puis un ajout non atomiques ; EEXIST ou ENOBUFS selon le perdant.
C'est une **course dans l'invité**, rendue visible par deux vCPU réellement parallèles (MTTCG)
et par le minutage du binaire. **Aucune cause QEMU** : ni slirp (le NAT relaie, l'invité
refuse lui-même), ni la carte réseau (en0 vivante, DHCP fait), ni l'horloge. Une nuance : un
démarrage de la nuit (instance 0, 06:18:54) avait le même message `in6_` et un ssh normal ; le
message n'est donc pas suffisant, ce qui va avec une course dont l'issue dépend du perdant.

**Non vérifié :** le rechargement de `ssh.plist` par la console (`launchctl unload/load`)
n'a pas tourné sur #7 (condition fautive, corrigée après coup) ; on s'attend à ce qu'il échoue
tant que lo0 n'a pas d'IPv6, et réussisse une fois en0 en IPv6 (`in6_ifaddrs` non vide).

**Corrections possibles (non faites).** Côté invité, pour le banc et pour la VM de
l'utilisateur : (a) `SockFamily IPv4` dans `ssh.plist` (launchctl ne fait plus de `bind` IPv6) ;
(b) un LaunchDaemon du paquet invité qui, une fois en0 configurée, recharge `ssh.plist` si rien
n'écoute sur 22. Côté QEMU : rien à corriger.

### 8.4 Surprise : une panique au démarrage sur le binaire du 07/10

`pc-reboot-serie` #76 (instance 2), juste après le `shutdown -r` (msgbuf arrêté à « Waiting on
… boot-uuid-media ») : **les deux vCPU dans `panic()`**, « 0x300 - Data access ». CPU 0 :
`_kalloc_canblock+0xd4` ← `OSCollectionIterator::isValid` ← `IORegistryIterator::getNextObjectFlat`
← `IOService::getMatchingServices` ← `is_io_service_get_matching_services` (appel MIG depuis
l'espace utilisateur) ; CPU 1 : `IOWorkLoop::threadMain+0x68`. Une occurrence, non analysée ici :
`incidents/0076-panique/` (`kpanic.txt`, `ecran.png`).
