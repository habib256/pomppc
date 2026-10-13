# Épreuve « GPU hôte bloqué » (GL3, KT4, K6, KT5) et A6 (L2) — 14/10/2026

Fermeture de deux entrées de [TODO.md](../TODO.md) : le point (2) de « Restes des épreuves
en VM des bug hunts » et « A6, reste L2 — créneau rendu malgré un nettoyage échoué »
([architecture.md](architecture.md) §5 et §6). Toutes deux demandaient de quoi **bloquer le
GPU de l'hôte à la demande** ; le device n'avait aucune propriété de test, et ni l'échéance
de 5 s de `qgpu_soft_reset`, ni l'état « cassé », ni `scanout_rebind`, ni un nettoyage de
tranche refusé ne s'atteignaient en VM.

Hôte : PC Linux x86-64 (Ubuntu 26.04), QEMU 11.1.2 reconstruit **hors** de `~/src/qemu`
(voir §5), backend `gl` (RTX 4060 Ti, EGL). Invité : copie de la VM de dev
`disks/tiger-dev-stall.raw` (10.4.11), single-user, `tools/guest/devloop.py`.

## 1. Les propriétés de test du device

`patches/qgpu/qgpu-pci.c`, propriétés **d'instance** (pas `DEFINE_PROP_*` : une propriété
statique de qdev refuse d'être écrite après `realize`) ; ligne de commande, `-global` et
`qom-set` à chaud :

| Propriété | Effet | Coût éteinte |
|---|---|---|
| `x-test-stall-ms` (uint32, 0) | le thread de rendu dort N ms **avant chaque job**, job marqué en cours (`q_running`) : exactement ce que voit le device quand le pilote GL hôte ne rend plus la main. Relu toutes les 10 ms : `qom-set … 0` débloque le job en cours, une valeur plus courte raccourcit la sieste ; la demande d'arrêt du thread l'interrompt. Une ligne `x-test-stall-ms : job … retenu N ms` par job retenu ≥ 1 s. | une lecture relâchée par job |
| `x-test-refuse-reset` (bool, off) | toute écriture de `QGPU_REG_CLIENT_RESET` est refusée par `QGPU_ST_QUEUE_FULL` (le refus réel d'une file pleine, après lequel le kext réessaie `DESTROY_RETRIES` fois) ; rien en file, aucun compteur ne bouge ; une ligne au journal par refus. | une lecture par CLIENT_RESET |

Chaque changement sur un device vivant est journalisé (`qgpu-pci: x-test-stall-ms = …`).
Code générique (g_usleep, horloge de QEMU, qatomic) : rien de propre à un hôte, même niveau
de preuve sur le M4 dès qu'il sera reconstruit (règle de parité).

```
qom-set /machine/peripheral/gpu0 x-test-stall-ms 600000     # bloquer
qom-set /machine/peripheral/gpu0 x-test-stall-ms 0          # débloquer
qom-set /machine/peripheral/gpu0 x-test-refuse-reset true
```

## 2. A6 (L2) : le kext ne rend plus un créneau sale

`kext/POMPPCGPU/POMPPCGPU.cpp`, ABI inchangée (`qgpu_abi.h` identique, plugin inchangé) :

- `destroyClientObjects(slot, &fence)` rend un `IOReturn` : succès = CLIENT_RESET accepté
  **et** barrière atteinte ; sinon `fence` = la barrière du CLIENT_RESET accepté mais pas
  atteinte, ou 0 s'il n'est jamais parti (refusé, file non drainée) ;
- `slotGated` marque alors la tranche sale (`fSlotDirty` : `SLOT_DIRTY_UNSENT` ou
  `SLOT_DIRTY_FENCE` + `fSlotFence`), la libère quand même de son propriétaire, et rend
  l'échec (`resetSlot` le rend au client ; le plugin n'appelle pas `QGPU_UC_RESET`) ;
- `allocSlot`, en trois passes sous la gate : (1) relance le CLIENT_RESET de toute tranche
  sale jamais partie, **avant** de redonner quoi que ce soit (sinon, mis en file après la
  réattribution, il détruirait les objets du nouvel occupant : `run_a6`, cas (c)) ; (2) prend
  une tranche propre, ou sale dont la barrière est atteinte (FIFO : objets détruits ET tranche
  plus lue) ; (3) à défaut, attend (borné par `DESTROY_WAIT_MS`, gate relâchée, tranche tenue
  par `fSlotBusy`) la barrière d'une tranche sale ; sinon refus, comme une table pleine.
  Jamais une tranche aux objets vivants.
- Journal : `slot N: dirty, CLIENT_RESET never queued…`, `…queued but fence F not reached…`,
  `…CLIENT_RESET resent (fence F)`, `…refused again…`, `slot N: clean again (fence F reached)`.

## 3. Correctif trouvé par l'épreuve : lien d'écran abandonné faute de drainage

