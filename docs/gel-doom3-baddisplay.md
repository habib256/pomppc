# Gel au chargement de DOOM 3, `kCGLBadDisplay` après un kill, reset bloqué

29/09/2026, 3ᵉ chantier de « Maintenant ». VM quotidienne (`tiger.qcow2`, SMP=2),
noyau de l'invité Darwin 8.6.0 (`xnu-792.6.70`, Tiger 10.4.6), QEMU de référence.

Trois défauts, traités dans cet ordre :

| Défaut | Reproduit | Cause | État |
|---|---|---|---|
| `kCGLBadDisplay` après un `killall` de DOOM 3 | **non**, 0 sur 25 | symptôme reproduit en occupant les 4 tranches du kext ; tranches perdues par le kext du 24/09 (hypothèse) | la matrice ne redémarre plus l'invité après DOOM 3 ; elle vérifie à la place que le kext a rendu ses tranches |
| « gel » au chargement (vCPU 0 en `0x268b4`) | signature reproduite, déclencheur non reproduit | c'est une **panique** noyau : `_panic+0x254`, la boucle `b .` finale ; texte perdu | outil `tools/re/kpanic.py` pour la lire au prochain cas ; mécanisme probable ci-dessous |
| `system_reset` bloqué après ce gel (« using 1966 buffer headers ») | **oui**, 2 sur 2 | l'OpenPIC de QEMU garde au reset une source de niveau « en attente » | **corrigé** (`patches/openpic/0001`) : 8 sur 8 après correctif |

## 1. `kCGLBadDisplay` après un kill : ne se reproduit plus

Le 24/09, après un `killall` de DOOM 3, tout lancement suivant du jeu
échouait (`CGLQueryRendererInfo -> 10006`, « no OpenGL-supported video card »)
jusqu'au redémarrage de l'invité ; Prey et `gltest` démarraient. La matrice
redémarrait donc l'invité après chaque cellule DOOM 3.

**Reproduction tentée** (lanceur `~/d3k/lance.command`, boucle depuis l'hôte) :
25 parties de suite sans redémarrer l'invité. Chacune a été tuée, puis la
suivante lancée : 6 à la main, puis deux boucles de 11 et 8. Les kills
couvraient TERM et KILL, fenêtre (`r_mode 3`) et plein écran (`r_mode 5`),
au chargement (~300 échanges) et en jeu (~2200 échanges, après la
cinématique). S'y ajoutent un lancement depuis ssh (hors `open`) et une partie
morte d'elle-même (SIGBUS avec `libgltrap`, voir §4). **Les 25 lancements qui
suivaient un kill ont tous démarré** : « 16 MB Video Memory » affiché, images
produites. Après ces kills, `ioreg` compte 0 `POMPPCGPUUserClient` : le kext a
rendu toutes les tranches.

**Le symptôme se reproduit à coup sûr en occupant les tranches.** On garde les
4 clients du kext ouverts (`tools/guest/jobs/stallmeter/qhold.c`, `qhold 4 400`) :

- DOOM 3 sort aussitôt avec « Could not initialize OpenGL. There does not
  appear to be an OpenGL-supported video card in your system. » ;
- `gltest` démarre sur le rendu **logiciel** d'Apple (`accel=0`,
  `GL_RENDERER = Generic`) : c'est le « gltest démarre » du 24/09 ;
- le noyau note `POMPPCGPU: already 4 clients: open refused`.

DOOM 3 exige un rendu accéléré, alors que Prey et `gltest` se contentent du
logiciel. Des tranches perdues expliquent donc le tableau du 24/09. La ligne
`CGLQueryRendererInfo -> 10006` n'apparaît pas dans cette reproduction : elle
vient de `Sys_QueryVideoMemory` (masque d'écran de
`CGDisplayIDToOpenGLDisplayMask`), que le jeu n'atteint pas ici.

**Hypothèses classées :**

