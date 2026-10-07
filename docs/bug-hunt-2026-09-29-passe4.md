# Bug hunt, quatrième passe — 29/09/2026

Six relecteurs en lecture seule sur `4fafc58` (après la passe 3,
docs/bug-hunt-2026-09-29-passe3.md), consigne n° 1 : régressions des correctifs de la
passe 3. Quatre agents de correction en worktrees, fusionnés sans conflit (`e379e54`
à `5722adc`). Environ un tiers des findings sont des correctifs de la passe 3 faux ou
incomplets.

## Outils de la matrice et scripts

| # | Gravité | Quoi |
|---|---|---|
| O1 | **majeur** | `hote.py`, `ab-copie-gpu.sh`, `d3run.sh` supprimaient `.run/tiger.lock` quand leur `quit` échouait en silence, puis relançaient : deux QEMU sur `tiger.qcow2` (macOS : QEMU ne verrouille pas l'image, ce flock est le seul garde-fou). Plus aucun `rm` du verrou dans le dépôt : attente de sa libération (`host_wait_unlocked`, `verrou_tenu`), abandon avec le PID détenteur |
| O2 | moyen, **FS2 incomplet** | la dernière VM arrivée prenait `mon-PID.sock`, quotidienne ou non ; tous les outils codaient `mon.sock`/2222. La VM qui tient le verrou garde `mon.sock` ; `run_tiger.sh` publie `.run/tiger.mon` et `.run/tiger.sshport`, lus par les outils |
| O3 | moyen | `charge_hote` excluait toute VM Tiger (motif de chemin) ; n'exclut plus que le détenteur du verrou, et relève la charge après la fenêtre de mesure |
| O4 | moyen | une exception autre qu'`Echec` sautait le ménage et le redémarrage de l'invité ; `cont` réessayé, `VMEnPause` arrête le tour |
| O5–O7 | faible | réglages de Nexuiz perdus ; `--reprendre` vert avec la preuve du tour précédent ; `host_sock_alive` jugeait mort un moniteur occupé |

## Plugin

| # | Gravité | Quoi |
|---|---|---|
| P-I1 | majeur, **PC1** | la texture en cours de téléversement échappait à l'invalidation (`up_seq` posé après la boucle) |
| P-I2 | majeur, **PC2/T1** | fenêtre « seq − 2 » ratée quand des sondes s'intercalent → plancher `err_floor` |
| P-I3 | moyen, **PC5** | `SURF_TEX` refusé en `NO_MEM` : `surf_copied` restait à 1, reflet vide toute la course (trouvé par deux relecteurs) |
| P-I4 | moyen | `raw_sync` écrasait un `rd_all()` fait pendant son téléversement (`rd_gen`) |
| P-I5 | moyen, **T2** | relectures d'une moitié fautive recopiées comme bonnes quand l'erreur est vue au doorbell de l'autre moitié |
| P-I6 | faible | ligne de base `NOMEM` non relue après un refus de sonde |
| P-B2 | faible | bandes perdues puis contexte déclaré SYNCED (SYNCED posé avant les bandes) |
| P-B4 | faible | `posts_lost` basculait tous les contextes (ciblé par `Post.ctx`) |
| P-B5 | mineur | `POMPPC_GL_VRAM_MB=2048` → VRAM négative (borné à 2047) |

## Cœur, kext, device

| # | Gravité | Quoi |
|---|---|---|
| C9 | moyen, **C6**, reproduit | GLSL dont toutes les sources sont refusées : perdu pour de bon, sources empilées, faute fatale au nouvel essai |
| C10 | moyen, reproduit | case refusée liée puis recréée avec une autre cible : le VP exécutait un programme GLSL ou un FP (`prog_retarget`, cible vérifiée par `qgpu_prog_active`) |
| C11 | test | contrôle de `mem_total` avant/après chaque séquence (`run_bh4`) |
| K6 | mineur, **KT4** | fenêtre qfb d'avant la panne gardée après réparation (`scanout_rebind`) |
| K7 | mineur | `forgetSlot` pouvait libérer la tranche d'un autre client (`OSCompareAndSwap`) |

## Son et TCG

| # | Gravité | Quoi |
|---|---|---|
| Q7 | moyen, **Q3** | le témoin `0xFFFF` lisait le descripteur suivant (rechargé par `dbdma_end`) ; statut relu en mémoire à `cp`, stub du test fidèle |
| Q8 | mineur, **Q6** | split-wx : `x-jit-near` place désormais l'alias RX, pas la vue RW |

## Validation

Hôte : `tests/run-all.sh` 156 OK, 0 échec ; `qgpu_core_test` sous ASan/UBSan ;
`qgpu_contract.py --check` ; `run_bh4` rouge (6 échecs) sur `4fafc58`, vert après ;
tests screamer rouges sur `4fafc58`, verts après. VM : voir la validation du
29/09 dans `docs/matrice-jeux.md`.

Vitesse : le tour de matrice sur `4fafc58` (images toutes justes) est inexploitable
pour les ms/image — charge hôte 40 à 53 (autre projet et Spotlight en parallèle).
Un A/B DOOM 3 sur hôte au repos reste à faire contre `~/src/qemu/build/*.avant-bughunt`.

## Épreuves en VM des correctifs des trois bug hunts (07/10/2026, PC)

VM quotidienne du PC (`disks/tiger.qcow2`, 10.4.11, SMP=2, son, binaire de référence
`~/src/qemu/build`, kext v22 installé = `disks/prebuilt` du 04/10, MD5 `b5ef06b4…`), lancée
par `POMPPC_FRONTEND=native ./run_tiger.sh`. Outils compilés dans une copie de la VM de dev :
`guest/qgpu-test/unloadpeer.c` (nouveau : un dormeur en `WAIT_FENCE` sur une barrière
impossible et un soumetteur de `NOP` en boucle, tenus ouverts pendant le `kextunload`, puis
chaque sélecteur rejoué), `qgpu_test`, `POMPPCQFB.kext`. Relevés, captures, WAV et journaux
dans `bench/validation-20261007/` (hors dépôt). Une ligne par point :

| Point | Verdict | Preuve (07/10, PC) |
|---|---|---|
| `kextunload` avec un jeu ouvert (K4, KG4, KT1) | **tenu** | Marble Blast en jeu (tranche 0, asynchrone) + `unloadpeer` (tranche 1) ouverts : `kextunload -b net.pomppc.POMPPCGPU` réussit en 1,35 s, module déchargé (`kextstat` vide), aucune panique. Le dormeur sort 0,9 s après le début du déchargement avec `kIOReturnNotReady` (K4/KG4), le soumetteur en vol prend une erreur après 1 740 `SUBMIT` réussis (KT1). Marble Blast ne meurt pas : trois attentes refusées, « falling back to software », il continue en rendu d'Apple (chrono qui avance, `mb1-apres-unload.png`) puis quitte en code 0 |
| `SUBMIT` après déchargement → erreur propre | **tenu**, autre code que prévu | les six sélecteurs rendent `0xe00002c2` (`kIOReturnBadArgument`), pas `kIOReturnNotAttached` : sous 10.4.11 la terminaison détache le port de la connexion, l'appel n'atteint plus le kext et les sorties ne sont pas écrites. C'est ce qui permet au module de se décharger malgré deux clients ouverts (le `retain` de KT1 n'y fait pas obstacle). `unloadpeer` accepte désormais les deux refus (son premier passage comptait 8 « échecs » sur ce seul code) |
| Kext rechargé, jeu relancé (KT2) | **tenu** | `kextload` → « started: protocol v22 … 4 clients », `qgpu_test` vert sur la tranche 0 qu'occupait Marble Blast (aucun `QGPU_ST_LIMIT` : l'écriture de MAGIC a vidé les objets laissés), Marble Blast relancé : 762 soumissions asynchrones, 0 repli synchrone, 0 erreur |
| Créneaux perdus (KT2, KT3, K7) | **tenu** | après le déchargement avec deux clients : six tours de 4 `qgpu_test` en parallèle (chacun `SUBMIT`, `RESET`, fermeture) + un `unloadpeer` tué par `kill -9` en vol ; ensuite quatre clients tenus reçoivent les tranches 0, 1, 2, 3, le 5e est refusé (« already 4 clients »), et tout revient à 0 client. Remarque : un processus tué pendant un `WAIT_FENCE` garde sa tranche jusqu'à l'échéance de l'attente (`THREAD_UNINT`, 60 s au plus pour `unloadpeer`, 5 s pour le plugin), état `E` dans `ps` ; voulu (K10), pas une perte. Les `qgpu_test` parallèles rougissent sur `ERRORS`/`peek` globaux (L5) : ce binaire n'a pas encore `QGPU_CAP_CLIENT_ERRORS` |
| `QFB=1` avec Marble Blast (Q1, Q2) | **tenu** | `POMPPCQFB.kext` installé le temps de l'épreuve (bureau étendu au second écran 1280×800, `qfb-ecran-qfb.png`), QEMU : « SURF_PRESENT présentera sur VGA (scanout=auto) ». Plein écran : **614 présentations hôte** sur 615 images, chrono 17,56 → 24,61 s entre deux captures du VGA ; fenêtré : 481 relectures, image vivante. Kext QFB retiré ensuite |
| GPU hôte bloqué, puis rechargement et `system_reset` (GL3, KT4, K6) | **non éprouvable** | le device n'a aucune propriété de test (seulement `shmem_mb`, `backend`, `scanout`, `trace`) ; sans elle, ni l'échéance de 5 s de `qgpu_soft_reset` ni l'état « cassé » ni `scanout_rebind` ne s'atteignent. Fait sans blocage : rechargement du kext (ci-dessus) et `system_reset` (ligne suivante), kext redémarré et `qgpu_test` vert après. Propriété à ajouter : `x-test-stall-ms` (le thread de rendu dort N ms avant chaque job, réglable à chaud par `qom-set`) et `x-test-refuse-reset` (CLIENT_RESET refusé), qui servent aussi l'épreuve de L2 (`architecture.md` §5) |
| `system_reset` en SMP=2 (M1) | **tenu** | NIP des deux CPU toutes les 0,25 s (`m1-reset1.txt`) : CPU 1 parqué à `0xfff00100` pendant OpenBIOS et BootX ; CPU 0 dans le noyau à t = 17,6 s, CPU 1 ne bouge qu'à t = 19,1 s ; après le démarrage `hw.activecpu: 2`, kext v22 démarré, `qgpu_test` vert |
| Son Tiger lecture/pause/arrêt/relance (S1–S7, Q3–Q7) | **tenu** | QuickTime Player piloté par AppleScript, PCM du Screamer capturé par `wavcapture` : son pendant chaque lecture (−9 à −11 dBFS crête), **silence numérique exact** (−120 dBFS) pendant chaque pause et après chaque arrêt (pas de tampon rejoué, rien après l'arrêt du canal), reprise après pause, relance après fermeture puis par un autre client (`say`). Refait après le `system_reset` : même résultat (Q5) |
| `QGPU_MEM_MB=512` sous DOOM 3 (R5) | **tenu sans être éprouvé** | DOOM 3 joue (cinématique puis Mars City Hangar, 1 015 images, `d3-mem512-b.png`) mais `QGPU_REG_NOMEM` (lu par `xp` sur BAR1) reste à **0** : 384 Mio par tranche suffisent, le refus n'arrive pas |
| `QGPU_MEM_MB=128` sous DOOM 3 (R5, refus forcé) | **échoué** | 201 refus `NO_MEM` (`QGPU_REG_NOMEM` : 425 commandes refusées), aucune faute fatale côté plugin ni device, mais DOOM 3 meurt à l'image 290 : `exit()` de GLEngine dans `gleBuildInterpolateFunc` (`gleClipPoly` ← `gleVPRenderTriangles` ← `RB_ARB2_CreateDrawInteractions`, pendant un `CaptureRenderToFile`), puis double faute dans ses destructeurs (`d3-mem128-crash.log`). Cause : le repli « objet non résident → Apple » envoie des dessins d'interaction ARB au rendu logiciel de GLEngine, qui sort sur l'interpolateur de découpage trop long (`docs/re/glengine-exit-interpolateur.md`). Pas un petit correctif : entrée TODO |
| `run-all.sh` par les deux chemins | **tenu**, deux rouges hors épreuve | rapide : 237 OK, 1 échec ; `--slow` : 238 OK, 2 échecs. Les deux rouges : le binaire rapide `build-fast` est construit sur une autre série de patches (écarté en ce moment, attendu) et `qgpu_smoke.py` attend `QGPU_CAP_CLIENT_ERRORS` (07/10), absent du binaire de référence tant qu'il n'est pas reconstruit ; `qfb_smoke.py`, `qgpu_core_test` (soft + GL), `qgpu_backend_test` verts, `bridge_probe` non construit dans le worktree |