`system_reset` avec un job bloqué : le reset de `qfb-pci` passe **avant** celui de qgpu
(device pas encore cassé), `qgpu_bind_scanout` draine 2 s, échoue (« file encore pleine »)
et abandonne la nouvelle géométrie. Avec qfb rien ne la redemandait : la notification est
consommée, `qgpu_scanout_recheck` ne suit que VGA, et `scanout_rebind` n'était posé que sur un
device cassé. Vu le 14/10 : écran revenu à 1024x768 au pas 128, lien resté à 640x480 au pas
1280 après la réparation (`bench/stall-20261014/qemu-gl3-avant-rebind.log`, aucune ligne de
reliaison après le second `system_reset`). Désormais l'échec de drainage pose
`scanout_rebind`, retenté au prochain doorbell d'un device sain (`qgpu_scanout_recheck`) et par
le reset qui aboutit ; preuve au §4 (ligne K6 bis). Message du reset : « reset abouti, écran
relié de nouveau (…) ». Prix assumé, celui que GL8 a déjà choisi pour VGA : tant que le GPU
reste bloqué sans que le device soit cassé, chaque doorbell retente le lien et peut tenir le
BQL `QGPU_SYNC_WAIT_MS` (2 s) ; un lien juste vaut mieux qu'une image écrite au mauvais pas.

## 4. Résultats (14/10/2026, PC)