1. *(retenue)* **Des tranches perdues par le kext du 24/09.** La course
   `clientClose` ⊥ `clientDied` (K3) et la tranche non rendue quand la gate
   refuse (K9) ont été corrigées par les bug hunts du 29/09. Elles perdaient
   une tranche « pour toujours », et un redémarrage les rendait toutes.
   Preuve : le symptôme par épuisement des tranches, et 0 client restant
   après 25 kills avec le kext actuel. Manque : une rejouée sur le kext du
   24/09, qu'il faudrait réinstaller.
2. *(écartée)* **Processus lancé hors de la session graphique** (10006 de CGL
   pour un processus root ou sans WindowServer) : DOOM 3 lancé directement
   par ssh démarre et dessine.
3. *(écartée)* **Capture d'écran ou changement de mode laissés par le processus
   tué** : les kills en plein écran (capture de l'écran) sont suivis de
   lancements normaux.

**Correctif.** `tools/matrice/jeux/d3.py` passe `redemarrer_apres = False`.
Après chaque cellule, `matrice.py` compte les clients du kext encore ouverts
(`Hote.clients_kext`, `ioreg -c POMPPCGPUUserClient`). S'il en reste, la
cellule le note (« N client(s) du kext restant(s)… tranches perdues ») et
l'invité est redémarré. Même règle au début d'un tour pour un jeu resté en
marche. On supprime ainsi un redémarrage par cellule DOOM 3, et avec lui les
démarrages bloqués qui suivaient (§3). Colin McRae garde son redémarrage : il
n'a pas été éprouvé ici.

## 2. Le « gel » au chargement est une panique

**Symbolisation** contre `/mach_kernel` de l'invité (copie par
`tools/guest/tssh.sh "cat /mach_kernel" > mach_kernel`, `nm -n` et capstone) :

- `0x268b4` = `_panic+0x254`. Après `Debugger()`, `panic()` imprime
  « panic: We are hanging here... » puis boucle sur `b .` (`0x268b4`),
  interruptions coupées. C'est l'état exact du vCPU 0 relevé le 26/09.
- `0xaf6b4` = `_machine_idle+0x194` : le vCPU 1 est simplement **au repos**.

Pourquoi ni écran de panique ni `panic.log` : `debug_buf` n'est alloué
(`debug_log_init`) que par `kdp_register_send_receive`, c'est-à-dire avec un
transport de débogage ; sinon `debug_buf_size` reste nul, `Debugger()` saute
`PESavePanicInfo` (rien en NVRAM, donc pas de `panic.log` au démarrage
suivant), et le texte part par `kdb_printf` vers la console seule, jamais dans
le msgbuf. Une panique par `panic()` laisse l'écran tel quel : bureau figé,
horloge arrêtée. Seuls les « System Failure » du gestionnaire d'exception
dessinent leur texte (panique UT2004 du 27/09, `bench/utweapon-priorites/panic.png`).

**Reproduction de la signature** : panique synthétique (le CPU 0 pris en mode
noyau par le stub GDB, `r3` = format, PC = `_panic`, `tools/re/epreuve_reset.py`) :
CPU 0 en `_panic+0x254` EE=0, CPU 1 en `_machine_idle+0x194`, bureau figé,
pas de ssh — **identique au 26/09**.

**Lire la prochaine** : `python3 tools/re/kpanic.py mach_kernel`, invité gelé, au
moniteur de la VM quotidienne. La matrice le fait seule désormais : au début
du tour, elle copie `/mach_kernel` dans `.run/mach_kernel` ; sur un invité
gelé, AVANT le reset, elle range `gel/gel.ppm` et `gel/kpanic.txt` dans le
dossier de la cellule (`Hote.autopsie`). L'outil donne les PC et LR de chaque CPU,
symbolisés dans le noyau ou dans les kexts (chaîne `kmod`). Il reconstitue le
format (`panicstr`, sinon `r24` : `panic()` remet `panicstr` à 0 avant sa
boucle finale), l'appelant (`panic_caller`), le texte avec ses arguments lus
sur la pile du CPU arrêté, la chaîne d'appels, et la fin du msgbuf. Épreuve
sur la panique synthétique :
`System Failure: cpu=0; code=00000099 (panic)`, `panic_caller = _mapSetUp+0x34`.

