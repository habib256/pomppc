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
