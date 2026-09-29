# Bug hunt, troisième passe — 29/09/2026

Six relecteurs en lecture seule sur `6c11f37` (correctifs des passes 2 et 2-bis,
docs/bug-hunt-2026-09-29.md), consigne n° 1 : chercher les régressions introduites
par ces correctifs. Puis quatre agents de correction, chacun dans son worktree,
fusionnés sans conflit (merges `b0da922` à `e0b15a9`). **Presque la moitié des
findings sont des correctifs de la passe 2 faux ou incomplets** — d'où l'intérêt
d'une passe de relecture après chaque vague de correctifs.

## Plugin — rendu vers texture (piste Colin McRae ≥ 1024×768)

| # | Gravité | Quoi |
|---|---|---|
| T1 | majeur, **régression P3** | `check_errors` → `invalidate_mirrors()` effaçait `host_only`/`surf_copied` de TOUTES les textures à chaque mouvement d'`ERRORS` — y compris nos propres sondes refusées, les `NO_MEM` et les erreurs d'autres processus. Une texture rendue une fois (reflet, halo) repartait chez Apple (blanche) ou avait son unité coupée jusqu'à la fin de la course. Invalidation désormais ciblée par numéro de soumission (`invalidate_mirrors_from`) ; nos refus sont comptés dans `G.errors` |
| T2 | majeur | relectures jugées sur le statut du lot et non sur ce qui a été exécuté (sync : jetées sur un refus de dessin non fatal ; async : recopiées après une faute fatale) |
| T3 | majeur, **P16 à moitié** | au-delà de l'arène (≥ 1600×1200) `sync_to_host`/`sync_to_sw` se disaient SYNCED sans transférer → transferts par bandes |
| T4 | moyen | fraîcheur d'une texture de surface sans sa source (`surf_src`) |
| T5 | moyen | source disparue → chaque dessin chez Apple ; la dernière image hôte est gardée |
| T6 | mineur | `SURF_TEX` > 2048 fatal côté cœur → refusé côté plugin |
| T7 | mineur | cache `pixtex` survivant à une soumission perdue |

Diagnostics ajoutés : `gl_note` « SURFTEX taille » quand la taille GLEngine diffère de
la surface source ; `POMPPC_GL_VRAM_MB` (16–2048, défaut 64) pour la VRAM annoncée.
Hypothèses « arène pleine à 1024×768 » et « COPY_TEX hors bornes » réfutées par lecture.

## Plugin — concurrence et plafond mémoire

| # | Gravité | Quoi |
|---|---|---|
| PC1 | moyen | `dirty` remis à 0 en fin de `upload_texture` malgré un `NO_MEM` ou une invalidation survenus pendant la boucle → compteur `lost_gen` |
| PC2 | moyen, **régression P3** | tout mouvement d'`ERRORS` retéléversait toutes les textures et resondait tous les programmes (même cause que T1) |
| PC3 | moyen | `NO_MEM` à une sonde traité comme un refus définitif (`DRAW_NATIVE` coupé pour la session, programmes `refused`) |
| PC4 | faible | ligne de base de `nomem_poll` lue trop tard |
| PC5 | faible | `SURF_TEX` ignorait `nomem_until` |

## Cœur qgpu

| # | Gravité | Quoi |
|---|---|---|
| C6 | moyen, **R5 incomplet** | `NO_MEM` sur `PROG_CREATE`/`PROG_STRING`/`GLSL_SOURCE` restait fatal ; programme « cassé » désormais, commandes suivantes en `NO_MEM` non fatal |
| C7 | mineur | mipmaps refusées : `base_format` périmé |
| C8 | test | contrôle « (a) reset » toujours vrai |

## Kext et transport

| # | Gravité | Quoi |
|---|---|---|
| KT1 | majeur, **KG4 incomplet** | `stop()` n'attendait que les dormeurs : un appel déjà passé le test de `fStopping` entrait dans une gate retirée → panic. Compteur d'appels en vol (`enterCall`/`leaveCall`, `fCallers`), user client qui retient son POMPPCGPU |
| KT2 | moyen | kext rechargé avec un client ouvert : objets hôte vivants → `start()` écrit `QGPU_REG_MAGIC` |
| KT3 | mineur | tranche perdue si `clientClose` tombe pendant un `RESET` (`fFreePending`) |
| KT4 | mineur | device « cassé » : CAPS réécrit, BQL tenu 2 s à chaque changement de géométrie |
| KT5 | mineur | sortie de QEMU bloquée par un GPU hôte bloqué (attente bornée à 5 s) |

## Patches QEMU et build

| # | Gravité | Quoi |
|---|---|---|
| Q1 | moyen | correctifs S7 (IRQ OldWorld) et J1 (jit-near) jamais posés sur un arbre déjà patché → marqueurs de version propres |
| Q2 | moyen | `patch_strict` sans `--forward` défaisait un fichier déjà patché |
| Q3–Q5 | faible | screamer : `xfer_status` sans RUN, CMDPTR réécrit pendant la pause, `processing` survivant au reset |
| Q6 | mineur | jit-near + split-wx : alias RX placé dans la fenêtre du texte |
| — | mineur | `build_qemu_qfb.sh` reclonait QEMU par-dessus un worktree (`-d .git`) |

## Frontend et scripts

| # | Gravité | Quoi |
|---|---|---|
| FS1 | moyen | `host_cpu_time` tronqué à la seconde en locale française |
| FS2 | moyen, **F6 incomplet** | `SNAPSHOT=1` supprimait le socket moniteur d'une VM vivante |
| FS3 | mineur, **F10** | le budget de 256 Mio pouvait jeter un Scanout |
| FS4 | mineur, **F2** | étapes clavier de `POMPPC_FE_SCRIPT` ignorées sans focus |
| FS5 | mineur, **F3** | `Via: 1.1 pomppc` envoyé aux serveurs réels |

## Validation

Hôte : `tests/run-all.sh` 156 OK + frontend ctest 2/2 ; `qgpu_core_test` sous
ASan/UBSan ; repros du bug hunt 3 (`run_bh3`) rouges sur `6c11f37`, verts après ;
fuzz mémoire sans erreur. VM : QEMU, kext et plugin de la branche d'intégration
construits et installés (gcc-4.0 dans Tiger), démarrage SMP=2 à 2 CPU, kext v22
chargé. Matrice de jeux sur `4fafc58` : images toutes justes, vitesses non exploitables (voir la passe 4).