**Mécanisme probable (non prouvé, texte de la panique du 26/09 perdu).** Les
verrous tournants de Tiger paniquent au bout de `LockTimeOut` =
250 ms (`ml_init_lock_timeout` : `0xee6b280` ns, soit `0x5f49c1` tops à
24 979 204 Hz ; aucun boot-arg ne le change en 10.4.6). Ce délai se mesure
avec `mftb`, et la base de temps de QEMU suit l'horloge de l'hôte. **Tout
arrêt d'un vCPU pendant qu'il tient ou attend un verrou** compte donc dans le
délai : hôte chargé, attente du BQL, section exclusive de TCG, doorbell
synchrone du qgpu qui garde le BQL jusqu'à `QGPU_SYNC_WAIT_MS` = 2 s. Or ces
arrêts n'existent pas sur un vrai G4. Deux incidents connus relèvent déjà de
cette famille :

- UT2004 du 27/09 : `System Failure: cpu=1; code=0000000A (Lock timeout)`, PC
  `0xAA010` = `_fpu_switch+0x410`, le « choke » qui suit le délai d'attente du
  verrou de contexte flottant (`lwz 0xac0` = `LockTimeOut`, boucles `mftb`) ;
- AppleUSBOHCI au démarrage : « simple lock deadlock detection » = délai de
  `usimple_lock` (même `LockTimeOut`).