Preuves dans `bench/stall-20261014/` (hors dépôt) : `epreuve-*.log` (journal horodaté de
l'hôte, sorties des jobs incluses), `qemu-*.log` (lignes `qgpu-pci:`), `jobs/*/out`.

| Point | Verdict | Preuve |
|---|---|---|
| **GL3** reset borné, device « cassé » puis réparé | **tenu** | `epreuve-gl3.log` : GPU bloqué (`x-test-stall-ms` 600000), un job en vol, `kextunload` + `kextload` : « reset non terminé après 5000 ms », le kext lit CAPS = 0 et refuse de démarrer (« without QGPU_CAP_CLIENTS … refusing ») ; le moniteur reste servi (pire réponse 5,01 s, le seul drainage du reset). Débloqué (« job retenu 47448 ms »), `kextload` répare : `QGPUClients = 4`, `qgpu_test` vert, première création OK. Même chose par `system_reset` sous blocage : commande rendue en 7,00 s (2 s de drainage du lien qfb + 5 s du reset), après redémarrage CAPS = CLIENTS = 0 et le job toujours en vol (`EN_FILE=1`), débloqué FENCE avance mais CAPS reste 0 jusqu'au reset suivant, `kextload` répare (CAPS 0xafffe, `qgpu_test` vert). |
| **KT4** device cassé : CAPS non réécrit, pas de BQL 2 s par changement de géométrie | **tenu** | même journal : l'écran POMPPCQFB est ouvert (mode par défaut, pas 4096) puis passé en 640x480x16 **pendant la panne** (`qfbmode`) : ouverture 0,014 s, changement de mode 0,001 s, aucune réponse du moniteur au-delà de 0,25 s pendant le job `geom` (755 réponses, pire 0,00 s) ; registres relus juste après : CAPS = 0x0, CLIENTS = 0x0. |
| **K6** fenêtre qfb d'avant la panne remplacée à la réparation | **tenu** | avant la panne le lien est 1024x768 au pas 128 (reset de qfb) ; pendant la panne aucune ligne `scanout sur` (lien refusé) ; à la réparation par `kextload` : `scanout sur qfb-pci, fenêtre 640x480, pas 1280` puis « écran relié de nouveau (qfb-pci, 640x480, pas 1280) » — le mode posé pendant la panne, pas celui d'avant. **K6 bis** (§3) : `epreuve-gl3-sansgeom.log`, `system_reset` sous blocage sans autre changement de mode : réparation → « reset abouti, écran relié de nouveau (qfb-pci, 1024x768, pas 128) », que le binaire d'avant le correctif ne relie jamais. |
| **KT5** sortie de QEMU bornée | **tenu** | `epreuve-kt5.log` : `quit` avec un job bloqué, processus terminé en 5,06 s (« thread de rendu encore occupé après 5000 ms à la sortie … QEMU sort sans l'attendre »). |
| **L2 / A6** créneau sale jamais redonné | **tenu** (kext du dépôt) ; défaut **reproduit** avec le kext d'avant (`2e98d77`) | `epreuve-a6-old.log` puis `epreuve-a6-new.log`, cinq scénarios, client A tué par `kill -9` avec ses soumissions en file, puis premier CTX_CREATE d'un client B. **full** (stall 1,5 s, file tenue pleine) : avant, « host did not drain … before CLIENT_RESET » puis B en tranche 0 → `QGPU_ST_LIMIT` ; après, « dirty, CLIENT_RESET never queued » → « resent (fence 19) », B reçoit la **tranche 1**, création OK. **wait** (mêmes, tranches 1-3 tenues) : avant LIMIT ; après, B attend 1,52 s à l'ouverture, « clean again (fence 22 reached) », tranche 0, création OK. **late** (stall 0,5 s, CLIENT_RESET accepté mais fini après l'échéance) : avant, B en tranche 0 sans LIMIT (FIFO, cas (b) de `run_a6`) ; après, « queued but fence 18 not reached », B en tranche 1. **refuse** (`x-test-refuse-reset`) : avant LIMIT ; après « refused again … slot not reused », B en tranche 1. **again** (refus levé) : avant LIMIT ; après « resent (fence 4100) » puis « clean again », tranche 0, création OK. Bilan : 4 LIMIT sur 5 avant, 0 sur 5 après. |

Non-régression : `tests/qgpu_smoke.py` sur le nouveau binaire (61 OK, code 0), `qgpu_test` vert
à chaque réparation, contrôles du kext de `run-all.sh` (ABI identique, aucun symbole de
`qgpu_proto.h`).

## 5. Rejouer

```
# QEMU dans un arbre à soi (jamais ~/src/qemu : d'autres bancs en dépendent)
rsync -a --exclude=/build --exclude=/build-fast --exclude=/pgo-fast ~/src/qemu/ ~/src/qemu-stall/
QEMU_SRC=~/src/qemu-stall ./scripts/build_qemu_qfb.sh        # pose la série, configure, ninja

export DEVDISK=disks/tiger-dev-stall.raw QEMU_BIN=~/src/qemu-stall/build/qemu-system-ppc
export SND=none GPU_TRACE=1 QEMU_EXTRA="-device qfb-pci -global qgpu-pci.scanout=qfb"
python3 tools/guest/devloop.py start
tools/guest/epreuve_gpu_bloque.py prep --old-rev 2e98d77   # kexts (dépôt + d'avant), outils
tools/guest/epreuve_gpu_bloque.py gl3                      # GL3, KT4, K6 (≈ 6 min)
tools/guest/epreuve_gpu_bloque.py gl3 --part 2 --sans-geom # K6 bis
tools/guest/epreuve_gpu_bloque.py a6 --kext old            # défaut reproduit
tools/guest/epreuve_gpu_bloque.py a6 --kext new            # A6
```

Outils invités (compilés dans l'invité par le job `stall`, rangés dans `/pomppc/st/`) :
`guest/qgpu-test/slotpeer.c` (client minimal : `fill [keep]`, `create`, `hold`, `async1`,
`reset` ; créations en asynchrone + `WAIT_FENCE` + `PEEK`, pour ne pas buter sur l'abandon à
2 s du doorbell synchrone sous `x-test-stall-ms`) et `guest/qgpu-test/qfbmode.c` (ouvre
POMPPCQFB en connexion « serveur » et change son mode, sans WindowServer).

## 6. Pièges

- **Ubuntu 26.04 a retiré les en-têtes de développement** (glib, GTK, SDL, PulseAudio, libGL) :
  le `configure` de `build_qemu_qfb.sh` échoue (« Dependency glib-2.0 not found »). Sans sudo :
  `apt-get download` des paquets `-dev` (libglib2.0-dev, libgio-2.0-dev, libglib2.0-dev-bin,
  libpcre2-dev, libffi-dev, libmount-dev, libblkid-dev, libselinux-dev, libsepol-dev,
  zlib1g-dev, libsysprof-capture-4-dev, libgl-dev, libglx-dev, libopengl-dev, libglvnd-dev),
  `dpkg-deb -x` dans un préfixe, `.pc` réécrits, `PKG_CONFIG_PATH` (script
  `~/src/qemu-stall-deps/prep.sh`), puis `configure --disable-gtk --disable-sdl
  --audio-drv-list=alsa` : binaire de test sans affichage ni son (lancer avec `SND=none`).
  Le binaire de référence, lui, demandera un `sudo apt install` des `-dev` avant sa prochaine
  reconstruction. Le `configure` sous Python 3.14 (venv de meson) passe sans retouche.
- **`IOFramebufferOpen` d'IOGraphicsLib en single-user** programme le mode puis ne rend jamais
  la main ; le clavier de la console est perdu avec lui (VM coupée, `fsck` deux fois).
  `qfbmode` passe donc par `IOServiceOpen(…, kIOFBServerConnectType)` et le sélecteur 4.
- L'agent de la VM de dev est d'avant le code de sortie (« en-tête à 3 champs ») :
  `devloop.py run` rend toujours 0 ; le script lit la ligne `stall PHASE : N echec(s)`.
- `kextload` prend ~28 s dans cette VM (sans blocage) : la durée du rechargement sous blocage
  (32 s) ne mesure pas l'échéance ; c'est la latence du moniteur qui la date (5,0 s).