**Mesure** (`tools/guest/jobs/stallmeter`, deux fils endormis 1 ms en
`nice -20`, sauts de `mftb` ≥ 20 ms) pendant 8 chargements et 8 kills de
DOOM 3, sur 15 min : 63 sauts, **max 95 ms**, aucun ≥ 100 ms. Les sauts touchent
les deux fils à la fois : arrêt de toute la VM. Les plus longs, une rafale de
30 en 2 s, tombent au kill de DOOM 3 (destruction des objets hôte du client).
La marge restante est donc de ×2,6 hôte peu chargé (charge 2 à 5). Un profil
`sample` de QEMU pendant un chargement ne montre aucun vCPU en attente dans
le doorbell. Le déclencheur du 26/09 (1 lancement sur ~12, juste après un
redémarrage de l'invité) n'a pas été reproduit ici.

**Pistes de correctif** (non faites, à trancher) : relever `LockTimeOut` dans
l'invité par le kext au chargement. C'est une variable du noyau (`0x360ac0`),
relue à chaque attente, et la valeur 250 ms suppose un matériau qui ne s'arrête
jamais. Ou borner les arrêts de vCPU côté QEMU : doorbell synchrone sans BQL,
ou destruction des objets d'un client mort hors du fil principal. Le prochain
gel lu par `kpanic.py` dira si c'est bien un « Lock timeout ».

## 3. `system_reset` bloqué après la panique : l'OpenPIC

**Reproduction** : panique synthétique (§2), 10 s, `system_reset`. Le
démarrage se bloque après « using 1966 buffer headers » **2 fois sur 2**
(17:14 et 17:30), comme le 26/09 et le 27/09 (UT2004). État relevé :

- CPU 0 : PC alternant `com.apple.driver.AppleMPIC+0x1c40..0x1dac` et
  `com.apple.driver.AppleUSBOHCI+0x4d60..0x4d78`, `r1 = 0xbe80` (pile
  d'interruption), EE=0 ; CPU 1 au repos ;
- OpenPIC (`0x80040000`), source 28 (OHCI de KeyLargo, IRQ 28) :
  IVPR `0x4048001c` (niveau, **active**) ;
- OHCI (`0x80080000`) : `HcInterruptEnable` = MIE seul, rien à signaler.

Le démarrage est englouti par une interruption perpétuelle : AppleMPIC la
présente, le filtre d'AppleUSBOHCI la refuse (le contrôleur n'a rien à
signaler), puis AppleMPIC la représente.

**Cause** (`hw/intc/openpic.c` de QEMU 9.2) : `openpic_reset` remet chaque
source à `ivpr_reset` (sensible au **front**) **sans** remettre `pending` à 0.
Pendant la panique, les interruptions sont coupées et l'OHCI tient sa ligne
haute (SOF, WDH), `pending` = 1. Au reset, l'OpenPIC repasse la source en
front ; la retombée de ligne qu'envoie ensuite le reset PCI de l'OHCI
(`pci_device_deassert_intx`) est **ignorée en front** (`openpic_set_irq` ne
retient que les montées). Au démarrage, Tiger remet la source au niveau et
la démasque : `pending` vaut toujours 1, et rien ne peut plus le remettre à 0.
Deux `system_reset` de suite ne changent rien ; relancer QEMU répare, car
tout l'état est neuf.

**Correctif** (`patches/openpic/0001-openpic-reset-niveau.patch`, étape 1 bis de
`scripts/build_qemu_qfb.sh`, après la série SMP qui touche le même fichier) :
chaque source retient le niveau brut de sa ligne (`input`, posé à chaque
`openpic_set_irq`). Le reset remet `pending` à 0. Quand l'invité (ré)écrit
l'IVPR d'une source de niveau, `pending` reprend l'état réel de la ligne.
`post_load` reconstitue `input` depuis `pending` (champ non migré).

**Preuve après** (binaire de référence reconstruit le 29/09 à 17:35,
précédent en `~/src/qemu/build/*.avant-openpic`) : panique synthétique puis
`system_reset`, la source 28 étant **active** au moment du reset :
ssh revenu en 88, 27 et 51 s (3/3 à la main), puis `tools/re/epreuve_reset.py` :
1/1 (l'outil s'est ensuite arrêté : moniteur laissé ouvert, corrigé) et 4/4
(27, 51, 26 et 89 s), soit **8/8**. Témoin sans panique : ssh en 26 s.

**Lien probable, non prouvé** : la panique AppleUSBOHCI « simple lock deadlock
detection » au démarrage (~1 sur 10). Un `shutdown -r` remet la machine à zéro
par le même chemin : si l'OHCI tenait sa ligne à ce moment, le démarrage
suivant part avec une interruption perpétuelle sur un CPU. Pendant ce temps,
l'autre CPU attend un verrou tenu par le premier, puis panique au bout de
250 ms. Le banc d'endurance départagera (taux avant et après 17:35:50).

## 4. En passant

- `~/doom3.command` (ancien lanceur, avec `DYLD_INSERT_LIBRARIES=libgltrap`)
  : DOOM 3 meurt en SIGBUS au premier rendu de la carte (`CRASH pc 025e7094
  dar 00000470`, hors du plugin). Les lanceurs sans `gltrap` et la matrice
  ne le font pas. Non creusé.
- Un QEMU quotidien a disparu trois fois sans message entre 16:02 et 16:06
  (coordinateur prévenu ; surveillance dans `.run/qemuwatch.log`).
- Écrire le PC d'un CPU **au repos** (`MSR[POW]`) par le stub GDB ne le fait
  pas repartir et abîme le fil de repos (invité muet) : `epreuve_reset.py`
  attend un CPU 0 en mode noyau hors repos.

## 5. Outils

| Outil | Rôle |
|---|---|
| `tools/re/kpanic.py MACH_KERNEL` | autopsie d'un invité gelé par le moniteur : PC/LR symbolisés (noyau et kexts), texte de panique, appelant, pile, msgbuf |
| `tools/re/epreuve_reset.py MACH_KERNEL [N] [--normal]` | épreuve panique synthétique → `system_reset` ×N (détruit l'état de l'invité) |
| `tools/guest/jobs/stallmeter/stallmeter.c` | arrêts de vCPU vus par la base de temps, dans l'invité |
| `tools/guest/jobs/stallmeter/qhold.c` | tient N clients du kext ouverts (tranches perdues simulées) |
